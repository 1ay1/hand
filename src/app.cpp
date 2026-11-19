// hand::App — update, input, settings, view.

#include <hand/app.hpp>

#include <hand/shell.hpp>

#include <mayag/widget/hud.hpp>
#include <mayag/widget/resize_hud.hpp>
#include <mayag/widget/scrollback_bar.hpp>
#include <mayag/widget/term_search.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <stop_token>
#include <thread>
#include <utility>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using E = Edit<App::Model>;

namespace {

constexpr float kMinimapRest = 14.0f, kMinimapOpen = 96.0f;

/// The minimap maps the normal screen's history: a full-screen app (the
/// alternate screen: vim, htop, cmatrix) owns the whole window and gets none.
/// The command whose output is under the top of the view, when the sticky header applies.
std::optional<Term::Command> sticky_of(const App::Model& m) {
    if (!m.term || !m.cfg.shell_marks || m.term->alt_screen()) return std::nullopt;
    if (!(m.scroll_d > 0.01f || m.term->scroll_offset() > 0)) return std::nullopt;
    return m.term->command_at_top();
}

std::int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// "12.3s", "4m05s", "1h02m"
std::string pretty_ms(std::int64_t ms) {
    char b[32];
    const double s = static_cast<double>(ms) / 1000.0;
    if (s < 60.0) std::snprintf(b, sizeof b, "%.1fs", s);
    else if (s < 3600.0) std::snprintf(b, sizeof b, "%dm%02ds", static_cast<int>(s / 60), static_cast<int>(s) % 60);
    else std::snprintf(b, sizeof b, "%dh%02dm", static_cast<int>(s / 3600), static_cast<int>(s / 60) % 60);
    return b;
}

/// A path for a header: $HOME as ~, long ones keep their last two parts.
std::string short_path(std::string p) {
    if (const char* h = std::getenv("HOME"); h && *h && p.rfind(h, 0) == 0) p = "~" + p.substr(std::strlen(h));
    int slashes = 0;
    for (char ch : p) slashes += ch == '/';
    if (p.size() > 28 && slashes > 2) {
        const auto b = p.rfind('/', p.rfind('/') - 1);
        p = "\u2026" + p.substr(b);
    }
    return p;
}

bool minimap_on(const App::Model& m) { return m.cfg.minimap && m.term && !m.term->alt_screen(); }

/// The live tail shows while you read history and the live prompt is off screen.
bool tail_wanted(const App::Model& m) {
    if (!m.term || m.cfg.live_tail < 1.0f || m.term->alt_screen()) return false;
    const auto cur = m.term->cursor();
    return m.term->scroll_offset() > 0 && cur.row + m.term->scroll_offset() >= m.term->rows();
}

/// The pty reader: a background stream for as long as the shell lives.
/// Coalesces bursts (cat of a big file) into one Output per wakeup.
void read_pty(Sink<App::Msg> out, std::stop_token st, int fd, int pid) {
    std::string buf;
    while (!st.stop_requested()) {
        const bool alive = pty_read(fd, buf, 50);   // 50 ms poll: stop is always prompt
        if (!buf.empty()) { out.send(App::Output{std::move(buf)}); buf.clear(); }
        if (!alive) {
            int code = -1;
            for (int i = 0; i < 20 && code < 0; ++i) {   // bounded: ≤ ~200 ms
                code = pty_reap(static_cast<pid_t>(pid));
                if (code < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out.send(App::Exited{std::max(code, 0)});
            return;
        }
    }
}

/// The viewport row under window y, through a smooth-scroll glide (the grid is
/// drawn up to a row lower mid-glide). May be -1: the row just above the view.
int view_row(const App::Model& m, float y) {
    const float frac = m.cfg.smooth_scroll ? m.scroll_d - static_cast<float>(m.scroll_applied) : 0.0f;
    const float r = (y - m.metrics.origin_y) / std::max(m.metrics.cell_h, 1.0f) - (frac > 0.001f ? frac : 0.0f);
    const int lo = frac > 0.001f ? -1 : 0;
    return std::clamp(static_cast<int>(std::floor(r)), lo, std::max(m.term ? m.term->rows() - 1 : 0, 0));
}

/// Selecting into the top/bottom margin scrolls; the further out, the faster.
float edge_speed(const App::Model& m, float y) {
    const float ch = std::max(m.metrics.cell_h, 1.0f);
    const float top = m.metrics.origin_y + ch * 0.5f, bottom = m.size.y - m.metrics.origin_y - ch * 0.5f;
    if (y < top) return 6.0f + (top - y) / ch * 18.0f;
    if (y > bottom) return -(6.0f + (y - bottom) / ch * 18.0f);
    return 0.0f;
}

int cell_of(float v, float origin, float size, int max) {
    return std::clamp(static_cast<int>(std::floor((v - origin) / std::max(size, 1.0f))), 0, std::max(max - 1, 0));
}

FontFamily family_of(const Config& c) {
    return font_names().empty() ? FontFamily::mono
                                : custom_family(static_cast<std::size_t>(std::clamp(c.font_family, 0, static_cast<int>(font_names().size()) - 1)));
}

}  // namespace

// ── geometry + config ─────────────────────────────────────────────────────

void App::fit(Model& m) {
    const float cw = cell_width(m.font_size, m.cfg.font_family);
    const float ch = std::round(m.font_size * m.cfg.line_height);
    const float pad = m.cfg.padding;
    const float gutter = m.cfg.shell_marks ? 10.0f : 0.0f;   // terminal_view's mark margin
    m.metrics.cell_w = cw;
    m.metrics.cell_h = ch;
    m.metrics.origin_x = pad + gutter;
    m.metrics.origin_y = pad;
    const int cols = std::max(2, static_cast<int>((m.size.x - 2.0f * pad - gutter) / cw));
    const int rows = std::max(1, static_cast<int>((m.size.y - 2.0f * pad) / ch));
    m.metrics.fit_cols = cols;
    m.metrics.fit_rows = rows;
    if (!m.term || (cols == m.term->cols() && rows == m.term->rows())) return;
    m.term->resize(cols, rows);
    if (m.pty) m.pty->resize(cols, rows, static_cast<int>(m.size.x), static_cast<int>(m.size.y));
}

void App::apply(Model& m, const Config& before) {
    const Config& c = m.cfg;
    if (c.font_size != before.font_size) m.font_size = c.font_size;
    if (c.scheme != before.scheme) { m.palette = palette_of(c); m.term->set_palette(m.palette); }
    if (c.scrollback != before.scrollback) m.term->set_scrollback(static_cast<int>(c.scrollback));
    if (!c.cursor_blink) m.cursor_on = true;
    fit(m);   // font family / size / line height / padding
    m.grid_key = 0;
}

void App::scroll_by(Model& m, float rows) {
    if (!m.term) return;
    const float top = static_cast<float>(m.term->history());
    m.scroll_goal = std::clamp(m.scroll_goal + rows, 0.0f, top);
    if (!m.cfg.smooth_scroll) {
        m.scroll_goal = std::round(m.scroll_goal);
        m.scroll_d = m.scroll_goal;
        m.term->set_scroll_offset(m.scroll_applied = static_cast<int>(m.scroll_goal));
    }
    if (m.scroll_goal == 0.0f) m.new_below = 0;
}

void App::settle_scroll(Model& m) {
    if (!m.term) return;
    const int now = m.term->scroll_offset();
    if (now == m.scroll_applied) return;
    m.scroll_goal = static_cast<float>(now);           // fly there...
    if (!m.cfg.smooth_scroll || m.term->alt_screen()) {
        m.scroll_d = m.scroll_goal;
        m.scroll_applied = now;
        return;
    }
    m.scroll_d = std::clamp(m.scroll_d, 0.0f, static_cast<float>(m.term->history()));
    m.scroll_applied = static_cast<int>(std::floor(m.scroll_d));   // ...from where we are
    m.term->set_scroll_offset(m.scroll_applied);
    if (now == 0) m.new_below = 0;
}

App::Cmd App::init(Model& m) {
    m.cfg = config;
    m.palette = palette_of(m.cfg);
    m.font_size = m.cfg.font_size;
    fit(m);
    m.term = std::make_shared<Term>(m.metrics.fit_cols, m.metrics.fit_rows, m.palette, static_cast<int>(m.cfg.scrollback));
    if (m.cfg.demo) return SetTitle{"hand"};
    m.pty = std::make_shared<Pty>();
    const std::string shell = Pty::resolve_shell(m.cfg.shell_cli.empty() ? m.cfg.shell : m.cfg.shell_cli);
    const ShellLaunch si = m.cfg.shell_integration ? shell_integration(shell) : ShellLaunch{};
    if (!m.pty->spawn(m.metrics.fit_cols, m.metrics.fit_rows, shell, si.env, si.args)) {
        (void)m.term->feed("\x1b[31mhand: could not start a shell\x1b[0m\r\n");
        m.pty.reset();
    }
    return SetTitle{"hand"};
}

// ── updates ────────────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, Output o) {
    const int off0 = m.term->scroll_offset(), hist0 = m.term->history();
    const Effects fx = m.term->feed(o.bytes);
    {   // toe keeps a scrolled-back view still by bumping its offset: follow it
        const int off1 = m.term->scroll_offset();
        if (off0 > 0 && off1 != off0) {
            const float d = static_cast<float>(off1 - off0);
            m.scroll_d += d; m.scroll_goal += d;
            m.scroll_applied = off1;
        } else if (off1 == 0 && off0 == 0) {
            const int grew = std::max(m.term->history() - hist0, 0);
            if (m.scroll_goal > 0.0f && !m.term->alt_screen()) {
                // A glide up that has not reached its first whole row yet: toe is
                // still following the bottom, so anchor to the CONTENT the user is
                // moving toward -- otherwise streaming output resets every frame and
                // a trackpad can never leave the live view.
                const float top = static_cast<float>(m.term->history());
                m.scroll_d = std::min(m.scroll_d + static_cast<float>(grew), top);
                m.scroll_goal = std::min(m.scroll_goal + static_cast<float>(grew), top);
                m.scroll_applied = static_cast<int>(std::floor(m.scroll_d));
                m.term->set_scroll_offset(m.scroll_applied);
                m.new_below += grew;
            } else {
                m.scroll_d = m.scroll_goal = 0.0f;
                m.scroll_applied = 0;
            }
        }
        if (off1 > 0) m.new_below += std::max(m.term->history() - hist0, 0);
    }
    if (!fx.reply.empty() && m.pty) m.pty->write(fx.reply);
    if (fx.bell && m.cfg.visual_bell) m.bell_at = m.time;
    m.cursor_on = true;   // output resets the blink, like foot
    std::vector<Cmd> cmds;
    if (fx.title_set) { m.title = fx.title.empty() ? "hand" : fx.title; cmds.push_back(SetTitle{m.title}); }
    if (fx.clipboard_set) cmds.push_back(WriteClipboard{fx.clipboard});
    // shell integration: a long command finished while you were away -> tell you
    for (const auto& c : m.term->commands()) {
        if (!c.finished || !c.ran || c.id <= m.notified) continue;
        m.notified = c.id;
        const double secs = static_cast<double>(c.duration_ms) / 1000.0;
        if (m.cfg.notify_after <= 0.0f || secs < m.cfg.notify_after) continue;
        std::string cmd = m.term->command_text(c);
        if (cmd.size() > 80) cmd = cmd.substr(0, 77) + "...";
        char took[32];
        std::snprintf(took, sizeof took, secs < 60 ? "%.1fs" : "%.0fm%02.0fs", secs < 60 ? secs : std::floor(secs / 60.0),
                      std::fmod(secs, 60.0));
        const std::string title = c.exit_code == 0 ? "\u2713 done in " + std::string{took}
                                                   : "\u2717 failed (" + std::to_string(c.exit_code) + ") after " + took;
        if (!m.focused && !m.cfg.demo)
            spawn_detached({"notify-send", "-a", "hand", "-u", c.exit_code == 0 ? "normal" : "critical", title,
                            cmd.empty() ? "command finished" : cmd});
        m.toast = Toast{.title = title, .body = cmd, .kind = c.exit_code == 0 ? Toast::Kind::success : Toast::Kind::error};
        m.toast_at = m.time;
    }
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, Exited) {
    if (!m.cfg.demo) (void)config_save(m.cfg, config_path());
    return Cmd::quit();   // foot closes with its shell
}

App::Cmd App::update(Model& m, Blink) { m.cursor_on = !m.cursor_on || !m.cfg.cursor_blink; return {}; }
App::Cmd App::update(Model& m, Tick t) {
    m.time += t.dt;
    if (m.selecting && m.sel_edge != 0.0f) {   // drag-select past an edge: history scrolls under the pointer
        scroll_by(m, m.sel_edge * static_cast<float>(t.dt));
        const int col = cell_of(m.sel_pos.x + m.metrics.cell_w * 0.5f, m.metrics.origin_x, m.metrics.cell_w, m.term->cols() + 1);
        m.term->select_extend(col, view_row(m, m.sel_pos.y));
    }
    {   // the peek card and the live tail fade / slide in and out
        const auto ease = [&](float& v, bool on, float rate) {
            v += ((on ? 1.0f : 0.0f) - v) * (1.0f - std::exp(-static_cast<float>(t.dt) * rate));
            if (std::fabs(v - (on ? 1.0f : 0.0f)) < 0.01f) v = on ? 1.0f : 0.0f;
        };
        ease(m.peek_t, m.cfg.peek && m.mm_hover_y >= 0.0f, 24.0f);
        ease(m.tail_t, tail_wanted(m), 16.0f);
        const auto st = sticky_of(m);
        if (st && st->id != m.sticky_id) { m.sticky_id = st->id; m.sticky_t = std::min(m.sticky_t, 0.35f); }   // a new command: re-slide
        ease(m.sticky_t, st.has_value(), 20.0f);
    }
    {   // the minimap slides open / shut
        const float goal = m.mm_open ? kMinimapOpen : kMinimapRest;
        m.mm_w += (goal - m.mm_w) * (1.0f - std::exp(-static_cast<float>(t.dt) * 22.0f));
        if (std::fabs(goal - m.mm_w) < 0.3f) m.mm_w = goal;
    }
    if (m.scroll_d != m.scroll_goal) {   // critically damped glide, ~120 ms, frame-rate independent
        const float k = 1.0f - std::exp(-static_cast<float>(t.dt) * 18.0f);
        m.scroll_d += (m.scroll_goal - m.scroll_d) * k;
        if (std::fabs(m.scroll_goal - m.scroll_d) < 0.004f) m.scroll_d = m.scroll_goal;
        const int whole = std::clamp(static_cast<int>(std::floor(m.scroll_d)), 0, m.term->history());
        if (whole != m.scroll_applied) { m.term->set_scroll_offset(whole); m.scroll_applied = whole; }
        if (m.scroll_d == 0.0f) m.new_below = 0;
    }
    const auto cur = m.term->cursor();
    const float tx = static_cast<float>(cur.col), ty = static_cast<float>(cur.row);
    if (!m.cfg.cursor_smooth || !m.cur_known) { m.cur_x = tx; m.cur_y = ty; m.cur_known = true; return {}; }
    const float k = 1.0f - std::exp(-static_cast<float>(t.dt) * 26.0f);   // ~90 ms to settle, frame-rate independent
    m.cur_x += (tx - m.cur_x) * k;
    m.cur_y += (ty - m.cur_y) * k;
    if (std::fabs(tx - m.cur_x) < 0.01f) m.cur_x = tx;
    if (std::fabs(ty - m.cur_y) < 0.01f) m.cur_y = ty;
    return {};
}

App::Cmd App::update(Model& m, HintPick h) {
    m.hinting = false;
    m.hint_typed.clear();
    m.grid_key = 0;
    if (h.i >= m.hints.size()) return {};
    const Term::Hint hint = m.hints[h.i];
    m.hints.clear();
    if (h.insert) {   // type it at the prompt
        if (m.pty) m.pty->write(m.term->encode_paste(hint.text));
        return {};
    }
    if (hint.kind == Term::Hint::Kind::url) return update(m, OpenUrl{hint.text});
    m.toast = Toast{.title = "copied", .body = hint.text, .kind = Toast::Kind::info};
    m.toast_at = m.time;
    return WriteClipboard{hint.text};
}

App::Cmd App::update(Model& m, CopyOutput) {
    const auto cs = m.term->commands();
    for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
        if (!it->ran || !it->finished) continue;
        const std::string out = m.term->output_text(*it);
        m.toast = Toast{.title = "copied the output of", .body = m.term->command_text(*it), .kind = Toast::Kind::info};
        m.toast_at = m.time;
        return WriteClipboard{out};
    }
    m.toast = Toast{.title = "no command output yet", .body = "needs shell integration (settings \u203A shell)", .kind = Toast::Kind::warning};
    m.toast_at = m.time;
    return {};
}

