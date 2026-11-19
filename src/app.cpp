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
#include <fstream>
#include <unistd.h>
#include <utility>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using E = Edit<App::Model>;

namespace {

constexpr float kMinimapRest = 14.0f, kMinimapOpen = 96.0f;
/// Where the terminal starts (the strip may sit above it or below it).
float term_top(const App::Model& m) { return m.cfg.tab_bottom ? 0.0f : m.strip_h(); }
bool in_strip(const App::Model& m, float y) {
    return m.cfg.tab_bottom ? y >= m.win.y - m.strip_h() : y < m.strip_h();
}

/// The foreground process of a session's shell, from /proc (cached ~0.5 s).
std::string fg_process(const App::Session& se, double now) {
    if (!se.pty || now - se.proc_at < 0.5) return se.proc;
    se.proc_at = now;
    const pid_t pg = ::tcgetpgrp(se.pty->fd());
    if (pg <= 0) return se.proc;
    std::ifstream f{"/proc/" + std::to_string(pg) + "/comm"};
    std::string name;
    std::getline(f, name);
    se.proc = name;
    return se.proc;
}

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

/// hand's logo, drawn: a raised hand on a scheme-gradient tile, with a
/// terminal cursor blinking in its palm. `px` = the tile's size.
Node hand_logo(const Palette& p, float px, bool cursor_on, float round = 1.0f) {
    const float k = px / 52.0f;
    const auto skin = mix(colors::white, rgb_of(p.ansi[4]), 0.04f);   // opaque: overlaps leave no seams
    const auto c1 = rgb_of(p.ansi[4]), c2 = rgb_of(p.ansi[5]);
    std::vector<Node> parts;
    parts.push_back((box() | size(px, px) | radius(12.0f * k * round)
                     | linear_gradient(mix(c1, colors::white, 0.08f), c2, {0.0f, 0.0f}, {1.0f, 1.0f})).build());
    // a soft sheen across the top
    parts.push_back((box() | size(px, px * 0.5f) | radius(12.0f * k * round, 12.0f * k * round, 0.0f, 0.0f)
                     | linear_gradient(colors::white.fade(0.16f), colors::white.fade(0.0f))).build());
    const auto cap = [&](float x, float y, float w, float h) {
        parts.push_back((box() | size(w * k, h * k) | radius(w * k * 0.5f) | bg(skin) | absolute(x * k, y * k)).build());
    };
    cap(15.5f, 10.0f, 7.0f, 22.0f);   // index
    cap(23.5f, 7.5f, 7.0f, 24.0f);    // middle
    cap(31.5f, 9.5f, 7.0f, 22.0f);    // ring
    cap(39.0f, 14.5f, 6.0f, 17.0f);   // little
    parts.push_back((box() | size(30.0f * k, 20.0f * k) | radius(6.0f * k, 6.0f * k, 11.0f * k, 11.0f * k) | bg(skin)
                     | absolute(15.0f * k, 23.0f * k)).build());   // palm
    for (int i = 0; i <= 8; ++i) {   // the thumb: a stroke angled out from the palm
        const float f = static_cast<float>(i) / 8.0f;
        const float x = 18.0f + (8.5f - 18.0f) * f, y = 34.0f + (22.5f - 34.0f) * f;
        parts.push_back((box() | size(7.5f * k, 7.5f * k) | radius(3.75f * k) | bg(skin)
                         | absolute((x - 3.75f) * k, (y - 3.75f) * k)).build());
    }
    if (cursor_on)   // the cursor in the palm
        parts.push_back((box() | size(5.0f * k, 9.0f * k) | radius(1.0f * k) | bg(c1) | absolute(27.5f * k, 29.0f * k)).build());
    return (z(std::move(parts)) | size(px, px)).build();
}

// ── confirmation: what is risky, what is running ─────────────────────────

struct Risk { bool flag = false; std::string note; };

/// Does this pasted line deserve a second look?
Risk risk_of(const std::string& l) {
    for (unsigned char ch : l)
        if ((ch < 0x20 && ch != '\t') || ch == 0x7F) return {true, "control chars"};
    const auto has = [&](const char* w) { return l.find(w) != std::string::npos; };
    const auto word = [&](const char* w) {   // at the start, or after a separator
        const std::string ws = w;
        for (std::size_t p = l.find(ws); p != std::string::npos; p = l.find(ws, p + 1))
            if ((p == 0 || std::string_view{" ;&|("}.find(l[p - 1]) != std::string_view::npos) &&
                (p + ws.size() == l.size() || l[p + ws.size()] == ' '))
                return true;
        return false;
    };
    if (word("sudo") || word("doas") || word("su")) return {true, "root"};
    if ((word("rm") && (has(" -rf") || has(" -fr") || has(" -r ") || has(" -R"))) || word("shred")) return {true, "deletes"};
    if ((has("curl ") || has("wget ")) && (has("| sh") || has("|sh") || has("| bash") || has("|bash"))) return {true, "runs a download"};
    if (has("mkfs") || has("dd if=") || has("of=/dev/") || has("> /dev/sd") || has("> /dev/nvme")) return {true, "writes a disk"};
    if (has(":(){") || has(":() {")) return {true, "fork bomb"};
    if (has("chmod -R 777") || has("chown -R")) return {true, "permissions"};
    if (word("git") && has(" push") && (has(" --force") || has(" -f"))) return {true, "force push"};
    if (word("shutdown") || word("reboot") || word("poweroff")) return {true, "power"};
    return {};
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); out.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool is_shell(const std::string& p) {
    return p.empty() || p == "fish" || p == "zsh" || p == "bash" || p == "sh" || p == "dash" || p == "nu" || p == "xonsh" || p == "ksh";
}

/// What a tab is busy with ("" = just a prompt), and for how long.
std::string busy_of(const App::Session& se, double now, std::string* elapsed) {
    if (!se.term) return {};
    std::string what;
    for (const auto& c : se.term->commands()) {   // shell integration knows exactly
        if (c.ran && !c.finished) {
            what = se.term->command_text(c);
            if (elapsed && c.start_ms > 0) *elapsed = pretty_ms(std::max<std::int64_t>(wall_ms() - c.start_ms, 0));
        }
    }
    const std::string proc = fg_process(se, now);
    if (what.empty() && !is_shell(proc)) what = proc;   // without integration: the foreground program
    return what;
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
void read_pty(Sink<App::Msg> out, std::stop_token st, int fd, int pid, std::uint64_t sid) {
    std::string buf;
    while (!st.stop_requested()) {
        const bool alive = pty_read(fd, buf, 50);   // 50 ms poll: stop is always prompt
        if (!buf.empty()) { out.send(App::Output{std::move(buf), sid}); buf.clear(); }
        if (!alive) {
            int code = -1;
            for (int i = 0; i < 20 && code < 0; ++i) {   // bounded: ≤ ~200 ms
                code = pty_reap(static_cast<pid_t>(pid));
                if (code < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out.send(App::Exited{std::max(code, 0), sid});
            return;
        }
    }
}

/// The viewport row under window y, through a smooth-scroll glide (the grid is
/// drawn up to a row lower mid-glide). May be -1: the row just above the view.
/// How far below its row the grid is drawn (0..1): a scroll glide between rows
/// plus the glide of a freshly arrived line at the live prompt.
float draw_frac(const App::Model& m) {
    const float scroll = m.cfg.smooth_scroll ? m.scroll_d - static_cast<float>(m.scroll_applied) : 0.0f;
    return std::clamp(scroll + m.out_shift, 0.0f, 0.999f);
}

int view_row(const App::Model& m, float y) {
    const float frac = draw_frac(m);
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

float App::Model::strip_h() const noexcept {
    // the strip shows from the second tab on (or always, by setting), easing in
    return std::round(std::clamp(cfg.tab_height, 26.0f, 60.0f) * strip_t);
}

void App::fit(Model& m) {
    m.size = Vec2{m.win.x, std::max(m.win.y - m.strip_h(), 1.0f)};
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
    // every tab, not just the visible one: a background shell sees the real size
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        Session& se = m.at(i);
        if (!se.term || (cols == se.term->cols() && rows == se.term->rows())) continue;
        se.term->resize(cols, rows);
        if (se.pty) se.pty->resize(cols, rows, static_cast<int>(m.size.x), static_cast<int>(m.size.y));
        se.grid_key = 0;
    }
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

App::Session App::spawn_session(Model& m, const std::string& cwd) {
    Session se;
    se.sid = m.next_sid++;
    se.opened_at = m.time;
    se.term = std::make_shared<Term>(std::max(m.metrics.fit_cols, 2), std::max(m.metrics.fit_rows, 1), m.palette,
                                     static_cast<int>(m.cfg.scrollback));
    if (m.cfg.demo) return se;
    se.pty = std::make_shared<Pty>();
    const std::string shell = Pty::resolve_shell(m.cfg.shell_cli.empty() ? m.cfg.shell : m.cfg.shell_cli);
    ShellLaunch si = m.cfg.shell_integration ? shell_integration(shell) : ShellLaunch{};
    if (!cwd.empty()) si.env.emplace_back("PWD", cwd);
    se.term->set_prompt_protect(!si.reflow_aware);
    if (!se.pty->spawn(m.metrics.fit_cols, m.metrics.fit_rows, shell, si.env, si.args, cwd)) {
        (void)se.term->feed("\x1b[31mhand: could not start a shell\x1b[0m\r\n");
        se.pty.reset();
    }
    return se;
}

App::Cmd App::init(Model& m) {
    m.cfg = config;
    m.palette = palette_of(m.cfg);
    m.font_size = m.cfg.font_size;
    m.strip_t = m.cfg.tabs_always ? 1.0f : 0.0f;
    fit(m);
    static_cast<Session&>(m) = spawn_session(m, "");
    m.tabs.resize(1);
    m.active = 0;
    return SetTitle{"hand"};
}

// ── updates ────────────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, Output o) {
    if (o.sid != 0 && o.sid != m.sid) {   // a background tab: feed it, mark it
        for (std::size_t i = 0; i < m.tab_count(); ++i) {
            if (i == m.active || m.tabs[i].sid != o.sid) continue;
            Session& se = m.tabs[i];
            const Effects fx = se.term->feed(o.bytes);
            if (!fx.reply.empty() && se.pty) se.pty->write(fx.reply);
            if (fx.title_set) se.title = fx.title.empty() ? "hand" : fx.title;
            if (fx.bell) se.bell = true;
            se.unread = true;
            se.bytes += o.bytes.size();
            se.grid_key = 0;
            // a long command finished in a background tab: say so, naming the tab
            for (const auto& c : se.term->commands()) {
                if (!c.finished || !c.ran || c.id <= se.notified) continue;
                se.notified = c.id;
                if (m.cfg.notify_after <= 0.0f || static_cast<double>(c.duration_ms) / 1000.0 < m.cfg.notify_after) continue;
                m.toast = Toast{.title = (c.exit_code == 0 ? "\u2713 tab " : "\u2717 tab ") + std::to_string(i + 1) + " " +
                                         (c.exit_code == 0 ? "done" : "failed (" + std::to_string(c.exit_code) + ")"),
                                .body = se.term->command_text(c),
                                .kind = c.exit_code == 0 ? Toast::Kind::success : Toast::Kind::error};
                m.toast_at = m.time;
            }
            return {};
        }
        return {};
    }
    m.bytes += o.bytes.size();
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
                // Enter at a prompt: the screen glides up a line instead of jumping.
                // Only a lone line from rest -- a stream of output snaps, so it never lags.
                if (m.cfg.motion && m.cfg.smooth_scroll && grew == 1 && m.out_shift < 0.05f && !m.term->alt_screen())
                    m.out_shift = 1.0f;
                else if (grew > 0)
                    m.out_shift = 0.0f;
            }
        }
        if (off1 > 0) m.new_below += std::max(m.term->history() - hist0, 0);
    }
    if (!fx.reply.empty() && m.pty) m.pty->write(fx.reply);
    if (fx.bell && (m.cfg.visual_bell || m.cfg.bell_shake)) m.bell_at = m.time;
    m.cursor_on = true;   // output resets the blink, like foot
    m.cur_a = 1.0f;
    m.active_at = m.time;
    std::vector<Cmd> cmds;
    if (fx.title_set) { m.title = fx.title.empty() ? "hand" : fx.title; cmds.push_back(SetTitle{m.title}); }
    if (fx.bell) m.bell = false;   // seen: it is the tab you are looking at
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

App::Cmd App::update(Model& m, Exited e) {
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        if (m.at(i).sid == e.sid || (e.sid == 0 && i == m.active)) return update(m, CloseTab{i, true});   // the shell left: nothing to ask
    return {};
}

// ── tabs ───────────────────────────────────────────────────────────────────

namespace {
/// Park the active session in its slot, bring slot `i` into the Model.
void activate(App::Model& m, std::size_t i) {
    if (i == m.active || i >= m.tab_count()) return;
    m.tabs[m.active] = std::move(static_cast<App::Session&>(m));
    static_cast<App::Session&>(m) = std::move(m.tabs[i]);
    m.tabs[i] = App::Session{};
    m.active = i;
    m.unread = m.bell = false;
    m.grid_key = 0;
    m.cur_known = false;              // the cursor appears where it is, no flight across tabs
    m.fx.ps.clear();
    m.mm_hover_y = -1.0f;
    m.cursor_on = true;
    m.cur_a = 1.0f;
}
}  // namespace

App::Cmd App::update(Model& m, NewTab) {
    const std::string cwd = m.term ? m.term->cwd() : std::string{};   // the new tab opens where you are
    const bool first_split = m.tab_count() == 1 && !m.cfg.tabs_always;
    m.tabs.push_back(spawn_session(m, cwd));
    const std::size_t i = m.tabs.size() - 1;
    m.switch_dir = 1;
    activate(m, i);
    m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    if (first_split && !m.cfg.motion) m.strip_t = 1.0f;
    fit(m);
    return SetTitle{m.title};
}

App::Cmd App::update(Model& m, CloseTab c) {
    if (c.i >= m.tab_count()) return {};
    if (!c.confirmed && m.cfg.close_confirm != CloseConfirm::never && m.ask == Model::Ask::none) {
        const bool busy = !busy_of(m.at(c.i), m.time, nullptr).empty();
        if (busy || m.cfg.close_confirm == CloseConfirm::always) {
            m.ask = m.tab_count() == 1 ? Model::Ask::quit : Model::Ask::close_tab;
            m.ask_tab = c.i;
            m.ask_focus = 1;
            m.ask_remember = false;
            return {};
        }
    }
    if (m.tab_count() == 1) {   // the last one: like foot, the window goes with it
        if (!m.cfg.demo) (void)config_save(m.cfg, config_path());
        return Cmd::quit();
    }
    if (c.i == m.active) {
        const std::size_t to = c.i + 1 < m.tab_count() ? c.i + 1 : c.i - 1;   // the neighbour on the right, else left
        m.switch_dir = to > c.i ? 1 : -1;
        activate(m, to);
        m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    }
    // the closing session's pty dies with its Session (SIGHUP to the shell)
    m.tabs.erase(m.tabs.begin() + static_cast<long>(c.i));
    if (m.active > c.i) --m.active;
    if (m.tab_count() == 1 && !m.cfg.tabs_always && !m.cfg.motion) m.strip_t = 0.0f;
    fit(m);
    return SetTitle{m.title};
}

App::Cmd App::update(Model& m, SelectTab st) {
    if (st.i >= m.tab_count() || st.i == m.active) return {};
    m.switch_dir = st.i > m.active ? 1 : -1;
    activate(m, st.i);
    m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    return SetTitle{m.title};
}

App::Cmd App::update(Model& m, StepTab st) {
    const auto n = static_cast<long>(m.tab_count());
    if (n < 2) return {};
    return update(m, SelectTab{static_cast<std::size_t>(((static_cast<long>(m.active) + st.d) % n + n) % n)});
}

App::Cmd App::update(Model& m, MoveTab mv) {
    const long to = static_cast<long>(m.active) + mv.d;
    if (to < 0 || to >= static_cast<long>(m.tab_count())) return {};
    std::swap(m.tabs[m.active], m.tabs[static_cast<std::size_t>(to)]);   // the placeholder moves with it
    m.active = static_cast<std::size_t>(to);
    return {};
}

App::Cmd App::update(Model& m, Clock) {
    ++m.clock;
    m.grid_key = 0;
    // one activity sample a second per tab: bytes in, as a 0..1 rate
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        Session& se = m.at(i);
        se.activity.push_back(std::clamp(std::log1p(static_cast<float>(se.bytes)) / 9.0f, 0.0f, 1.0f));
        if (se.activity.size() > 20) se.activity.erase(se.activity.begin());
        se.bytes = 0;
    }
    return {};
}

App::Cmd App::update(Model& m, Blink) {
    m.cursor_on = !m.cursor_on || !m.cfg.cursor_blink;
    if (m.cfg.blink_style == BlinkStyle::hard) m.cur_a = m.cursor_on ? 1.0f : 0.0f;
    return {};
}
App::Cmd App::update(Model& m, Tick t) {
    m.time += t.dt;
    const float mdt = static_cast<float>(t.dt) * m.cfg.motion_speed;   // the motion clock
    if (m.selecting && m.sel_edge != 0.0f) {   // drag-select past an edge: history scrolls under the pointer
        scroll_by(m, m.sel_edge * static_cast<float>(t.dt));
        const int col = cell_of(m.sel_pos.x + m.metrics.cell_w * 0.5f, m.metrics.origin_x, m.metrics.cell_w, m.term->cols() + 1);
        m.term->select_extend(col, view_row(m, m.sel_pos.y));
    }
    {   // the tab strip eases in/out (the grid refits as it does); a switched tab slides in
        const float goal = (m.cfg.tabs_always || m.tab_count() > 1) ? 1.0f : 0.0f;
        if (m.strip_t != goal) {
            m.strip_t += (goal - m.strip_t) * (1.0f - std::exp(-mdt * 18.0f));
            if (std::fabs(goal - m.strip_t) < 0.01f) m.strip_t = goal;
            fit(m);
        }
        if (m.switch_t < 1.0f) m.switch_t = std::min(1.0f, m.switch_t + mdt * 6.0f);
        if (m.show_numbers && m.time - m.numbers_at > 1.2) m.show_numbers = false;   // no key-up events: a timed hint
    }
    {   // the blink fades; the settings panel and new lines glide
        const auto toward = [&](float& v, float goal, float rate) {
            v += (goal - v) * (1.0f - std::exp(-static_cast<float>(t.dt) * rate));
            if (std::fabs(v - goal) < 0.01f) v = goal;
        };
        if (m.cfg.blink_style == BlinkStyle::fade)
            toward(m.cur_a, m.cursor_on || !m.focused || !m.cfg.cursor_blink ? 1.0f : 0.0f, 16.0f * m.cfg.motion_speed);
        toward(m.settings_t, m.settings_open ? 1.0f : 0.0f, m.cfg.motion ? 22.0f : 1000.0f);
        toward(m.out_shift, 0.0f, 22.0f);
        toward(m.ask_t, m.ask != Model::Ask::none ? 1.0f : 0.0f, m.cfg.motion ? 20.0f : 1000.0f);
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
    const float cw = m.metrics.cell_w, chh = m.metrics.cell_h;
    // the cursor moved because you typed: the effect plays at its new cell
    if (cur.col != m.fx.last_col || cur.row != m.fx.last_row) {
        const bool typed = m.time - m.fx.typed_at < 0.35;
        if (typed && m.fx.last_col >= 0 && m.cfg.cursor_fx != CursorFx::none && m.term->scroll_offset() == 0 && cur.visible)
            motion::emit(m.fx, m.cfg.cursor_fx, m.metrics.origin_x + (tx + 0.5f) * cw, m.metrics.origin_y + (ty + 0.5f) * chh,
                         (tx - static_cast<float>(m.fx.last_col)) * cw, (ty - static_cast<float>(m.fx.last_row)) * chh, cw, chh,
                         m.time - m.fx.enter_at < 0.35, m.palette);
        m.fx.last_col = cur.col;
        m.fx.last_row = cur.row;
    }
    motion::step(m.fx, mdt);
    if (m.cfg.cursor_anim == CursorAnim::none || !m.cur_known) {
        m.cur_x = tx; m.cur_y = ty; m.cur_known = true;
        m.fx.tx = tx; m.fx.ty = ty;
        return {};
    }
    motion::move_cursor(m.fx, m.cfg.cursor_anim, m.cur_x, m.cur_y, tx, ty, mdt);
    {   // the tail follows the head (smear: the head IS the cell)
        const bool smear = m.cfg.cursor_anim == CursorAnim::smear;
        motion::move_tail(m.fx, smear ? tx : m.cur_x, smear ? ty : m.cur_y, m.cfg.cursor_trail, mdt);
    }
    if (!motion::cursor_settled(m.fx, m.cfg.cursor_anim, m.cur_x, m.cur_y, tx, ty) && m.term->scroll_offset() == 0)
        motion::travel(m.fx, m.cfg.cursor_fx, m.metrics.origin_x + (m.cur_x + 0.5f) * cw, m.metrics.origin_y + (m.cur_y + 0.5f) * chh,
                       chh, m.palette);
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

// ── copy mode ──────────────────────────────────────────────────────────────

namespace {
bool word_ch(char32_t c) { return c != U' ' && c != U'\0' && c != U'\t'; }

/// Keep the copy-mode cursor on screen: scroll (smoothly) just enough.
void cm_reveal(App::Model& m) {
    const auto info = m.term->scroll_info();
    const std::int64_t first = static_cast<std::int64_t>(info.total - info.visible) - static_cast<std::int64_t>(std::lround(m.scroll_goal));
    if (m.cm_row < first) App::scroll_by(m, static_cast<float>(first - m.cm_row));
    else if (m.cm_row >= first + info.visible) App::scroll_by(m, -static_cast<float>(m.cm_row - (first + info.visible) + 1));
}

void cm_select(App::Model& m) {
    if (m.cm_vis == 0) { m.term->select_clear(); return; }
    m.term->select_abs(m.cm_arow, m.cm_acol, m.cm_row, m.cm_col, m.cm_vis == 2 ? 1 : m.cm_vis == 3 ? 2 : 0);
}
}  // namespace

App::Cmd App::update(Model& m, CopyKey k) {
    using K = Key;
    const std::int64_t last = std::max<std::int64_t>(m.term->total_rows() - 1, 0);
    const int maxc = std::max(m.term->cols() - 1, 0);
    const std::string& t = k.text;
    const auto ln = [&](std::int64_t r) { return m.term->line(r); };
    const auto exit_mode = [&] { m.copy_mode = false; m.cm_vis = 0; };
    auto yank = [&]() -> Cmd {
        Cmd c = update(m, Copy{});
        exit_mode();
        return c;
    };
    // leaving
    if (k.key == K::escape || t == "q") {
        if (m.cm_vis != 0 && k.key == K::escape) { m.cm_vis = 0; m.term->select_clear(); return {}; }
        exit_mode();
        m.term->select_clear();
        return {};
    }
    if (t == "y" || k.key == K::enter) {
        if (m.cm_vis == 0) {   // nothing marked: yank the line under the cursor
            m.term->select_abs(m.cm_row, 0, m.cm_row, maxc, 1);
        }
        return yank();
    }
    // visual modes
    const auto vis = [&](int mode) {
        if (m.cm_vis == mode) { m.cm_vis = 0; m.term->select_clear(); return; }
        if (m.cm_vis == 0) { m.cm_arow = m.cm_row; m.cm_acol = m.cm_col; }
        m.cm_vis = mode;
    };
    if (t == "v") vis(1);
    else if (t == "V") vis(2);
    else if (k.key == K::v && k.mods.ctrl) vis(3);
    // motions
    else if (t == "h" || k.key == K::left) m.cm_col = std::max(m.cm_col - 1, 0);
    else if (t == "l" || k.key == K::right) m.cm_col = std::min(m.cm_col + 1, maxc);
    else if (t == "k" || k.key == K::up) m.cm_row = std::max<std::int64_t>(m.cm_row - 1, 0);
    else if (t == "j" || k.key == K::down) m.cm_row = std::min(m.cm_row + 1, last);
    else if ((k.key == K::u && k.mods.ctrl) || k.key == K::page_up) m.cm_row = std::max<std::int64_t>(m.cm_row - std::max(m.term->rows() / 2, 1), 0);
    else if ((k.key == K::d && k.mods.ctrl) || k.key == K::page_down) m.cm_row = std::min(m.cm_row + std::max(m.term->rows() / 2, 1), last);
    else if (t == "0" || k.key == K::home) m.cm_col = 0;
    else if (t == "^") { const auto l = ln(m.cm_row); int c = 0; while (c < static_cast<int>(l.size()) && !word_ch(l[static_cast<std::size_t>(c)])) ++c; m.cm_col = std::min(c, maxc); }
    else if (t == "$" || k.key == K::end) { const auto l = ln(m.cm_row); int c = static_cast<int>(l.size()) - 1; while (c > 0 && !word_ch(l[static_cast<std::size_t>(c)])) --c; m.cm_col = std::max(c, 0); }
    else if (t == "g") { m.cm_row = 0; m.cm_col = 0; }
    else if (t == "G") { m.cm_row = m.term->cursor_abs(); m.cm_col = m.term->cursor().col; }
    else if (t == "w" || t == "e") {   // next word start / end, across lines
        std::int64_t r = m.cm_row; int c = m.cm_col;
        auto l = ln(r);
        const auto at = [&](int x) { return x < static_cast<int>(l.size()) && word_ch(l[static_cast<std::size_t>(x)]); };
        if (t == "w") {
            while (at(c)) ++c;
            while (true) {
                while (c < static_cast<int>(l.size()) && !at(c)) ++c;
                if (c < static_cast<int>(l.size()) || r >= last) break;
                ++r; l = ln(r); c = 0;
            }
        } else {
            ++c;
            while (true) {
                while (c < static_cast<int>(l.size()) && !at(c)) ++c;
                if (c < static_cast<int>(l.size()) || r >= last) break;
                ++r; l = ln(r); c = 0;
            }
            while (at(c + 1)) ++c;
        }
        m.cm_row = r; m.cm_col = std::clamp(c, 0, maxc);
    } else if (t == "b") {   // previous word start, across lines
        std::int64_t r = m.cm_row; int c = m.cm_col - 1;
        auto l = ln(r);
        const auto at = [&](int x) { return x >= 0 && x < static_cast<int>(l.size()) && word_ch(l[static_cast<std::size_t>(x)]); };
        while (true) {
            while (c >= 0 && !at(c)) --c;
            if (c >= 0 || r == 0) break;
            --r; l = ln(r); c = static_cast<int>(l.size()) - 1;
        }
        while (at(c - 1)) --c;
        m.cm_row = r; m.cm_col = std::clamp(c, 0, maxc);
    } else if (t == "{" || t == "}") {   // the previous / next prompt
        std::int64_t best = -1;
        for (const auto& cmd : m.term->commands()) {
            const std::int64_t pr = cmd.prompt_row;
            if (pr < 0) continue;
            if (t == "{" && pr < m.cm_row && pr > best) best = pr;
            if (t == "}" && pr > m.cm_row && (best < 0 || pr < best)) best = pr;
        }
        if (best >= 0) { m.cm_row = best; m.cm_col = 0; }
    } else if (t == "o" && m.cm_vis != 0) {   // swap to the other end
        std::swap(m.cm_row, m.cm_arow);
        std::swap(m.cm_col, m.cm_acol);
    } else if (t == "/") {
        exit_mode();
        return update(m, Search{0});
    } else {
        return {};
    }
    cm_select(m);
    cm_reveal(m);
    m.grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, RequestQuit) {
    if (m.cfg.close_confirm != CloseConfirm::never && m.ask == Model::Ask::none) {
        bool busy = false;
        for (std::size_t i = 0; i < m.tab_count(); ++i) busy = busy || !busy_of(m.at(i), m.time, nullptr).empty();
        if (busy || m.cfg.close_confirm == CloseConfirm::always) {
            m.ask = Model::Ask::quit;
            m.ask_focus = 1;
            m.ask_remember = false;
            return {};
        }
    }
    if (!m.cfg.demo) (void)config_save(m.cfg, config_path());
    return Cmd::quit();
}

App::Cmd App::update(Model& m, Answer a) {
    const auto kind = m.ask;
    if (kind == Model::Ask::none) return {};
    if (a.action == 100) { m.ask_remember = !m.ask_remember; return {}; }
    m.ask = Model::Ask::none;
    const bool yes = a.action > 0;
    if (yes && m.ask_remember) {   // "don't ask again", remembered in hand.vibe
        if (kind == Model::Ask::paste) m.cfg.paste_confirm = PasteConfirm::risky;
        else m.cfg.close_confirm = CloseConfirm::never;
        if (!m.cfg.demo) (void)config_save(m.cfg, config_path());
    }
    switch (kind) {
        case Model::Ask::paste: {
            std::string text = std::exchange(m.pending_paste, {});
            if (!yes) return {};
            if (a.action == 1) {   // one line: newlines become spaces, nothing runs until you press enter
                std::string one;
                for (const auto& l : split_lines(text)) { if (!one.empty() && !l.empty()) one += ' '; one += l; }
                text = one;
            }
            return update(m, Pasted{std::move(text), true});
        }
        case Model::Ask::close_tab:
            return yes ? update(m, CloseTab{m.ask_tab, true}) : Cmd{};
        case Model::Ask::quit:
            if (!yes) return {};
            if (!m.cfg.demo) (void)config_save(m.cfg, config_path());
            return Cmd::quit();
        default: return {};
    }
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
    const auto si = m.term->selection_info();
    m.copy_flash_at = m.time;
    m.toast = Toast{.title = "copied", .body = std::to_string(si.lines) + (si.lines == 1 ? " line, " : " lines, ") +
                                             std::to_string(si.chars) + " chars" + (si.block ? " (block)" : ""),
                    .kind = Toast::Kind::info};
    m.toast_at = m.time;
    return WriteClipboard{m.term->selection_text()};
}
App::Cmd App::update(Model&, PasteReq) {
    return ReadClipboard<Msg>{[](std::string s) -> Msg { return Pasted{std::move(s)}; }};
}
App::Cmd App::update(Model& m, Pasted p) {
    if (p.text.empty()) return {};
    // Ask first (paste.confirm): risky lines, multi-line, or only into programs
    // that would run each line as it lands.
    if (m.ask != Model::Ask::none) return {};   // one question at a time
    if (!p.confirmed) {
        std::string body = p.text;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
        const bool multi = body.find('\n') != std::string::npos;
        bool risky = false;
        for (const auto& l : split_lines(body)) risky = risky || risk_of(l).flag;
        bool ask = false;
        switch (m.cfg.paste_confirm) {
            case PasteConfirm::never: break;
            case PasteConfirm::risky: ask = risky; break;
            case PasteConfirm::multiline: ask = multi || risky; break;
            case PasteConfirm::unbracketed: ask = (multi || risky) && !m.term->bracketed(); break;
        }
        if (ask) {
            m.pending_paste = p.text;
            m.ask = Model::Ask::paste;
            m.ask_focus = 2;
            m.ask_remember = false;
            return {};
        }
    }
    if (!m.pty) return {};
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
        mm.fx.typed_at = mm.active_at = mm.time;   // the motion effects follow keystrokes, pty or not
        if (b.find('\r') != std::string::npos) mm.fx.enter_at = mm.time;
        if (!mm.pty) return;
        mm.term->select_clear();
        mm.term->scroll_to_bottom();   // typing snaps back to the live screen
        mm.scroll_d = mm.scroll_goal = 0.0f;
        mm.scroll_applied = 0;
        mm.new_below = 0;
        mm.cursor_on = true;
        mm.cur_a = 1.0f;
        mm.fx.typed_at = mm.active_at = mm.time;
        if (b.find('\r') != std::string::npos) mm.fx.enter_at = mm.time;
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
    if (m.ask != Model::Ask::none) {   // the dialog has the keyboard
        const int n = m.ask == Model::Ask::paste ? 3 : 2;
        if (k.key == Key::escape) return Answer{-1};
        if (k.key == Key::enter) return Answer{m.ask == Model::Ask::paste && md.shift ? 1 : m.ask_focus};
        if (k.key == Key::tab || k.key == Key::right || k.key == Key::left) {
            const int d = (k.key == Key::left || (k.key == Key::tab && md.shift)) ? -1 : 1;
            return E{[d, n](Model& mm) { mm.ask_focus = ((mm.ask_focus + d) % n + n) % n; }};
        }
        if (k.key == Key::space) return Answer{100};
        return E{[](Model&) {}};
    }
    if (cs && k.key == Key::m)   // copy mode: vim keys through the scrollback
        return E{[](Model& mm) {
            if (mm.copy_mode) { mm.copy_mode = false; mm.cm_vis = 0; mm.term->select_clear(); return; }
            mm.copy_mode = true;
            mm.cm_vis = 0;
            const auto si = mm.term->selection_info();
            if (si.any) { mm.cm_row = si.r1; mm.cm_col = si.c1; }
            else { mm.cm_row = mm.term->cursor_abs(); mm.cm_col = mm.term->cursor().col; }
            mm.grid_key = 0;
        }};
    if (m.copy_mode) {
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt) return std::nullopt;
        if (md.ctrl && (k.key == Key::v || k.key == Key::u || k.key == Key::d)) return CopyKey{k.key, md, {}};
        switch (k.key) {   // named keys; letters arrive as text
            case Key::escape: case Key::enter: case Key::left: case Key::right: case Key::up: case Key::down:
            case Key::home: case Key::end: case Key::page_up: case Key::page_down:
                return CopyKey{k.key, md, {}};
            default: return E{[](Model&) {}};   // nothing reaches the shell in copy mode
        }
    }
    if (k.key == Key::ctrl && !m.ctrl_held) return E{[](Model& mm) { mm.ctrl_held = true; }};
    // ── tabs: kitty/foot-style chords, plus the browser ones ──────────────
    if (k.key == Key::alt && m.tab_count() > 1 && !m.show_numbers) return E{[](Model& mm) { mm.show_numbers = true; mm.numbers_at = mm.time; }};
    if (cs && k.key == Key::t) return NewTab{};
    if (cs && k.key == Key::w) return CloseTab{m.active};
    if (md.ctrl && k.key == Key::tab) return StepTab{md.shift ? -1 : 1};
    if (cs && (k.key == Key::page_down || k.key == Key::page_up)) return MoveTab{k.key == Key::page_down ? 1 : -1};
    if (md.ctrl && !md.shift && (k.key == Key::page_down || k.key == Key::page_up)) return StepTab{k.key == Key::page_down ? 1 : -1};
    if (cs && (k.key == Key::right || k.key == Key::left)) return StepTab{k.key == Key::right ? 1 : -1};
    if (md.alt && !md.ctrl && !md.shift && k.key >= Key::n1 && k.key <= Key::n9) {
        const auto i = static_cast<std::size_t>(static_cast<int>(k.key) - static_cast<int>(Key::n1));
        if (m.tab_count() > 1) return SelectTab{k.key == Key::n9 ? m.tab_count() - 1 : std::min(i, m.tab_count() - 1)};
    }
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
    if (m.ask != Model::Ask::none) return E{[](Model&) {}};
    if (m.copy_mode && !m.settings_open) return CopyKey{Key::unknown, {}, t.text};
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

std::optional<App::Msg> App::on(const Model& m, const ScrollEvent& s_win) {
    if (in_strip(m, s_win.position.y)) return std::nullopt;   // the tab strip's
    ScrollEvent s = s_win;
    s.position.y -= term_top(m);   // terminal coordinates from here on
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
        mm.win = s;
        fit(mm);
        if (mm.sized_once && mm.term && (mm.term->cols() != c0 || mm.term->rows() != r0)) mm.resized_at = mm.time;
        mm.sized_once = true;
    }};
}

std::optional<App::Msg> App::on(const Model& m, const FocusEvent& f) {
    return E{[on = f.focused, rep = m.term->encode_focus(f.focused)](Model& mm) {
        mm.focused = on;
        mm.cursor_on = true;
        mm.cur_a = 1.0f;
        if (!rep.empty() && mm.pty) mm.pty->write(rep);
    }};
}

std::optional<App::Msg> App::on(const Model& m, const MouseDown& e_win) {
    if (in_strip(m, e_win.position.y)) return std::nullopt;   // the tab strip's
    MouseDown e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
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
    if (btn == 0) {
        const int vr = view_row(m, e.position.y);
        // the selection toolbar is a control, not text
        if (m.sel_chip.size.x > 0.0f && m.sel_chip.contains(e.position)) return std::nullopt;
        // shift+click: extend what is selected to here
        if (e.mods.shift && m.term->has_selection() && e.click_count == 1)
            return E{[col, vr, p = e.position](Model& mm) {
                mm.term->select_extend(col, vr);
                mm.selecting = true; mm.sel_pos = p; mm.sel_edge = 0.0f;
            }};
        // a click in the gutter on a prompt, or a fourth click: the whole command's output
        const bool gutter = m.cfg.shell_marks && e.position.x < m.metrics.origin_x;
        if (gutter || e.click_count >= 4) {
            const std::int64_t abs = m.term->view_to_abs(vr);
            for (const auto& c : m.term->commands()) {
                if (!c.ran || c.prompt_row < 0) continue;
                const std::int64_t end = c.finished && c.end_row > c.prompt_row ? c.end_row : m.term->total_rows();
                const bool hit = gutter ? abs == c.prompt_row : abs >= c.prompt_row && abs < end;
                if (!hit || c.output_row < 0 || end - 1 < c.output_row) continue;
                return E{[o0 = c.output_row, o1 = end - 1](Model& mm) {
                    mm.term->select_abs(o0, 0, o1, mm.term->cols() - 1, 1);
                    mm.selecting = false;
                }};
            }
            if (gutter) return std::nullopt;
        }
        return E{[col, row = vr, n = e.click_count, p = e.position, block = e.mods.alt](Model& mm) {
            mm.term->select_begin(col, row, n, block);   // alt+drag: a rectangle
            mm.selecting = true;
            mm.sel_clicks = block ? 0 : n;
            mm.sel_pos = p;
            mm.sel_edge = 0.0f;
        }};
    }
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const MouseMove& e_win) {
    if (in_strip(m, e_win.position.y) && m.mouse_button < 0 && !m.selecting) return std::nullopt;   // the tab strip's
    MouseMove e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
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

std::optional<App::Msg> App::on(const Model& m, const MouseUp& e_win) {
    if (in_strip(m, e_win.position.y) && m.mouse_button < 0 && !m.selecting) return std::nullopt;   // the tab strip's
    MouseUp e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    if (m.mouse_button >= 0) {
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        auto bytes = m.term->encode_mouse(m.mouse_button, col, row, false, false, e.mods);
        return E{[b = std::move(bytes)](Model& mm) { mm.mouse_button = -1; if (mm.pty && !b.empty()) mm.pty->write(b); }};
    }
    if (m.selecting) {
        // a plain click that never left its cell selects nothing: no highlight, no toolbar, no copy
        if (m.sel_clicks == 1) {
            const auto si = m.term->selection_info();
            if (si.any && si.r0 == si.r1 && si.c0 == si.c1)
                return E{[](Model& mm) { mm.term->select_clear(); mm.selecting = false; mm.sel_edge = 0.0f; mm.sel_clicks = 0; }};
        }
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

/// The tab strip: press selects (instantly, like a browser), drag reorders.
std::optional<App::Msg> App::on(const Model& m, const Pressed& p) {
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        if (p.node == part_id("tabs", i) && p.button == MouseButton::left)
            return E{[i](Model& mm) {
                (void)update(mm, SelectTab{i});
                mm.drag_tab = static_cast<int>(mm.active);
                mm.drag_dx = 0.0f;
            }};
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const Dragged& d) {
    if (m.drag_tab < 0) return std::nullopt;
    return E{[dx = d.delta.x](Model& mm) {
        mm.drag_dx += dx;
        // past half a tab: swap with the neighbour (the pill slides after it)
        const float tab_w = std::clamp(mm.win.x / static_cast<float>(mm.tab_count()), 96.0f, 236.0f);
        if (mm.drag_dx > tab_w * 0.5f && mm.active + 1 < mm.tab_count()) { (void)update(mm, MoveTab{1}); mm.drag_dx -= tab_w; }
        else if (mm.drag_dx < -tab_w * 0.5f && mm.active > 0) { (void)update(mm, MoveTab{-1}); mm.drag_dx += tab_w; }
        mm.drag_tab = static_cast<int>(mm.active);
    }};
}

std::optional<App::Msg> App::on(const Model& m, const Released&) {
    if (m.drag_tab < 0) return std::nullopt;
    return E{[](Model& mm) { mm.drag_tab = -1; mm.drag_dx = 0.0f; }};
}

/// The tab strip's buttons, and the search bar's (mayag term_search parts).
std::optional<App::Msg> App::on(const Model& m, const Clicked& c) {
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (c.node == part_id("tabs", tab_part_close + i)) return CloseTab{i};
        if (c.node == part_id("tabs", i) && c.button == MouseButton::middle) return CloseTab{i};
        if (c.node == part_id("tabs", i) && c.count == 2) return NewTab{};   // double-click a tab: a new one beside it
    }
    if (c.node == part_id("tabs", part_inc) || (c.node == node_id("tabs") && c.count == 2)) return NewTab{};
    if (m.ask != Model::Ask::none) {
        for (int i = 0; i < 3; ++i) if (c.node == part_id("ask", static_cast<std::size_t>(i))) return Answer{i};
        if (c.node == part_id("ask", 100)) return Answer{100};
    }
    if (c.node == node_id("sel.copy")) return Copy{};
    if (c.node == node_id("sel.find")) return Search{0};
    if (c.node == node_id("sel.open")) return OpenUrl{m.term->selection_text()};
    if (c.node == node_id("sel.clear")) return E{[](Model& mm) { mm.term->select_clear(); }};
    if (!m.search_open) return std::nullopt;
    if (c.node == part_id("search", 5)) return Search{-1};
    if (c.node == part_id("search", 6)) return Search{1};
    if (c.node == part_id("search", 9)) return Search{0};
    return std::nullopt;
}

// ── subscriptions, theme, view ─────────────────────────────────────────────

App::Sub App::subscribe(const Model& m) {
    std::vector<Sub> s;
    // every tab's shell keeps running: one reader per session, keyed by its id
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Session& se = m.at(i);
        if (se.pty) s.push_back(Sub::stream("pty." + std::to_string(se.sid), read_pty, se.pty->fd(),
                                            static_cast<int>(se.pty->pid()), se.sid));
    }
    if (m.tab_count() > 1) s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));   // activity sparklines
    if (m.focused && m.cfg.cursor_blink)
        s.push_back(Sub::every(std::chrono::milliseconds(static_cast<int>(m.cfg.blink_ms)), Msg{Blink{}}));
    const auto cur = m.term->cursor();
    const bool gliding = m.cfg.cursor_anim != CursorAnim::none &&
                         (!m.cur_known || !motion::cursor_settled(m.fx, m.cfg.cursor_anim, m.cur_x, m.cur_y,
                                                                  static_cast<float>(cur.col), static_cast<float>(cur.row)));
    const bool breathing = m.cfg.blink_style == BlinkStyle::breathe && m.cfg.cursor_blink && m.focused;
    const bool tailing = m.cfg.cursor_anim != CursorAnim::none && m.cfg.cursor_trail > 0.0f &&
                         !motion::tail_settled(m.fx, m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.col) : m.cur_x,
                                               m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.row) : m.cur_y);
    const bool strip_goal = m.cfg.tabs_always || m.tab_count() > 1;
    const bool animating = gliding || breathing || tailing || m.fx.busy() || m.ask != Model::Ask::none || m.ask_t > 0.0f || m.time - m.copy_flash_at < 0.5 || m.tab_motion.animating() ||
                           m.strip_t != (strip_goal ? 1.0f : 0.0f) || m.switch_t < 1.0f || m.drag_tab >= 0 || m.show_numbers || m.scroll_d != m.scroll_goal || m.out_shift != 0.0f ||
                           (m.bell_at >= 0.0 && m.time - m.bell_at < 0.45) ||
                           (m.cfg.blink_style == BlinkStyle::fade && m.cur_a != (m.cursor_on || !m.focused || !m.cfg.cursor_blink ? 1.0f : 0.0f)) ||
                           m.settings_t != (m.settings_open ? 1.0f : 0.0f) || (m.selecting && m.sel_edge != 0.0f) ||
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
    Theme t = derive(Seed{.accent = rgb_of(p.ansi[4]), .mode = p.light ? Mode::light : Mode::dark,
                          .roundness = std::max(m.cfg.roundness, 0.0f)});
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

