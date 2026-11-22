// hand::view — the terminal and everything drawn over its cells.
//
//   z
//   ├── terminal_view            the grid (cached snapshot), the cursor's motion
//   ├── cursor trail, particles  (cursor_fx)
//   ├── remote_guard / connection_lost   (an ssh tab)
//   ├── copy_flash, copy_cursor, selection_chip
//   ├── live_tail, sticky_command, history_chip   (reading the scrollback)
//   └── scrollback_bar + scrollback_peek          (the minimap)

#include "view.hpp"

#include <mayag/widget/scrollback_bar.hpp>
#include <mayag/widget/terminal/history.hpp>
#include <mayag/widget/terminal/overlays.hpp>
#include <mayag/widget/terminal/remote.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <cmath>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

/// The grid to draw, cached until the screen (or what decorates it) changes.
const TermGrid& snapshot(const Model& m, bool above) {
    const Session& se = m.tab();
    const Config& cfg = m.cfg;
    const bool app_shape = se.term->app_cursor_shape();
    std::uint64_t key = (se.term->epoch() * 8 + (app_shape ? 7 : static_cast<std::uint64_t>(cfg.cursor))) * 2 + (above ? 1 : 0);
    if (se.hint) key ^= std::hash<std::string>{}(se.hint->typed) * 31 + 0x9E37;
    if (!m.preedit.empty()) key ^= std::hash<std::string>{}(m.preedit) * 131 + 0x51ED;
    if (!se.grid || se.grid_key != key) {
        auto g = se.term->snapshot(above);
        if (!app_shape)
            g.cursor_shape = cfg.cursor == CursorShape::beam ? TermCursorShape::beam
                           : cfg.cursor == CursorShape::underline ? TermCursorShape::underscore : TermCursorShape::block;
        if (se.hint) {
            for (std::size_t i = 0; i < se.hint->hints.size(); ++i) {
                const auto& h = se.hint->hints[i];
                g.hints.push_back({TermSpan{h.row, h.col, h.len}, se.hint->labels[i]});
                g.matches.push_back({h.row, h.col, h.len});   // light up what is pickable
            }
            g.hint_typed = se.hint->typed;
        }
        g.preedit = m.preedit;
        se.grid = std::make_shared<const TermGrid>(std::move(g));
        se.grid_key = key;
    }
    return *se.grid;
}

TermTheme term_theme(const Model& m) {
    const Palette& p = m.palette;
    TermTheme th{.fg = p.fg, .bg = p.bg, .cursor = p.cursor, .cursor_text = p.bg, .selection_bg = p.selection,
                 .bold_brighten = m.cfg.bold_is_bright ? 0.22f : 0.0f};
    th.mark_prompt = p.ansi[8]; th.mark_ok = p.ansi[2]; th.mark_error = p.ansi[1]; th.mark_running = p.ansi[4];
    th.match_bg = p.selection; th.hint_bg = p.ansi[3]; th.hint_fg = p.bg; th.link = p.ansi[6];
    return th;
}

/// The cursor's blink as an opacity: hard (on/off), fade, or a slow breath.
float cursor_alpha(const Model& m) {
    const Session& se = m.tab();
    const Config& cfg = m.cfg;
    if (cfg.blink_style == BlinkStyle::fade) return se.cur_a;
    if (cfg.blink_style == BlinkStyle::breathe && cfg.cursor_blink && m.focused && m.time - se.active_at > 0.6) {
        const double period = 2.0 * static_cast<double>(cfg.blink_ms) / 1000.0 / static_cast<double>(cfg.motion_speed);
        return 0.62f + 0.38f * static_cast<float>(std::cos(6.2831853 * (m.time - se.active_at - 0.6) / period));
    }
    return 1.0f;
}

TermViewOptions term_options(const Model& m, float frac, bool above) {
    const Session& se = m.tab();
    const Config& cfg = m.cfg;
    const float bell = se.bell_at >= 0.0 && cfg.visual_bell ? static_cast<float>(std::max(0.0, 1.0 - (m.time - se.bell_at) / 0.2)) : 0.0f;
    TermViewOptions o{.font_size = m.font_size, .line_height = cfg.line_height, .padding = cfg.padding,
                      .focused = m.focused,
                      .cursor_on = cfg.blink_style != BlinkStyle::hard ? true : (m.cursor_on || !m.focused || !cfg.cursor_blink),
                      .dim_unfocused = m.focused ? 0.0f : cfg.dim_unfocused, .bell = bell, .marks = cfg.shell_marks,
                      .opacity = 0.0f, .family = family_of(cfg)};   // the window clear is the paper
    if (cfg.cursor_anim != CursorAnim::none && cfg.cursor_anim != CursorAnim::smear && se.cur_known) {
        const auto cur = se.term->cursor();
        o.cursor_dx = se.cur_x - static_cast<float>(cur.col);
        o.cursor_dy = se.cur_y - static_cast<float>(cur.row);
        o.cursor_trail = false;   // the trail is cursor_fx's, drawn over it
    }
    o.cursor_glow = cfg.cursor_glow ? 1.0f : 0.0f;
    o.y_offset = above ? (frac - 1.0f) * m.metrics.cell_h : 0.0f;   // row 0 is the one above: start a row up, slide down by frac
    o.cursor_alpha = std::max(cursor_alpha(m), (o.cursor_dx != 0.0f || o.cursor_dy != 0.0f) ? 1.0f : 0.0f);
    return o;
}