App::Cmd App::update(Model& m, Copy) {
    m.selecting = false;
    m.sel_edge = 0.0f;
    if (!m.term->has_selection()) return {};
    return WriteClipboard{m.term->selection_text()};
}
App::Cmd App::update(Model&, PasteReq) {
    return ReadClipboard<Msg>{[](std::string s) -> Msg { return Pasted{std::move(s)}; }};
}
App::Cmd App::update(Model& m, Pasted p) {
    if (!m.pty || p.text.empty()) return {};
    m.term->select_clear();
    m.term->scroll_to_bottom();
    m.pty->write(m.term->encode_paste(p.text));
    settle_scroll(m);
    return {};
}

App::Cmd App::update(Model& m, Search s) {
    if (s.op == 0) {
        m.search_open = !m.search_open;
        if (!m.search_open) { m.term->search_clear(); m.search_query.clear(); }
        else if (m.term->has_selection()) {   // seed with the selection, like every editor
            const std::string sel = m.term->selection_text();
            if (!sel.empty() && sel.find('\n') == std::string::npos) { m.search_query.set(sel); (void)m.term->search(sel); }
        }
        return {};
    }
    if (s.op > 0) m.term->search_next(); else m.term->search_prev();
    settle_scroll(m);   // fly to the match
    return {};
}