namespace {

/// "~/src/hand" for the strip's second line.
std::string tab_dir(const std::string& cwd) {
    std::string p = cwd;
    if (const char* h = std::getenv("HOME"); h && *h && p.rfind(h, 0) == 0) p = "~" + p.substr(std::strlen(h));
    return p;
}

/// The window: the tab strip (once there are two tabs) over the terminal,
/// which slides in from the side it came from when you switch.
Node frame(const Ctx& c, const App::Model& m, Node term) {
    const float sh = m.strip_h();
    const float e = 1.0f - (1.0f - m.switch_t) * (1.0f - m.switch_t) * (1.0f - m.switch_t);   // ease-out cubic
    if (m.switch_t < 1.0f)
        term = (node(std::move(term)) | translate(static_cast<float>(m.switch_dir) * (1.0f - e) * 42.0f, 0.0f)
                | opacity(0.35f + 0.65f * e)).build();
    if (sh < 0.5f) return term;

    std::vector<TermTabInfo> infos;
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const App::Session& se = m.at(i);
        TermTabInfo ti;
        const std::string proc = fg_process(se, m.time);
        const std::string cwd = se.term ? se.term->cwd() : std::string{};
        // the title: your name for it > the running command > the program > the shell's title > the folder
        std::string running_cmd;
        if (se.term) {
            const auto cs = se.term->commands();
            for (auto it = cs.rbegin(); it != cs.rend(); ++it)
                if (it->ran) { if (!it->finished) running_cmd = se.term->command_text(*it); break; }
        }
        const bool shell_proc = proc.empty() || proc == "fish" || proc == "zsh" || proc == "bash" || proc == "sh";
        std::string base = cwd.empty() ? std::string{} : cwd.substr(cwd.find_last_of('/') + 1);
        if (base.empty() && !cwd.empty()) base = "/";
        // a shell title that is just the directory ("~/P/hand") says less than the folder
        const bool path_title = se.title.empty() || se.title == "hand" || se.title[0] == '~' || se.title[0] == '/' ||
                                se.title.find(cwd.empty() ? "\x01" : base) != std::string::npos;
        const std::string folder = !base.empty() ? base : "shell";
        switch (m.cfg.tab_title) {
            case TabTitle::folder: ti.title = folder; break;
            case TabTitle::program: ti.title = !shell_proc ? proc : proc.empty() ? folder : proc; break;
            case TabTitle::shell: ti.title = se.title.empty() ? folder : se.title; break;
            case TabTitle::smart:
                ti.title = !running_cmd.empty() ? running_cmd : !shell_proc ? proc : !path_title ? se.title : folder;
                break;
        }
        if (!se.name.empty()) ti.title = se.name;
        ti.cwd = tab_dir(cwd);
        ti.process = proc;
        ti.unread = se.unread && i != m.active;
        ti.bell = se.bell && i != m.active;
        ti.activity = se.activity;
        // the last command: running now, or how it ended
        if (se.term) {
            const auto cs = se.term->commands();
            for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
                if (!it->ran) continue;
                ti.running = !it->finished;
                if (it->finished) {
                    ti.last_exit = it->exit_code;
                    ti.finished_ago = it->start_ms > 0 ? static_cast<double>(wall_ms() - it->start_ms - it->duration_ms) / 1000.0 : 99.0;
                }
                if (!it->finished && it->start_ms > 0) ti.elapsed = static_cast<double>(wall_ms() - it->start_ms) / 1000.0;
                break;
            }
        }
        infos.push_back(std::move(ti));
    }
    const float full = std::clamp(m.cfg.tab_height, 26.0f, 60.0f);
    Node strip = tab_strip(c, "tabs", infos, static_cast<int>(m.active), &m.tab_motion, m.time,
                           {.show_numbers = m.show_numbers,
                            .max_tab = m.cfg.tab_width, .min_tab = std::min(96.0f, m.cfg.tab_width),
                            .height = full,
                            .style = static_cast<TabStripStyle>(m.cfg.tab_style),
                            .radius = m.cfg.tab_radius >= 0.0f ? m.cfg.tab_radius : 10.0f * m.cfg.roundness,
                            .icon = m.cfg.tab_icon, .subtitle = m.cfg.tab_subtitle && full >= 36.0f,
                            .activity = m.cfg.tab_activity, .hue = m.cfg.tab_colors, .new_button = m.cfg.tab_new_button,
                            .close = static_cast<mayag::dsl::TabClose>(m.cfg.tab_close),
                            .motion = static_cast<mayag::dsl::TabMotion>(m.cfg.tab_anim), .number_prefix = "alt+",
                            .bottom = m.cfg.tab_bottom});
    // the strip eases in (from the top edge, or up from the bottom) as the second tab opens
    strip = (node(std::move(strip)) | translate(0.0f, m.cfg.tab_bottom ? 0.0f : sh - full)
             | opacity(std::clamp(m.strip_t * 1.4f, 0.0f, 1.0f))).build();
    Node bar = (v(std::move(strip)) | height(sh) | width(pct(100)) | clip).build();
    if (m.cfg.tab_bottom) return (v(std::move(term), std::move(bar)) | size(m.win.x, m.win.y)).build();
    return (v(std::move(bar), std::move(term)) | size(m.win.x, m.win.y)).build();
}

}  // namespace