TermBox box_of(const Model& m, const TermViewOptions& o) {
    return TermBox{.size = m.size, .origin_x = m.metrics.origin_x, .origin_y = m.metrics.origin_y,
                   .cell_w = m.metrics.cell_w, .cell_h = m.metrics.cell_h, .y_offset = o.y_offset,
                   .font_size = m.font_size, .padding = m.cfg.padding, .right_inset = minimap_w(m), .family = o.family};
}

CommandState state_of(const Term::Command& c) {
    if (!c.ran) return CommandState::none;
    if (!c.finished) return CommandState::running;
    return c.exit_code == 0 ? CommandState::ok : CommandState::failed;
}

void push_all(std::vector<Node>& to, std::vector<Node>&& from) {
    for (auto& n : from) to.push_back(std::move(n));
}

// ── branches ─────────────────────────────────────────────────────────────

void cursor_motion(std::vector<Node>& layers, const Model& m) {
    const Session& se = m.tab();
    const Config& cfg = m.cfg;
    if (cfg.cursor_anim != CursorAnim::none && cfg.cursor_trail > 0.0f && se.cur_known && m.focused &&
        se.term->scroll_offset() == 0 && se.term->cursor().visible) {
        const auto cur = se.term->cursor();
        const float cw = m.metrics.cell_w, ch = m.metrics.cell_h, ox = m.metrics.origin_x, oy = m.metrics.origin_y;
        const float tailx = ox + se.fx.tx * cw, taily = oy + se.fx.ty * ch;
        push_all(layers, cfg.cursor_anim == CursorAnim::smear
                             ? motion::smear(tailx, taily, ox + static_cast<float>(cur.col) * cw, oy + static_cast<float>(cur.row) * ch, cw, ch, m.palette.cursor)
                             : motion::comet(ox + se.cur_x * cw, oy + se.cur_y * ch, tailx, taily, cw, ch, m.palette.cursor,
                                             0.55f + 0.15f * cfg.cursor_trail));
    }
    push_all(layers, motion::draw(se.fx));
}

void remote(std::vector<Node>& layers, const Ctx& c, const Model& m, const TermBox& b) {
    const auto& r = remote_of(m.tab());
    if (!r) return;
    if (m.tab().link.dropped) {
        layers.push_back(connection_lost(c, "ssh", b, {.alias = r->alias, .command = ssh_command(*r),
                                                       .gone = m.time - m.tab().link.dropped_at, .motion = m.cfg.motion}));
    } else if (m.cfg.ssh_prod_guard && remote_env(m, *r) == HostEnv::prod) {
        push_all(layers, remote_guard(c, "ssh.prod", b, {.who = (r->user.empty() ? "" : r->user + "@") + r->alias}));
    }
}

void selection(std::vector<Node>& layers, const Ctx& c, const Model& m, const TermBox& b, bool above, float tail_h) {
    const Session& se = m.tab();
    const Config& cfg = m.cfg;
    if (cfg.sel_flash && m.time - se.copy_flash_at < 0.45 && se.grid)
        push_all(layers, copy_flash(c, b, se.grid->selection, 1.0f - static_cast<float>((m.time - se.copy_flash_at) / 0.45)));
    const std::int64_t top_abs = se.term->view_to_abs(0);
    const auto row_of = [&](std::int64_t abs) { return static_cast<float>(abs - top_abs) + (above ? 1.0f : 0.0f); };
    const auto si = se.term->selection_info();
    if (se.copy) {
        push_all(layers, copy_cursor(c, b, {.row = row_of(se.copy->row), .col = se.copy->col, .line = static_cast<int>(se.copy->row),
                                            .visual = static_cast<int>(se.copy->visual), .sel_lines = si.lines, .sel_chars = static_cast<int>(si.chars),
                                            .lift = tail_h}));
    }
    if (cfg.sel_toolbar && si.any && si.chars > 0 && !se.selecting && !se.copy && !se.term->alt_screen()) {
        const std::string text = si.lines == 1 ? se.term->selection_text() : std::string{};
        const bool openable = !text.empty() && text.find(' ') == std::string::npos &&
                              (text.find("://") != std::string::npos || text.rfind("www.", 0) == 0 || text[0] == '/' || text[0] == '~');
        const auto end_line = se.term->line(si.r1);
        int used = static_cast<int>(end_line.size());
        while (used > 0 && (end_line[static_cast<std::size_t>(used - 1)] == U' ' || end_line[static_cast<std::size_t>(used - 1)] == U'\0')) --used;
        Node chip = selection_chip(c, "sel", b, {.lines = si.lines, .chars = static_cast<int>(si.chars), .block = si.block, .openable = openable,
                                                 .c0 = si.c0, .c1 = si.c1, .row0 = row_of(si.r0), .row1 = row_of(si.r1), .end_used = used});
        if (chip.style().id != 0) layers.push_back(std::move(chip));   // off screen: an empty node
    }
}