App::Cmd App::update(Model& m, OpenUrl u) {
    if (u.url.empty()) return {};
    std::string url = u.url;
    if (url.rfind("www.", 0) == 0) url = "https://" + url;
    if (!m.cfg.demo) spawn_detached({m.cfg.url_launcher, url});
    return {};
}

App::Cmd App::update(Model& m, ToggleSettings) {
    m.settings_open = !m.settings_open;
    m.panel.filtering = false;
    if (m.panel.menu >= 0) dsl::settings_ops::close_menu(m.panel, true);
    for (auto& e : std::exchange(m.panel.out, {})) { const Config before = m.cfg; if (setting_apply(m.cfg, e)) apply(m, before); }
    if (!m.settings_open && !m.cfg.demo) (void)config_save(m.cfg, config_path());   // closing is "done"
    return {};
}

App::Cmd App::update(Model& m, Panel p) {
    p.op(m.panel);
    for (auto& e : std::exchange(m.panel.out, {})) {
        const Config before = m.cfg;
        if (setting_apply(m.cfg, e)) apply(m, before);
    }
    if (std::exchange(m.panel.close, false)) return update(m, ToggleSettings{});
    return {};
}

// ── input ──────────────────────────────────────────────────────────────────

namespace {
std::optional<App::Msg> send(std::string bytes) {
    if (bytes.empty()) return std::nullopt;
    return E{[b = std::move(bytes)](App::Model& mm) {
        if (!mm.pty) return;
        mm.term->select_clear();
        mm.term->scroll_to_bottom();   // typing snaps back to the live screen
        mm.scroll_d = mm.scroll_goal = 0.0f;
        mm.scroll_applied = 0;
        mm.new_below = 0;
        mm.cursor_on = true;
        mm.pty->write(b);
    }};
}

}  // namespace