Node App::view(const Model& m) {
    const Ctx& c = ctx();
    const Config& cfg = m.cfg;
    const bool app_shape = m.term->app_cursor_shape();
    // mid-glide the view sits between two rows: draw one extra row above and slide
    const float frac = draw_frac(m);
    const bool above = frac > 0.001f;
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
    const float bell = m.bell_at >= 0.0 && cfg.visual_bell ? static_cast<float>(std::max(0.0, 1.0 - (m.time - m.bell_at) / 0.2)) : 0.0f;
    const float ch = m.metrics.cell_h;
    TermViewOptions o{.font_size = m.font_size, .line_height = cfg.line_height, .padding = cfg.padding,
                      .focused = m.focused,
                      .cursor_on = cfg.blink_style != BlinkStyle::hard ? true : (m.cursor_on || !m.focused || !cfg.cursor_blink),
                      .dim_unfocused = m.focused ? 0.0f : cfg.dim_unfocused, .bell = bell, .marks = cfg.shell_marks,
                      .opacity = 0.0f, .family = family_of(cfg)};   // the window clear is the paper
    th.mark_prompt = p.ansi[8]; th.mark_ok = p.ansi[2]; th.mark_error = p.ansi[1]; th.mark_running = p.ansi[4];
    th.match_bg = p.selection; th.hint_bg = p.ansi[3]; th.hint_fg = p.bg; th.link = p.ansi[6];
    if (cfg.cursor_anim != CursorAnim::none && cfg.cursor_anim != CursorAnim::smear && m.cur_known) {
        const auto cur = m.term->cursor();
        o.cursor_dx = m.cur_x - static_cast<float>(cur.col);
        o.cursor_dy = m.cur_y - static_cast<float>(cur.row);
        o.cursor_trail = false;   // the tail is hand's (motion.trail), drawn below
    }
    o.cursor_glow = cfg.cursor_glow ? 1.0f : 0.0f;
    o.y_offset = above ? (frac - 1.0f) * ch : 0.0f;   // row 0 is the one above: start it a row up, slide down by frac
    {   // the blink
        float a = 1.0f;
        if (cfg.blink_style == BlinkStyle::fade) a = m.cur_a;
        else if (cfg.blink_style == BlinkStyle::breathe && cfg.cursor_blink && m.focused && m.time - m.active_at > 0.6) {
            const double period = 2.0 * static_cast<double>(cfg.blink_ms) / 1000.0 / static_cast<double>(cfg.motion_speed);
            a = 0.62f + 0.38f * static_cast<float>(std::cos(6.2831853 * (m.time - m.active_at - 0.6) / period));
        }
        o.cursor_alpha = std::max(a, (o.cursor_dx != 0.0f || o.cursor_dy != 0.0f) ? 1.0f : 0.0f);
    }

    if (m.settings_open || m.settings_t > 0.0f) {
        const auto specs = setting_specs(cfg);
        const bool nerd = c.theme.icons == IconSet::nerd;
        SettingsPanelProps props{.title = "hand", .subtitle = "terminal settings", .brand = nerd ? "\uF256" : "h",
                                 .logo = hand_logo(p, 48.0f, m.cursor_on, m.cfg.roundness), .footer = config_path(),
                                 .width = std::min(860.0f, m.size.x - 40.0f), .height = std::min(600.0f, m.size.y - 40.0f)};
        const float e = 1.0f - (1.0f - m.settings_t) * (1.0f - m.settings_t);   // ease-out
        c.overlay(Overlay{
            .content = (node(settings_panel(c, "settings", specs, m.panel, props, [](SettingsOp op) -> Msg { return Panel{std::move(op)}; }))
                        | opacity(e) | translate(0.0f, (1.0f - e) * 16.0f)).build(),
            .id = part_id("settings", part_menu),
            .placement = Placement::centered,
            .dismiss = m.settings_open ? Dismiss::modal : Dismiss::never,
            .layer = 10,
            .scrim = 0.35f * e,
            .passthrough = !m.settings_open});   // closing: already out of the way
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
    if (cfg.bell_shake && m.bell_at >= 0.0 && m.time - m.bell_at < 0.4) {   // the shake: a damped wobble
        const float tb = static_cast<float>(m.time - m.bell_at);
        const float k = (1.0f - tb / 0.4f) * (1.0f - tb / 0.4f);
        term = (node(std::move(term)) | translate(7.0f * k * std::sin(tb * 62.0f), 2.0f * k * std::sin(tb * 47.0f))).build();
    }
    std::vector<Node> layers;
    layers.push_back(std::move(term));
    if (cfg.cursor_anim != CursorAnim::none && cfg.cursor_trail > 0.0f && m.cur_known && m.focused &&
        m.term->scroll_offset() == 0 && m.term->cursor().visible) {
        const auto cur = m.term->cursor();
        const float cw = m.metrics.cell_w, ox = m.metrics.origin_x, oy = m.metrics.origin_y;
        const float tailx = ox + m.fx.tx * cw, taily = oy + m.fx.ty * ch;
        std::vector<Node> tail = cfg.cursor_anim == CursorAnim::smear
            ? motion::smear(tailx, taily, ox + static_cast<float>(cur.col) * cw, oy + static_cast<float>(cur.row) * ch, cw, ch, p.cursor)
            : motion::comet(ox + m.cur_x * cw, oy + m.cur_y * ch, tailx, taily, cw, ch, p.cursor,
                            0.55f + 0.15f * cfg.cursor_trail);
        for (auto& n : tail) layers.push_back(std::move(n));
    }
    for (auto& n : motion::draw(m.fx)) layers.push_back(std::move(n));

    // ── selection & text: flash, copy mode, toolbar, paste guard ──────────
    {
        const float cw = m.metrics.cell_w, ox = m.metrics.origin_x;
        const std::int64_t top_abs = m.term->view_to_abs(0);
        const float shift = above ? 1.0f : 0.0f;
        const auto row_y = [&](std::int64_t abs) {
            return m.metrics.origin_y + o.y_offset + (static_cast<float>(abs - top_abs) + shift) * ch;
        };
        const auto ink = rgb_of(p.fg), paper = rgb_of(p.bg);
        const auto dim = mix(ink, paper, 0.45f);
        const float fs = m.font_size * 0.85f;
        const float fcw = cell_width(fs, cfg.font_family);
        // the copy flash: what you copied lights up and fades
        if (cfg.sel_flash && m.time - m.copy_flash_at < 0.45 && m.grid) {
            const float a = 1.0f - static_cast<float>((m.time - m.copy_flash_at) / 0.45);
            for (const auto& sp : m.grid->selection)
                layers.push_back((box() | size(static_cast<float>(sp.len) * cw, ch) | radius(2.0f)
                                  | bg(rgb_of(p.ansi[4], 0.55f * a * a))
                                  | absolute(ox + static_cast<float>(sp.col) * cw, m.metrics.origin_y + o.y_offset + static_cast<float>(sp.row) * ch)).build());
        }
        // copy mode: its own cursor, and a mode line
        if (m.copy_mode) {
            const float y = row_y(m.cm_row);
            if (y > -ch && y < m.size.y)
                layers.push_back((box() | size(cw, ch) | bg(rgb_of(p.ansi[3], 0.35f)) | border(2.0f, rgb_of(p.ansi[3]))
                                  | absolute(ox + static_cast<float>(m.cm_col) * cw, y)).build());
            const char* mode = m.cm_vis == 1 ? "VISUAL" : m.cm_vis == 2 ? "V-LINE" : m.cm_vis == 3 ? "V-BLOCK" : "COPY";
            std::vector<HudSeg> segs{{"L" + std::to_string(m.cm_row + 1) + ":" + std::to_string(m.cm_col + 1), ink, true}};
            if (m.cm_vis != 0) {
                const auto si = m.term->selection_info();
                segs.push_back({std::to_string(si.lines) + "L " + std::to_string(si.chars) + "ch", rgb_of(p.ansi[3])});
            }
            segs.push_back({m.cm_vis ? "y yank  o other end  esc" : "hjkl w b e 0 $ { }  v V ^v  y yank  / find  q", dim});
            layers.push_back((hud_chip(c, segs, {.tag = mode, .tone = rgb_of(m.cm_vis ? p.ansi[5] : p.ansi[3]), .size = fs})
                              | absolute(cfg.padding + 10.0f, m.size.y - std::round(fs * 1.75f) - 6.0f - m.tail_t * (std::max(cfg.live_tail, 1.0f) * ch + 9.0f))).build());
        }
        // the selection toolbar: what you have, and what you can do with it
        m.sel_chip = Rect{};
        const auto si = m.term->selection_info();
        if (cfg.sel_toolbar && si.any && !m.selecting && !m.copy_mode && !m.term->alt_screen()) {
            const std::string text = si.lines == 1 ? m.term->selection_text() : std::string{};
            const bool openable = !text.empty() && text.find(' ') == std::string::npos &&
                                  (text.find("://") != std::string::npos || text.rfind("www.", 0) == 0 || text[0] == '/' || text[0] == '~');
            const std::string size = std::to_string(si.lines) + "L " + std::to_string(si.chars) + "ch" + (si.block ? " \u25A6" : "");
            const auto button = [&](const char* id, const char* label, Color<Srgb> tone) {
                const bool hot = c.hovered(node_id(id));
                return (h(text_owned(label) | font(fs) | monospace | fg(hot ? paper : tone))
                        | height(pct(100)) | pad(0, fs * 0.55f) | center | bg(hot ? tone : tone.fade(0.0f))
                        | pointer_cursor | id_of(id)).build();
            };
            std::vector<Node> parts;
            parts.push_back((h(text_owned("SEL") | font(fs) | monospace | semibold | fg(paper))
                             | height(pct(100)) | pad(0, fs * 0.5f) | center | bg(rgb_of(p.ansi[4]))).build());
            parts.push_back((h(text_owned(size) | font(fs) | monospace | fg(ink)) | height(pct(100)) | pad(0, fs * 0.5f) | center).build());
            parts.push_back(button("sel.copy", "copy", rgb_of(p.ansi[2])));
            parts.push_back(button("sel.find", "find", rgb_of(p.ansi[3])));
            if (openable) parts.push_back(button("sel.open", "open", rgb_of(p.ansi[6])));
            parts.push_back(button("sel.clear", "\u00D7", dim));
            const float chip_h = std::round(fs * 1.75f);
            std::size_t chars = 3 + size.size() + 4 + 4 + (openable ? 4 : 0) + 1;
            const float chip_w = static_cast<float>(chars) * fcw + fs * (1.0f + 1.0f + 1.1f * static_cast<float>(openable ? 4 : 3));
            // beside the selection's end when the row has room (covers no text); else
            // under it, or above its start when that would leave the screen
            const auto end_line = m.term->line(si.r1);
            int used = static_cast<int>(end_line.size());
            while (used > 0 && (end_line[static_cast<std::size_t>(used - 1)] == U' ' || end_line[static_cast<std::size_t>(used - 1)] == U'\0')) --used;
            const float after = ox + static_cast<float>(std::max(used, si.c1 + 1)) * cw + 2.0f * cw;
            const float right_edge = m.size.x - (minimap_on(m) ? std::max(m.mm_w, kMinimapRest) + 6.0f : 6.0f);
            float x, y;
            if (after + chip_w < right_edge) {
                x = after;
                y = row_y(si.r1) + (ch - chip_h) * 0.5f;
            } else {
                const float below = row_y(si.r1) + ch + 4.0f, abovey = row_y(si.r0) - chip_h - 4.0f;
                y = below + chip_h < m.size.y - 4.0f ? below : abovey;
                x = std::clamp(ox + static_cast<float>(si.block ? si.c0 : si.lines == 1 ? si.c0 : 0) * cw, 4.0f,
                               std::max(right_edge - chip_w, 4.0f));
            }
            if (y > -chip_h && y < m.size.y) {
                m.sel_chip = Rect{Vec2{x, y}, Vec2{chip_w, chip_h}};
                layers.push_back((h(std::move(parts)) | align(Align::center) | height(chip_h)
                                  | bg(mix(paper, ink, 0.07f)) | border(1.0f, mix(paper, rgb_of(p.ansi[4]), 0.55f))
                                  | absolute(x, y)).build());
            }
        }
        // the confirmation dialog (built while asking, kept while it fades out)
        if (m.ask != Model::Ask::none) {
            TermDialogSpec d;
            d.focused = m.ask_focus;
            d.remember_on = m.ask_remember;
            d.time = m.time;
            using T = DialogAction::Tone;
            if (m.ask == Model::Ask::paste) {
                std::string body = m.pending_paste;
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                const auto lines = split_lines(body);
                int flagged = 0;
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    const Risk r = risk_of(lines[i]);
                    flagged += r.flag ? 1 : 0;
                    if (i < 8) d.preview.push_back({lines[i], r.flag, r.note});
                }
                d.preview_more = std::max(static_cast<int>(lines.size()) - 8, 0);
                const std::string proc = fg_process(m, m.time);
                d.tag = flagged ? "CAREFUL" : "PASTE";
                d.tone = rgb_of(flagged ? p.ansi[1] : p.ansi[3]);
                d.title = "Paste " + std::to_string(lines.size()) + (lines.size() == 1 ? " line" : " lines") +
                          (proc.empty() ? std::string{"?"} : " into " + proc + "?");
                d.body = flagged ? std::to_string(flagged) + (flagged == 1 ? " line looks" : " lines look") + " dangerous — read before it runs."
                       : m.term->bracketed() ? proc + " will hold it for editing; nothing runs until you press enter."
                                             : "This program did not ask for bracketed paste: each line runs as it lands.";
                d.meta = std::to_string(lines.size()) + "L \u00B7 " + std::to_string(m.pending_paste.size()) + " bytes";
                d.actions = {{"cancel", "esc", T::normal}, {"one line", "\u21E7\u23CE", T::normal},
                             {"paste", "\u23CE", flagged ? T::danger : T::primary}};
                d.remember = m.cfg.paste_confirm == PasteConfirm::multiline ? "only ask for risky pastes" : "";
            } else {
                const bool quit = m.ask == Model::Ask::quit;
                std::vector<std::size_t> which;
                if (quit) { for (std::size_t i = 0; i < m.tab_count(); ++i) which.push_back(i); }
                else which.push_back(m.ask_tab);
                int busy = 0;
                for (std::size_t i : which) {
                    std::string el;
                    const std::string what = busy_of(m.at(i), m.time, &el);
                    if (what.empty() && quit) continue;
                    busy += what.empty() ? 0 : 1;
                    d.preview.push_back({"tab " + std::to_string(i + 1) + "  " + (what.empty() ? std::string{"(idle)"} : "\u276F " + what),
                                         false, el});
                }
                d.tag = quit ? "QUIT" : "CLOSE";
                d.tone = rgb_of(p.ansi[1]);
                d.title = quit ? "Quit hand?" : "Close tab " + std::to_string(m.ask_tab + 1) + "?";
                d.body = busy == 0 ? (quit ? "Every tab closes with it." : "Nothing is running in it.")
                       : busy == 1 ? "Something is still running — closing ends it."
                                   : std::to_string(busy) + " tabs still have something running — closing ends them.";
                d.meta = quit ? std::to_string(m.tab_count()) + (m.tab_count() == 1 ? " tab" : " tabs") : "";
                d.actions = {{"cancel", "esc", T::normal}, {quit ? "quit" : "close tab", "\u23CE", T::danger}};
                d.remember = "don't ask again";
            }
            d.width = std::min(600.0f, m.size.x - 40.0f);
            m.ask_last = std::move(d);
        }
        if (m.ask_t > 0.0f) {
            TermDialogSpec d = m.ask_last;
            d.appear = m.ask_t;
            c.overlay(Overlay{.content = term_dialog(c, "ask", d), .id = part_id("ask", part_menu),
                              .placement = Placement::centered, .dismiss = Dismiss::never, .layer = 40,
                              .scrim = 0.45f * m.ask_t, .passthrough = m.ask == Model::Ask::none});
        }

    }
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
    if (!minimap_on(m)) return frame(c, m, (z(std::move(layers)) | size(m.size.x, m.size.y)).build());

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
    return frame(c, m, (z(std::move(layers)) | size(m.size.x, m.size.y)).build());
}

}  // namespace hand