/// Reading history: the live rows pinned below, the command's header on top,
/// a chip saying where you are. Returns the tail's height (things sit above it).
float history(std::vector<Node>& layers, const Ctx& c, const Model& m, const TermBox& b, const TermViewOptions& o) {
    const Session& se = m.tab();
    const Config& cfg = m.cfg;
    float tail_h = 0.0f;
    if (se.tail_t > 0.0f) {
        const int n = std::max(static_cast<int>(cfg.live_tail), 1);
        const TermGrid tg = se.term->grid_at(se.term->cursor_abs() - n + 1, n);
        auto tail = live_tail(c, "tail", b, {.grid = &tg, .theme = term_theme(m), .view = o, .rows = n, .ease = se.tail_t,
                                            .fresh = se.new_below}, &tail_h);
        if (!tail.empty())
            tail.front() = (node(std::move(tail.front())) | on_click([] { return App::Msg{App::ScrollLive{}}; })).build();
        push_all(layers, std::move(tail));
    }
    const bool back = se.scroll_d > 0.01f || se.term->scroll_offset() > 0;
    if (back && se.sticky_t > 0.0f)
        if (const auto cmd = sticky_of(m)) {
            const auto info = se.term->scroll_info();
            const std::int64_t out0 = cmd->output_row >= 0 ? cmd->output_row : cmd->prompt_row + 1;
            const std::int64_t out1 = cmd->finished && cmd->end_row > out0 ? cmd->end_row : static_cast<std::int64_t>(info.total);
            const std::int64_t span = std::max<std::int64_t>(out1 - out0, 1);
            const std::string time = !cmd->finished ? (cmd->start_ms > 0 ? pretty_ms(std::max<std::int64_t>(m.wall_ms - cmd->start_ms, 0)) : std::string{})
                                                    : (cmd->duration_ms > 0 ? pretty_ms(cmd->duration_ms) : std::string{});
            layers.push_back((node(sticky_command(c, "sticky", b,
                                                  {.line = se.term->command_text(*cmd), .cwd = short_path(m, cmd->cwd), .state = state_of(*cmd),
                                                   .exit_code = cmd->exit_code, .time = time,
                                                   .at = std::clamp<std::int64_t>(info.first - out0 + 1, 1, span), .span = span,
                                                   .now = m.time, .ease = se.sticky_t, .marks = cfg.shell_marks}))
                              | on_click([prompt = cmd->prompt_row] {
                                    return App::Msg{App::ScrollToPrompt{prompt}};   // fly to its prompt
                                })).build());
        }
    if (back && !se.term->alt_screen()) {
        const auto info = se.term->scroll_info();
        const int pct = info.total > info.visible ? static_cast<int>(100.0 * info.first / (info.total - info.visible)) : 100;
        layers.push_back((node(history_chip(c, "tolive", b, {.rows_up = static_cast<int>(std::lround(se.scroll_d)), .percent = pct,
                                                            .fresh = se.new_below, .lift = tail_h}))
                          | on_click([] { return App::Msg{App::ScrollLive{}}; })).build());
    }
    return tail_h;
}