std::optional<App::Msg> App::on(const Model& m, const KeyEvent& k) {
    const Mods md = k.mods;
    if (md.ctrl && !md.shift && !md.alt && k.key == Key::comma) return ToggleSettings{};
    if (m.settings_open) {
        if (auto op = settings_panel_key(m.panel, setting_specs(m.cfg), k.key, md)) return Panel{std::move(*op)};
        return std::nullopt;
    }
    const bool cs = md.ctrl && md.shift && !md.alt;
    if (k.key == Key::ctrl && !m.ctrl_held) return E{[](Model& mm) { mm.ctrl_held = true; }};
    if (m.hinting) {
        if (k.key == Key::escape || (cs && k.key == Key::space))
            return E{[](Model& mm) { mm.hinting = false; mm.hints.clear(); mm.hint_typed.clear(); mm.grid_key = 0; }};
        if (k.key == Key::backspace)
            return E{[](Model& mm) { if (!mm.hint_typed.empty()) mm.hint_typed.pop_back(); mm.grid_key = 0; }};
        return std::nullopt;   // letters arrive as text
    }
    if (cs && k.key == Key::space)
        return E{[](Model& mm) {
            mm.hints = mm.term->hints();
            mm.hint_labels.clear();
            // home row first, then the rest; two letters once those run out
            static constexpr std::string_view keys = "asdfjklghqwertyuiopzxcvbnm";
            const std::size_t n = mm.hints.size();
            for (std::size_t i = 0; i < n; ++i)
                mm.hint_labels.push_back(n <= keys.size() ? std::string(1, keys[i])
                                                          : std::string{keys[i / keys.size() % keys.size()], keys[i % keys.size()]});
            mm.hinting = !mm.hints.empty();
            mm.hint_typed.clear();
            mm.grid_key = 0;
            if (!mm.hinting) { mm.toast = Toast{.title = "nothing to pick", .body = "no links, paths, hashes or IPs on screen"}; mm.toast_at = mm.time; }
        }};
    if (cs && (k.key == Key::z || k.key == Key::x)) {   // foot's prompt-prev / prompt-next
        const int d = k.key == Key::z ? -1 : 1;
        return E{[d](Model& mm) {
            // from where the scroll is HEADING, so quick presses chain prompt to prompt
            mm.term->set_scroll_offset(static_cast<int>(std::lround(mm.scroll_goal)));
            mm.scroll_applied = mm.term->scroll_offset();
            (void)mm.term->jump_prompt(d);
            const int to = mm.term->scroll_offset();
            mm.term->set_scroll_offset(static_cast<int>(std::floor(mm.scroll_d)));
            mm.scroll_applied = mm.term->scroll_offset();
            scroll_by(mm, static_cast<float>(to) - mm.scroll_goal);
        }};
    }
    if (cs && k.key == Key::g) return CopyOutput{};
    if (cs && k.key == Key::f) return Search{0};
    if (m.search_open) {
        if (k.key == Key::escape) return Search{0};
        if (k.key == Key::enter || k.key == Key::f3) return Search{md.shift ? -1 : 1};
        if (k.key == Key::up) return Search{-1};
        if (k.key == Key::down) return Search{1};
        return E{[k](Model& mm) {
            if (mm.search_query.handle_key(k.key, k.mods)) (void)mm.term->search(mm.search_query.text);
        }};
    }
    // foot's bindings
    if (cs && k.key == Key::c) return Copy{};
    if ((cs && k.key == Key::v) || (md.shift && !md.ctrl && k.key == Key::insert)) return PasteReq{};
    if (md.ctrl && !md.alt && !md.shift && (k.key == Key::equal || k.key == Key::minus || k.key == Key::n0)) {
        const float d = k.key == Key::equal ? 1.0f : k.key == Key::minus ? -1.0f : 0.0f;
        return E{[d](Model& mm) {
            mm.font_size = d == 0.0f ? mm.cfg.font_size : std::clamp(mm.font_size + d, 6.0f, 48.0f);
            fit(mm);
        }};
    }
    if (md.shift && !md.ctrl && (k.key == Key::page_up || k.key == Key::page_down) && !m.term->alt_screen()) {
        const float d = static_cast<float>((k.key == Key::page_up ? 1 : -1) * std::max(m.term->rows() - 2, 1));
        return E{[d](Model& mm) { scroll_by(mm, d); }};
    }
    if (cs && (k.key == Key::up || k.key == Key::down) && !m.term->alt_screen()) {
        const float d = k.key == Key::up ? 1.0f : -1.0f;
        return E{[d](Model& mm) { scroll_by(mm, d); }};
    }
    if (md.shift && !md.ctrl && (k.key == Key::home || k.key == Key::end) && !m.term->alt_screen()) {
        const bool top = k.key == Key::home;
        return E{[top](Model& mm) { scroll_by(mm, top ? 1e9f : -1e9f); }};
    }
    return send(m.term->encode_key(k.key, md));
}

std::optional<App::Msg> App::on(const Model& m, const TextEvent& t) {
    if (m.hinting) {
        std::string typed = m.hint_typed;
        bool upper = false;
        for (char ch : t.text) {
            if (ch >= 'A' && ch <= 'Z') { upper = true; ch = static_cast<char>(ch - 'A' + 'a'); }
            typed.push_back(ch);
        }
        for (std::size_t i = 0; i < m.hint_labels.size(); ++i)
            if (m.hint_labels[i] == typed) return HintPick{i, upper};
        return E{[typed](Model& mm) { mm.hint_typed = typed; mm.grid_key = 0; }};
    }
    if (m.settings_open) {
        if (auto op = settings_panel_text(m.panel, setting_specs(m.cfg), t.text)) return Panel{std::move(*op)};
        return std::nullopt;
    }
    if (m.search_open)
        return E{[s = t.text](Model& mm) { mm.search_query.insert(s); (void)mm.term->search(mm.search_query.text); }};
    return send(m.term->encode_text(t.text, {}));
}

std::optional<App::Msg> App::on(const Model& m, const ScrollEvent& s) {
    if (m.settings_open) return std::nullopt;   // the panel's scroll_view takes it
    const float ch = std::max(m.metrics.cell_h, 1.0f);
    const int lines = static_cast<int>(std::round(s.delta.y / ch * m.cfg.scroll_lines));
    if (lines == 0 && (m.term->mouse_reporting() || m.term->alt_screen())) return std::nullopt;
    if (m.term->mouse_reporting() && !s.mods.shift) {
        const int col = cell_of(s.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(s.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        std::string out;
        for (int i = 0; i < std::min(std::abs(lines), 10); ++i)
            out += m.term->encode_mouse(lines > 0 ? 64 : 65, col, row, true, false, s.mods);
        return send(out);
    }
    if (m.term->alt_screen()) {   // less/man on the alt screen: wheel = arrows
        std::string out;
        for (int i = 0; i < std::min(std::abs(lines), 10); ++i) out += m.term->encode_key(lines > 0 ? Key::up : Key::down, {});
        return send(out);
    }
    const float rows = s.delta.y / ch * m.cfg.scroll_lines;   // fractional: trackpads scroll by the pixel
    return E{[rows](Model& mm) { scroll_by(mm, rows); }};
}

std::optional<App::Msg> App::on(const Model&, const ResizeEvent& r) {
    return E{[s = r.size](Model& mm) {
        const int c0 = mm.term ? mm.term->cols() : 0, r0 = mm.term ? mm.term->rows() : 0;
        mm.size = s;
        fit(mm);
        if (mm.sized_once && mm.term && (mm.term->cols() != c0 || mm.term->rows() != r0)) mm.resized_at = mm.time;
        mm.sized_once = true;
    }};
}

std::optional<App::Msg> App::on(const Model& m, const FocusEvent& f) {
    return E{[on = f.focused, rep = m.term->encode_focus(f.focused)](Model& mm) {
        mm.focused = on;
        mm.cursor_on = true;
        if (!rep.empty() && mm.pty) mm.pty->write(rep);
    }};
}

std::optional<App::Msg> App::on(const Model& m, const MouseDown& e) {
    if (m.settings_open) return std::nullopt;
    if (minimap_on(m) && e.position.x > m.size.x - std::max(m.mm_w, kMinimapRest)) return std::nullopt;   // the minimap's
    const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
    const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
    const int btn = e.button == MouseButton::left ? 0 : e.button == MouseButton::middle ? 1 : e.button == MouseButton::right ? 2 : -1;
    if (btn < 0) return std::nullopt;
    if (m.term->mouse_reporting() && !e.mods.shift) {
        auto bytes = m.term->encode_mouse(btn, col, row, true, false, e.mods);
        return E{[btn, b = std::move(bytes)](Model& mm) { mm.mouse_button = btn; if (mm.pty) mm.pty->write(b); }};
    }
    if (btn == 1) return PasteReq{};   // middle click pastes, as everywhere on Linux
    if (btn == 0 && e.click_count == 1 && (e.mods.ctrl || !m.cfg.links_need_ctrl)) {
        if (auto url = m.term->link_at(col, row); !url.empty()) return OpenUrl{std::move(url)};
    }
    if (btn == 0)
        return E{[col, row = view_row(m, e.position.y), n = e.click_count, p = e.position](Model& mm) {
            mm.term->select_begin(col, row, n);
            mm.selecting = true;
            mm.sel_pos = p;
            mm.sel_edge = 0.0f;
        }};
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const MouseMove& e) {
    if (m.settings_open) return std::nullopt;
    if (!minimap_on(m) && (m.mm_open || m.mm_hover_y >= 0.0f))
        return E{[](Model& mm) { mm.mm_open = false; mm.mm_hover_y = -1.0f; }};
    if (minimap_on(m) && m.mouse_button < 0 && !m.selecting) {
        const bool near = e.position.x > m.size.x - (m.mm_open ? kMinimapOpen + 8.0f : kMinimapRest + 4.0f);
        const bool over = near && e.position.x > m.size.x - std::max(m.mm_w, kMinimapRest);
        const float y = over ? e.position.y : -1.0f;
        if (near != m.mm_open || y != m.mm_hover_y) return E{[near, y](Model& mm) { mm.mm_open = near; mm.mm_hover_y = y; }};
        if (near) return std::nullopt;
    }
    if (!m.selecting && m.mouse_button < 0) {
        // links read as clickable while ctrl is held (or always, when they need no ctrl)
        const bool armed = e.mods.ctrl || !m.cfg.links_need_ctrl;
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        std::string url = armed ? m.term->link_at(col, row) : std::string{};
        if (url == m.hover_url && armed == m.ctrl_held) return std::nullopt;
        return E{[col, row, armed, u = std::move(url)](Model& mm) {
            mm.ctrl_held = armed;
            mm.hover_url = u;
            (void)mm.term->hover(u.empty() ? -1 : col, u.empty() ? -1 : row);
        }};
    }
    const int col = cell_of(e.position.x + m.metrics.cell_w * 0.5f, m.metrics.origin_x, m.metrics.cell_w, m.term->cols() + 1);
    const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
    if (m.mouse_button >= 0) {
        auto bytes = m.term->encode_mouse(m.mouse_button, std::min(col, m.term->cols() - 1), row, true, true, e.mods);
        if (bytes.empty()) return std::nullopt;
        return E{[b = std::move(bytes)](Model& mm) { if (mm.pty) mm.pty->write(b); }};
    }
    return E{[col, row = view_row(m, e.position.y), p = e.position, speed = m.term->alt_screen() ? 0.0f : edge_speed(m, e.position.y)](Model& mm) {
        mm.term->select_extend(col, row);
        mm.sel_pos = p;
        mm.sel_edge = speed;
    }};
}

std::optional<App::Msg> App::on(const Model& m, const MouseUp& e) {
    if (m.mouse_button >= 0) {
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        auto bytes = m.term->encode_mouse(m.mouse_button, col, row, false, false, e.mods);
        return E{[b = std::move(bytes)](Model& mm) { mm.mouse_button = -1; if (mm.pty && !b.empty()) mm.pty->write(b); }};
    }
    if (m.selecting) {
        if (m.cfg.copy_on_select) return Copy{};
        return E{[](Model& mm) { mm.selecting = false; mm.sel_edge = 0.0f; }};
    }
    return std::nullopt;
}

/// Overlay dismissals arrive as Left on the overlay's id.
std::optional<App::Msg> App::on(const Model& m, const Left& l) {
    if (m.settings_open)
        if (auto op = settings_panel_left(m.panel, "settings", l.node)) return Panel{std::move(*op)};
    return std::nullopt;
}

/// The search bar's buttons (mayag term_search parts).
std::optional<App::Msg> App::on(const Model& m, const Clicked& c) {
    if (!m.search_open) return std::nullopt;
    if (c.node == part_id("search", 5)) return Search{-1};
    if (c.node == part_id("search", 6)) return Search{1};
    if (c.node == part_id("search", 9)) return Search{0};
    return std::nullopt;
}

// ── subscriptions, theme, view ─────────────────────────────────────────────

App::Sub App::subscribe(const Model& m) {
    std::vector<Sub> s;
    if (m.pty) s.push_back(Sub::stream("pty", read_pty, m.pty->fd(), static_cast<int>(m.pty->pid())));
    if (m.focused && m.cfg.cursor_blink)
        s.push_back(Sub::every(std::chrono::milliseconds(static_cast<int>(m.cfg.blink_ms)), Msg{Blink{}}));
    const auto cur = m.term->cursor();
    const bool gliding = m.cfg.cursor_smooth && (!m.cur_known || std::fabs(m.cur_x - static_cast<float>(cur.col)) > 0.0f ||
                                                 std::fabs(m.cur_y - static_cast<float>(cur.row)) > 0.0f);
    const bool animating = gliding || m.scroll_d != m.scroll_goal || (m.selecting && m.sel_edge != 0.0f) ||
                           m.mm_w != (m.mm_open ? kMinimapOpen : kMinimapRest) ||
                           m.peek_t != ((m.cfg.peek && m.mm_hover_y >= 0.0f) ? 1.0f : 0.0f) ||
                           m.tail_t != (tail_wanted(m) ? 1.0f : 0.0f) ||
                           m.sticky_t != (sticky_of(m) ? 1.0f : 0.0f) || (m.bell_at >= 0.0 && m.time - m.bell_at < 0.2) || m.time - m.resized_at < 0.9 ||
                           (m.toast && m.time - m.toast_at < 3.2);
    if (const auto st = sticky_of(m); st && st->ran && !st->finished)
        s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));
    if (animating) s.push_back(Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    return Sub::batch(std::move(s));
}

/// The chrome (the settings panel) is cut from the scheme itself: surfaces
/// are the paper lifted toward the ink, text is the ink, the accent is blue.
Theme App::theme(const Model& m) {
    const Palette& p = m.palette;
    const auto paper = rgb_of(p.bg), ink = rgb_of(p.fg);
    Theme t = derive(Seed{.accent = rgb_of(p.ansi[4]), .mode = p.light ? Mode::light : Mode::dark});
    t.background = paper;
    t.surface = mix(paper, ink, 0.05f);
    t.surface_raised = mix(paper, ink, 0.09f);
    t.overlay = mix(paper, ink, 0.07f);
    t.border = mix(paper, ink, 0.14f);
    t.border_strong = mix(paper, ink, 0.24f);
    t.text_primary = ink;
    t.text_secondary = mix(ink, paper, 0.30f);
    t.text_disabled = mix(ink, paper, 0.55f);
    t.accent = rgb_of(p.ansi[4]);
    // status roles = the scheme's ANSI colours, so the panel is syntax-coloured in it
    t.danger = rgb_of(p.ansi[1]);
    t.success = rgb_of(p.ansi[2]);
    t.warning = rgb_of(p.ansi[3]);
    t.info = rgb_of(p.ansi[6]);
    t.font_size = 12.5f;
    // Nerd icons only when the lead face carries them; otherwise plain glyphs (no tofu)
    t.icons = !font_names().empty() && font_names().front().find("Nerd") != std::string::npos ? IconSet::nerd : IconSet::letters;
    t.motion_scale = 0.0f;   // a terminal never animates its chrome
    // window.opacity: the window is CLEARED to the paper at this alpha; the
    // terminal paints no background of its own over it (cells with an
    // explicit colour, the settings panel and menus stay opaque, like foot).
    t.window_opacity = m.cfg.opacity;
    return t;
}

Node App::view(const Model& m) {
    const Ctx& c = ctx();
    const Config& cfg = m.cfg;
    const bool app_shape = m.term->app_cursor_shape();
    // mid-glide the view sits between two rows: draw one extra row above and slide
    const float frac = m.scroll_d - static_cast<float>(m.scroll_applied);
    const bool above = cfg.smooth_scroll && frac > 0.001f;
    const std::uint64_t key = (m.term->epoch() * 8 + (app_shape ? 7 : static_cast<std::uint64_t>(cfg.cursor))) * 2 + (above ? 1 : 0);
    if (!m.grid || m.grid_key != key) {
        auto g = m.term->snapshot(above);
        if (!app_shape)
            g.cursor_shape = cfg.cursor == CursorShape::beam ? TermCursorShape::beam
                           : cfg.cursor == CursorShape::underline ? TermCursorShape::underscore : TermCursorShape::block;
        if (m.hinting) {
            for (std::size_t i = 0; i < m.hints.size(); ++i)
                g.hints.push_back({TermSpan{m.hints[i].row, m.hints[i].col, m.hints[i].len}, m.hint_labels[i]});
            g.hint_typed = m.hint_typed;
            for (const auto& hnt : m.hints) g.matches.push_back({hnt.row, hnt.col, hnt.len});   // light up what is pickable
        }
        m.grid = std::make_shared<const TermGrid>(std::move(g));
        m.grid_key = key;
    }
    const Palette& p = m.palette;
    TermTheme th{.fg = p.fg, .bg = p.bg, .cursor = p.cursor, .cursor_text = p.bg, .selection_bg = p.selection,
                 .bold_brighten = cfg.bold_is_bright ? 0.22f : 0.0f};
    const float bell = m.bell_at >= 0.0 ? static_cast<float>(std::max(0.0, 1.0 - (m.time - m.bell_at) / 0.2)) : 0.0f;
    const float ch = m.metrics.cell_h;
    TermViewOptions o{.font_size = m.font_size, .line_height = cfg.line_height, .padding = cfg.padding,
                      .focused = m.focused, .cursor_on = m.cursor_on || !m.focused || !cfg.cursor_blink,
                      .dim_unfocused = m.focused ? 0.0f : cfg.dim_unfocused, .bell = bell, .marks = cfg.shell_marks,
                      .opacity = 0.0f, .family = family_of(cfg)};   // the window clear is the paper
    th.mark_prompt = p.ansi[8]; th.mark_ok = p.ansi[2]; th.mark_error = p.ansi[1]; th.mark_running = p.ansi[4];
    th.match_bg = p.selection; th.hint_bg = p.ansi[3]; th.hint_fg = p.bg; th.link = p.ansi[6];
    if (cfg.cursor_smooth && m.cur_known) {
        const auto cur = m.term->cursor();
        o.cursor_dx = m.cur_x - static_cast<float>(cur.col);
        o.cursor_dy = m.cur_y - static_cast<float>(cur.row);
        o.cursor_trail = true;
    }
    o.cursor_glow = cfg.cursor_glow ? 1.0f : 0.0f;
    o.y_offset = above ? (frac - 1.0f) * ch : 0.0f;   // row 0 is the one above: start it a row up, slide down by frac

    if (m.settings_open) {
        const auto specs = setting_specs(cfg);
        SettingsPanelProps props{.title = "settings", .subtitle = "", .brand = "\u276F", .footer = config_path(),
                                 .width = std::min(860.0f, m.size.x - 40.0f), .height = std::min(600.0f, m.size.y - 40.0f)};
        c.overlay(Overlay{
            .content = settings_panel(c, "settings", specs, m.panel, props, [](SettingsOp op) -> Msg { return Panel{std::move(op)}; }),
            .id = part_id("settings", part_menu),
            .placement = Placement::centered,
            .dismiss = Dismiss::modal,
            .layer = 10,
            .scrim = 0.35f});
    }
    if (m.search_open)
        term_search(c, "search", node_id("term"), m.search_query, false, false,
                    static_cast<int>(m.term->search_current()) - 1, static_cast<int>(m.term->search_count()));
    const auto shape = !m.hover_url.empty() ? mayag::CursorShape::pointer
                     : m.term->mouse_reporting() ? mayag::CursorShape::arrow : mayag::CursorShape::text;
    // resize HUD, toasts
    if (m.time - m.resized_at < 0.8)
        resize_hud(c, "hud", m.term->cols(), m.term->rows(),
                   std::to_string(static_cast<int>(m.size.x)) + "\u00D7" + std::to_string(static_cast<int>(m.size.y)) + "px");
    if (m.toast && m.time - m.toast_at < 3.0 && !m.settings_open) toasts(c, "toasts", {*m.toast}, m.size);
    if (m.hinting) {
        const auto dim = mix(rgb_of(p.fg), rgb_of(p.bg), 0.45f);
        const std::string n = std::to_string(m.hints.size()) + " targets";
        c.overlay(Overlay{.content = hud_chip(c, {{m.hint_typed.empty() ? n : m.hint_typed + "\u258F", rgb_of(p.fg), true},
                                                  {"a-z copy/open", dim}, {"A-Z type", dim}, {"esc", dim}},
                                              {.tag = "HINT", .tone = rgb_of(p.ansi[3]), .size = m.font_size * 0.85f}).build(),
                          .id = node_id("hint.tip"), .placement = Placement::cursor, .dismiss = Dismiss::never,
                          .offset = 0.0f, .layer = 5, .at = Vec2{16.0f, m.size.y - 40.0f}, .passthrough = true});
    }
    Node term = (node(terminal_view(c, "term", *m.grid, th, o)) | size(m.size.x, m.size.y) | cursor(shape)).build();
    std::vector<Node> layers;
    layers.push_back(std::move(term));
    const bool back = m.scroll_d > 0.01f || m.term->scroll_offset() > 0;
    const float mmw = minimap_on(m) ? std::max(m.mm_w, kMinimapRest) : 0.0f;

    // ── the live tail: while you read history, the last rows of live output stay
    //    pinned at the bottom (iTerm-style), sliding up as you leave the live view
    float tail_h = 0.0f;
    if (m.tail_t > 0.0f) {
        const int n = std::max(static_cast<int>(cfg.live_tail), 1);
        const std::int64_t cur = m.term->cursor_abs();
        const TermGrid tg = m.term->grid_at(cur - n + 1, n);
        const auto ink = rgb_of(p.fg), paper = rgb_of(p.bg);
        const float full = static_cast<float>(n) * ch + 2.0f * 4.0f + 1.0f;
        const float ease = 1.0f - (1.0f - m.tail_t) * (1.0f - m.tail_t);   // ease-out
        tail_h = full * ease;
        TermViewOptions to = o;
        to.focused = false;
        to.cursor_trail = false;
        to.cursor_glow = 0.0f;
        to.y_offset = 0.0f;
        to.padding = 4.0f;
        to.marks = cfg.shell_marks;
        const bool fresh = m.new_below > 0;
        const auto tone = rgb_of(fresh ? p.ansi[2] : p.ansi[4]);
        const bool hot = c.hovered(node_id("tail"));
        layers.push_back((v(node(terminal_view(c, "tail.grid", tg, th, to)) | width(pct(100)) | height(full - 1.0f))
                          | size(m.size.x - mmw, full) | clip
                          | bg(hot ? mix(paper, ink, 0.07f) : mix(paper, ink, 0.035f))
                          | inner_shadow(0.0f, mix(paper, tone, 0.6f), {0.0f, 1.0f})
                          | pointer_cursor | id_of("tail")
                          | on_click([] { return Msg{E{[](Model& mm) { scroll_by(mm, -1e9f); }}}; })
                          | absolute(0.0f, m.size.y - tail_h)).build());
        // its label rides the divider, top-left: LIVE (or LIVE +N while output streams in)
        layers.push_back((hud_chip(c, {{fresh ? "+" + std::to_string(m.new_below) : "\u2193 click to return", fresh ? tone : mix(ink, paper, 0.4f)}},
                                   {.tag = "LIVE", .tone = tone, .size = m.font_size * 0.72f})
                          | absolute(cfg.padding + 10.0f, m.size.y - tail_h - std::round(m.font_size * 0.72f * 1.75f) * 0.5f)).build());
    }

    // ── the sticky command: scrolled into a command's output, its header rides
    //    on top: where it ran, what it was, how it went, how far through it you are
    if (back && m.sticky_t > 0.0f)
        if (const auto cmd = sticky_of(m)) {
            const auto ink = rgb_of(p.fg), paper = rgb_of(p.bg);
            const auto dim = mix(ink, paper, 0.45f), faint = mix(paper, ink, 0.12f);
            const bool fail = cmd->finished && cmd->exit_code != 0;
            const auto tone = !cmd->finished ? rgb_of(p.ansi[4]) : fail ? rgb_of(p.ansi[1]) : rgb_of(p.ansi[2]);
            const float fs = m.font_size * 0.92f;
            const float hh = std::round(ch + 8.0f);
            const float w = m.size.x - (minimap_on(m) ? std::max(m.mm_w, kMinimapRest) : 0.0f);
            std::string line = m.term->command_text(*cmd);
            if (line.empty()) line = "(command)";
            // status: OK / EXIT n / a spinner with the live elapsed time
            std::string tag, time;
            if (!cmd->finished) {
                static constexpr const char* spin[] = {"\u280B", "\u2819", "\u2839", "\u2838", "\u283C", "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"};
                tag = std::string{spin[m.clock % 10]} + " RUN";
                if (cmd->start_ms > 0) time = pretty_ms(std::max<std::int64_t>(wall_ms() - cmd->start_ms, 0));
            } else {
                tag = fail ? "EXIT " + std::to_string(cmd->exit_code) : "OK";
                if (cmd->duration_ms > 0) time = pretty_ms(cmd->duration_ms);
            }
            // how far through this command's output the view's top is
            const auto info = m.term->scroll_info();
            const std::int64_t out0 = cmd->output_row >= 0 ? cmd->output_row : cmd->prompt_row + 1;
            const std::int64_t out1 = cmd->finished && cmd->end_row > out0 ? cmd->end_row : static_cast<std::int64_t>(info.total);
            const std::int64_t span = std::max<std::int64_t>(out1 - out0, 1);
            const std::int64_t at = std::clamp<std::int64_t>(info.first - out0 + 1, 1, span);
            const float through = static_cast<float>(at) / static_cast<float>(span);
            const bool hot = c.hovered(node_id("sticky"));
            std::vector<Node> row;
            row.push_back((h(text_owned(tag) | font(fs * 0.85f) | monospace | semibold | fg(paper))
                           | height(std::round(fs * 1.5f)) | pad(0, fs * 0.45f) | center | bg(tone)).build());
            if (!cmd->cwd.empty())
                row.push_back((text_owned(short_path(cmd->cwd)) | font(fs) | monospace | fg(rgb_of(p.ansi[4]))).build());
            row.push_back((text_owned("\u276F") | font(fs) | monospace | fg(rgb_of(p.ansi[5]))).build());
            row.push_back((text_owned(line) | font(fs) | monospace | semibold | fg(ink) | grow() | ellipsis | min_size(0, 0)).build());
            row.push_back((text_owned(std::to_string(at) + "/" + std::to_string(span)) | font(fs * 0.85f) | monospace | fg(dim)).build());
            if (!time.empty()) row.push_back((text_owned(time) | font(fs * 0.85f) | monospace | fg(cmd->finished ? dim : tone)).build());
            row.push_back((text_owned(hot ? "\u2191 prompt" : "") | font(fs * 0.85f) | monospace | fg(tone)).build());
            const float ease = 1.0f - (1.0f - m.sticky_t) * (1.0f - m.sticky_t);
            layers.push_back((v(h(std::move(row)) | gap(10.0f) | align(Align::center) | height(hh - 2.0f) | width(pct(100))
                                    | pad(0, cfg.padding + 10.0f, 0, cfg.padding + (cfg.shell_marks ? 10.0f : 0.0f)),
                                // progress through the output: a hairline in the status colour
                                h(box() | size(std::max(w * through, 2.0f), 2.0f) | bg(tone.fade(0.85f))) | width(pct(100)) | height(2.0f)
                                    | bg(faint))
                              | size(w, hh) | clip
                              | bg(hot ? mix(paper, ink, 0.085f) : mix(paper, ink, 0.05f))
                              | opacity(ease)
                              | pointer_cursor | id_of("sticky")
                              | on_click([prompt = cmd->prompt_row] {
                                    return Msg{E{[prompt](Model& mm) {   // fly to its prompt
                                        const auto i = mm.term->scroll_info();
                                        const float want = static_cast<float>(i.total - i.visible) - static_cast<float>(std::max<std::int64_t>(prompt - 1, 0));
                                        scroll_by(mm, std::clamp(want, 0.0f, static_cast<float>(mm.term->history())) - mm.scroll_goal);
                                    }}};
                                })
                              | absolute(0.0f, -hh * (1.0f - ease))).build());   // slides down from the top edge
        }

    // back in history: a status chip, bottom right, like less/vim -- click to go live
    if (back && !m.term->alt_screen()) {
        const int off = static_cast<int>(std::lround(m.scroll_d));
        const auto info = m.term->scroll_info();
        const int pct = info.total > info.visible ? static_cast<int>(100.0 * (info.first) / (info.total - info.visible)) : 100;
        const bool hot = c.hovered(node_id("tolive"));
        const bool fresh = m.new_below > 0;
        const auto acc = rgb_of(fresh ? p.ansi[2] : p.ansi[3]);
        const float fs = m.font_size * 0.85f;
        const std::string pos = std::to_string(off) + "\u2191";
        const std::string pc = std::to_string(std::clamp(pct, 0, 100)) + "%";
        std::vector<HudSeg> segs = fresh ? std::vector<HudSeg>{{"+" + std::to_string(m.new_below), acc, true}, {"\u2193", acc}}
                                         : std::vector<HudSeg>{{pos, rgb_of(p.fg), true}, {pc, mix(rgb_of(p.fg), rgb_of(p.bg), 0.45f)}};
        const float cw = cell_width(fs, cfg.font_family);
        std::size_t chars = 0;
        for (const auto& sg : segs) {   // codepoints, not bytes
            for (unsigned char b8 : sg.text) chars += (b8 & 0xC0) != 0x80 ? 1 : 0;
            ++chars;
        }
        const std::string tag = fresh ? "NEW" : "HIST";
        const float pw = (static_cast<float>(chars + tag.size()) + 2.0f) * cw + fs * 0.6f * 3.0f;
        const float ph = std::round(fs * 1.75f);
        layers.push_back((hud_chip(c, segs, {.tag = tag, .tone = acc, .size = fs, .hot = hot})
                          | pointer_cursor | id_of("tolive")
                          | on_click([] { return Msg{E{[](Model& mm) { scroll_by(mm, -1e9f); }}}; })
                          | absolute(m.size.x - pw - (minimap_on(m) ? std::max(m.mm_w, kMinimapRest) + 6.0f : 6.0f), m.size.y - ph - 6.0f - tail_h)).build());
    }
    if (!minimap_on(m)) return (z(std::move(layers)) | size(m.size.x, m.size.y)).build();

    // the minimap: the whole scrollback as a strip -- prompts, failures, search hits
    const auto info = m.term->scroll_info();
    ScrollbackInfo sb{.total = info.total, .first = info.first, .visible = info.visible};
    for (const auto& cmd : m.term->commands())
        if (cmd.prompt_row >= 0 && cmd.ran)
            sb.marks.push_back({static_cast<int>(cmd.prompt_row),
                                cmd.finished && cmd.exit_code != 0 ? ScrollbackMark::Kind::error : ScrollbackMark::Kind::prompt});
    const auto rows = m.term->search_rows();
    for (std::size_t i = 0; i < rows.size(); ++i)
        sb.marks.push_back({static_cast<int>(rows[i]), i + 1 == m.term->search_current() ? ScrollbackMark::Kind::current_match
                                                                                         : ScrollbackMark::Kind::match});
    // command spans: a status stripe
    for (const auto& cmd : m.term->commands()) {
        if (!cmd.ran || cmd.prompt_row < 0) continue;
        const int last = cmd.finished && cmd.end_row > cmd.prompt_row ? static_cast<int>(cmd.end_row) - 1 : info.total - 1;
        sb.spans.push_back({static_cast<int>(cmd.prompt_row), last,
                            !cmd.finished ? ScrollbackSpan::Kind::running
                            : cmd.exit_code != 0 ? ScrollbackSpan::Kind::error : ScrollbackSpan::Kind::ok});
    }
    const float hgt = m.size.y;
    sb.rest_width = kMinimapRest;
    sb.width = kMinimapOpen;
    sb.expanded = m.mm_w > kMinimapRest + 0.5f;
    sb.width = std::max(m.mm_w, kMinimapRest);
    sb.line_px = 2.0f;
    {   // the text itself, binned to the strip's rows
        const float used = std::min(hgt, scrollback_scale(sb, hgt) * static_cast<float>(std::max(info.total, 1)));
        sb.buckets = scrollback_buckets(info.total, used, 2.0f);
        sb.runs = m.term->minimap(sb.buckets);
    }
    auto bar = node(scrollback_bar(c, "minimap", sb, hgt))
             | on_slide([sb, hgt](Vec2 at) -> Msg {
                   const int line = scrollback_line_at(sb, at.y, hgt);
                   return E{[line](Model& mm) {
                       const auto i = mm.term->scroll_info();
                       scroll_by(mm, static_cast<float>(i.total - i.visible - line) - mm.scroll_goal);
                   }};
               });
    layers.push_back((std::move(bar) | absolute(m.size.x - std::max(m.mm_w, kMinimapRest), 0.0f)).build());

    // ── the peek: hovering the minimap shows the command that made that part of
    //    the history, and the lines themselves, without scrolling there
    if (m.peek_t > 0.0f && m.mm_hover_y >= 0.0f && info.total > 0) {
        const auto ink = rgb_of(p.fg), paper = rgb_of(p.bg);
        const auto dim = mix(ink, paper, 0.45f);
        const std::int64_t line = std::clamp<std::int64_t>(static_cast<std::int64_t>(m.mm_hover_y / hgt * static_cast<float>(info.total)),
                                                         0, info.total - 1);
        std::optional<Term::Command> at;
        for (const auto& cmd : m.term->commands()) {
            if (cmd.prompt_row < 0 || cmd.prompt_row > line) continue;
            const std::int64_t end = cmd.finished && cmd.end_row > cmd.prompt_row ? cmd.end_row : info.total;
            if (line < end) at = cmd;
        }
        constexpr int kRows = 7;
        const std::int64_t first = std::clamp<std::int64_t>(line - kRows / 2, 0, std::max<std::int64_t>(info.total - kRows, 0));
        const int pcols = std::min(m.term->cols(), 72);
        TermGrid pg = m.term->grid_at(first, kRows, pcols);
        // the hovered line, highlighted
        pg.selection.push_back({static_cast<int>(line - first), 0, pcols});
        const float pfs = m.font_size * 0.82f;
        const float pcw = cell_width(pfs, cfg.font_family);
        const float pch = std::round(pfs * cfg.line_height);
        const float cw_ = static_cast<float>(pcols) * pcw + 10.0f + 12.0f;
        TermViewOptions po = o;
        po.font_size = pfs; po.focused = false; po.cursor_trail = false; po.cursor_glow = 0.0f;
        po.y_offset = 0.0f; po.padding = 5.0f; po.marks = cfg.shell_marks; po.dim_unfocused = 0.0f;
        // header: the command, its result, its time; or just where you are
        std::vector<HudSeg> head;
        std::string tag = "L" + std::to_string(line + 1);
        Color<Srgb> tone = dim;
        if (at && at->ran) {
            std::string cmdline = m.term->command_text(*at);
            if (cmdline.empty()) cmdline = "(command)";
            const std::size_t max_chars = static_cast<std::size_t>(std::max(pcols - 18, 12));
            if (cmdline.size() > max_chars) cmdline = cmdline.substr(0, max_chars - 1) + "\u2026";
            const bool fail = at->finished && at->exit_code != 0;
            tone = !at->finished ? rgb_of(p.ansi[4]) : fail ? rgb_of(p.ansi[1]) : rgb_of(p.ansi[2]);
            tag = !at->finished ? "RUN" : fail ? "EXIT " + std::to_string(at->exit_code) : "OK";
            head.push_back({"\u276F " + cmdline, ink, true});
            if (at->finished && at->duration_ms > 0) {
                char d[24];
                std::snprintf(d, sizeof d, "%.1fs", static_cast<double>(at->duration_ms) / 1000.0);
                head.push_back({d, dim});
            }
        } else {
            head.push_back({"line " + std::to_string(line + 1) + " of " + std::to_string(info.total), dim});
        }
        const float card_h = std::round(pfs * 1.75f) + static_cast<float>(kRows) * pch + 10.0f;
        const float ease = 1.0f - (1.0f - m.peek_t) * (1.0f - m.peek_t);
        const float cx = m.size.x - mmw - cw_ - 8.0f + (1.0f - ease) * 10.0f;   // slides in from the minimap
        const float cy = std::clamp(m.mm_hover_y - card_h * 0.5f, 6.0f, m.size.y - card_h - 6.0f);
        layers.push_back((v(hud_chip(c, head, {.tag = tag, .tone = tone, .size = pfs}) | width(pct(100)),
                            node(terminal_view(c, "peek.grid", pg, th, po)) | width(pct(100)) | grow())
                          | size(cw_, card_h) | clip | bg(paper) | border(1.0f, mix(paper, tone, 0.55f))
                          | opacity(ease) | absolute(cx, cy)).build());
        // a hairline from the card to the hovered row on the minimap
        layers.push_back((box() | size(8.0f * ease, 1.0f) | bg(mix(paper, tone, 0.7f))
                          | absolute(m.size.x - mmw - 8.0f, std::clamp(m.mm_hover_y, cy, cy + card_h))).build());
    }
    return (z(std::move(layers)) | size(m.size.x, m.size.y)).build();
}

}  // namespace hand