/// The minimap: the whole scrollback as a strip -- prompts, failures, search
/// hits -- and, hovering it, a peek at what is there.
void minimap(std::vector<Node>& layers, const Ctx& c, const Model& m, const TermBox& b, const TermViewOptions& o) {
    const Session& se = m.tab();
    if (!minimap_on(m)) return;
    const auto info = se.term->scroll_info();
    ScrollbackInfo sb{.total = info.total, .first = info.first, .visible = info.visible};
    const auto cmds = se.term->commands();
    for (const auto& cmd : cmds)
        if (cmd.prompt_row >= 0 && cmd.ran)
            sb.marks.push_back({static_cast<int>(cmd.prompt_row),
                                cmd.finished && cmd.exit_code != 0 ? ScrollbackMark::Kind::error : ScrollbackMark::Kind::prompt});
    const auto rows = se.term->search_rows();
    for (std::size_t i = 0; i < rows.size(); ++i)
        sb.marks.push_back({static_cast<int>(rows[i]), i + 1 == se.term->search_current() ? ScrollbackMark::Kind::current_match
                                                                                         : ScrollbackMark::Kind::match});
    for (const auto& cmd : cmds) {   // command spans: a status stripe
        if (!cmd.ran || cmd.prompt_row < 0) continue;
        const int last = cmd.finished && cmd.end_row > cmd.prompt_row ? static_cast<int>(cmd.end_row) - 1 : info.total - 1;
        sb.spans.push_back({static_cast<int>(cmd.prompt_row), last,
                            !cmd.finished ? ScrollbackSpan::Kind::running
                            : cmd.exit_code != 0 ? ScrollbackSpan::Kind::error : ScrollbackSpan::Kind::ok});
    }
    const float hgt = m.size.y;
    sb.rest_width = kMinimapRest;
    sb.expanded = m.minimap.width > kMinimapRest + 0.5f;
    sb.width = std::max(m.minimap.width, kMinimapRest);
    sb.line_px = 2.0f;
    const float used = std::min(hgt, scrollback_scale(sb, hgt) * static_cast<float>(std::max(info.total, 1)));
    sb.buckets = scrollback_buckets(info.total, used, 2.0f);
    sb.runs = se.term->minimap(sb.buckets);
    sb.runs_version = se.term->minimap_version();
    layers.push_back((node(scrollback_dock(c, "minimap", sb, m.size))
                      | on_slide([sb, hgt](Vec2 at) -> App::Msg {
                            const int line = scrollback_line_at(sb, at.y, hgt);
                            return App::ScrollToLine{line};
                        })).build());

    if (m.minimap.peek_t <= 0.0f || m.minimap.hover_y < 0.0f || info.total <= 0) return;
    const std::int64_t line = std::clamp<std::int64_t>(static_cast<std::int64_t>(m.minimap.hover_y / hgt * static_cast<float>(info.total)),
                                                     0, info.total - 1);
    std::optional<Term::Command> at;
    for (const auto& cmd : cmds) {
        if (cmd.prompt_row < 0 || cmd.prompt_row > line) continue;
        const std::int64_t end = cmd.finished && cmd.end_row > cmd.prompt_row ? cmd.end_row : info.total;
        if (line < end) at = cmd;
    }
    constexpr int kRows = 7;
    const std::int64_t first = std::clamp<std::int64_t>(line - kRows / 2, 0, std::max<std::int64_t>(info.total - kRows, 0));
    const int pcols = std::min(se.term->cols(), 72);
    PeekProps pp{.grid = se.term->grid_at(first, kRows, pcols), .theme = term_theme(m), .view = o, .rows = kRows, .cols = pcols,
                 .line = line, .total = info.total, .hover_y = m.minimap.hover_y, .ease = m.minimap.peek_t};
    pp.grid.selection.push_back({static_cast<int>(line - first), 0, pcols});   // the hovered line, highlighted
    if (at && at->ran) {
        pp.command = se.term->command_text(*at);
        pp.state = state_of(*at);
        pp.exit_code = at->exit_code;
        pp.duration_ms = at->duration_ms;
    }
    push_all(layers, scrollback_peek(c, "peek", b, pp));
}

}  // namespace

Node terminal(const Ctx& c, const Model& m) {
    const Session& se = m.tab();
    const float frac = draw_frac(m);
    const bool above = frac > 0.001f;
    const TermGrid& grid = snapshot(m, above);
    const TermViewOptions o = term_options(m, frac, above);
    const TermBox b = box_of(m, o);
    const auto shape = !se.hover_url.empty() ? mayag::CursorShape::pointer
                     : se.term->mouse_reporting() ? mayag::CursorShape::arrow : mayag::CursorShape::text;
    Node grid_node = (node(terminal_view(c, "term", grid, term_theme(m), o)) | size(m.size.x, m.size.y) | cursor(shape)).build();
    std::vector<Node> layers;
    layers.push_back(m.cfg.bell_shake && se.bell_at >= 0.0 ? bell_shake(std::move(grid_node), static_cast<float>(m.time - se.bell_at))
                                                          : std::move(grid_node));
    if (!m.settings.open) remote(layers, c, m, b);
    cursor_motion(layers, m);
    std::vector<Node> history_layers;
    const float tail_h = history(history_layers, c, m, b, o);
    selection(layers, c, m, b, above, tail_h);
    push_all(layers, std::move(history_layers));
    minimap(layers, c, m, b, o);
    return (z(std::move(layers)) | size(m.size.x, m.size.y)).build();
}

}  // namespace hand::view
