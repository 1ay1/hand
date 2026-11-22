// hand::App — the shells (spawn, output, exit) and the clocks.

#include "detail.hpp"
#include "fx/fx.hpp"

#include <hand/core/shell.hpp>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

float App::Model::strip_h() const noexcept {
    // the strip shows from the second tab on (or always, by setting), easing in
    return std::round(std::clamp(cfg.tab_height, 22.0f, 60.0f) * strip.t);
}

void App::fit(Model& m) {
    // the file pane takes the right side; the terminal keeps at least ~320 px
    if (m.pane.open) m.pane.width = std::clamp(m.pane.width, 220.0f, std::max(220.0f, std::min(640.0f, m.win.x - 320.0f)));
    const float side = m.pane.open ? m.pane.width : 0.0f;
    m.size = Vec2{std::max(m.win.x - side, 1.0f), std::max(m.win.y - m.strip_h(), 1.0f)};
    const float cw = detail::cell_width(m, m.font_size);
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
        se.term->resize(cols, rows);   // the pty is told by the drain after this step
        se.grid_key = 0;
    }
}

void App::apply(Model& m, const Config& before) {
    Session& se = m.tab();
    const Config& c = m.cfg;
    if (c.font_size != before.font_size) m.font_size = c.font_size;
    if (m.pane.open) {   // the file pane's settings take effect now
        if (c.files_width != before.files_width) m.pane.width = c.files_width;
        if (c.files_sort != before.files_sort) m.pane.ui.sort = static_cast<mayag::dsl::FileSort>(std::clamp(c.files_sort, 0, 3));
        if (c.files_hidden != before.files_hidden) m.pane.ui.show_hidden = c.files_hidden;
        if (c.files_git != before.files_git || c.files_preview_lines != before.files_preview_lines) {
            m.pane.preview_path.clear();
            m.pane.where = "\x01";   // re-list (git marks) on the next sync
        }
    }
    if (c.scheme != before.scheme) { m.palette = palette_of(c); se.term->set_palette(m.palette); }
    if (c.scrollback != before.scrollback) se.term->set_scrollback(static_cast<int>(c.scrollback));
    if (!c.cursor_blink) m.cursor_on = true;
    fit(m);   // font family / size / line height / padding
    se.grid_key = 0;
}

void App::scroll_by(Model& m, float rows) {
    Session& se = m.tab();
    if (!se.term) return;
    const float top = static_cast<float>(se.term->history());
    se.scroll_goal = std::clamp(se.scroll_goal + rows, 0.0f, top);
    if (!m.cfg.smooth_scroll) {
        se.scroll_goal = std::round(se.scroll_goal);
        se.scroll_d = se.scroll_goal;
        se.term->set_scroll_offset(se.scroll_applied = static_cast<int>(se.scroll_goal));
    }
    if (se.scroll_goal == 0.0f) se.new_below = 0;
}

void App::settle_scroll(Model& m) {
    Session& se = m.tab();
    if (!se.term) return;
    const int now = se.term->scroll_offset();
    if (now == se.scroll_applied) return;
    se.scroll_goal = static_cast<float>(now);           // fly there...
    if (!m.cfg.smooth_scroll || se.term->alt_screen()) {
        se.scroll_d = se.scroll_goal;
        se.scroll_applied = now;
        return;
    }
    se.scroll_d = std::clamp(se.scroll_d, 0.0f, static_cast<float>(se.term->history()));
    se.scroll_applied = static_cast<int>(std::floor(se.scroll_d));   // ...from where we are
    se.term->set_scroll_offset(se.scroll_applied);
    if (now == 0) se.new_below = 0;
}

App::Session App::spawn_session(Model& m, const std::string& cwd) {
    Session se;
    se.sid = m.session_ids.next();
    se.opened_at = m.time;
    se.term = std::make_shared<Term>(std::max(m.metrics.fit_cols, 2), std::max(m.metrics.fit_rows, 1), m.palette,
                                     static_cast<int>(m.cfg.scrollback), m.env.entropy ^ se.sid.get());
    if (m.cfg.demo) return se;
    // `-e CMD`: a bare program runs as is; a command line (arguments, pipes)
    // goes through sh -c, the way every terminal's -e behaves
    const bool cmdline = !m.cfg.shell_cli.empty() && m.cfg.shell_cli.find_first_of(" \t|;&<>$'\"") != std::string::npos;
    Launch l{.spec = {.cols = m.metrics.fit_cols, .rows = m.metrics.fit_rows,
                      .shell = cmdline ? std::string{"/bin/sh"} : m.cfg.shell_cli.empty() ? m.cfg.shell : m.cfg.shell_cli,
                      .cwd = cwd},
             .integrate = m.cfg.shell_integration && !cmdline};
    if (cmdline) l.spec.args = {"-c", m.cfg.shell_cli};
    if (!cwd.empty()) l.spec.env.emplace_back("PWD", cwd);
    se.launch = std::move(l);   // the drain after this step starts it (fx::spawn)
    return se;
}

App::Session App::spawn_ssh(Model& m, const Host& h) {
    Session se;
    se.sid = m.session_ids.next();
    se.opened_at = m.time;
    se.term = std::make_shared<Term>(std::max(m.metrics.fit_cols, 2), std::max(m.metrics.fit_rows, 1), m.palette,
                                     static_cast<int>(m.cfg.scrollback), m.env.entropy ^ se.sid.get());
    se.link.launched = true;
    se.remote = h;
    // what is about to happen, dimmed, so a slow handshake is not a blank screen
    (void)se.term->feed("\x1b[2m\u21C4 " + ssh_command(h) + "\x1b[0m\r\n");
    start_ssh(m, se, h);
    return se;
}

App::Cmd App::init(Model& m, const Flags& f) {
    m.cfg = f.config;
    m.env = f.env;
    m.fonts = f.fonts;
    m.wall_ms = m.env.wall_epoch_ms;
    // the monospace families mayag attached (custom_family(0), (1), ...):
    // the choices font.family offers on this machine
    m.cfg.fonts.clear();
    if (m.fonts)
        for (std::size_t i = 0; i < mayag::custom_family_count; ++i) {
            const std::string& name = m.fonts->family_name(mayag::custom_family(i));
            if (name.empty() || std::ranges::find(m.cfg.fonts, name) != m.cfg.fonts.end()) break;
            m.cfg.fonts.push_back(name);
        }
    if (std::string p = resolve_font_family(m.cfg); !p.empty()) {   // hand.vibe named a face this machine lacks
        m.toast.card = Toast{.title = "hand.vibe", .body = std::move(p), .kind = Toast::Kind::error};
        m.toast.at = m.time;
    }
    m.palette = palette_of(m.cfg);
    m.font_size = m.cfg.font_size;
    m.strip.t = m.cfg.strip_always() ? 1.0f : 0.0f;
    fit(m);
    m.ssh.sources = detail::host_sources(m);
    m.tab() = spawn_session(m, "");   // Tabs starts with one empty session: the shell goes in it
    std::vector<Cmd> cmds;
    cmds.push_back(SetTitle{"hand"});
    // the host book (prod detection on tabs needs it before the picker ever opens)
    if (!m.cfg.demo) cmds.push_back(Cmd::run(fx::load_hosts, m.ssh.sources, false));
    if (m.cfg.files_open) {
        cmds.push_back(update(m, TogglePane{}));
        m.pane.ui.focused = false;   // the shell has the keyboard at start
    }
    cmds.push_back(drain(m));   // the first shell starts with the first step
    return Cmd::batch(std::move(cmds));
}

namespace {
/// Output for a tab that is not in front: fed and marked unread, and a long
/// command finishing there is announced, naming the tab.
void feed_background(Model& m, std::size_t i, const std::string& bytes) {
    Session& se = m.at(i);
    const Effects fx = se.term->feed(bytes, m.wall_ms);
    se.input += fx.reply;   // query answers go back to the program
    if (fx.title_set) se.title = fx.title.empty() ? "hand" : fx.title;
    if (fx.bell) se.bell = true;
    se.unread = true;
    se.bytes += bytes.size();
    se.grid_key = 0;
    // a long command finished in a background tab: say so, naming the tab
    for (const auto& c : se.term->commands()) {
        if (!c.finished || !c.ran || c.id <= se.notified) continue;
        se.notified = c.id;
        if (m.cfg.notify_after <= 0.0f || static_cast<double>(c.duration_ms) / 1000.0 < m.cfg.notify_after) continue;
        m.toast.card = Toast{.title = (c.exit_code == 0 ? "\u2713 tab " : "\u2717 tab ") + std::to_string(i + 1) + " " +
                                 (c.exit_code == 0 ? "done" : "failed (" + std::to_string(c.exit_code) + ")"),
                        .body = se.term->command_text(c),
                        .kind = c.exit_code == 0 ? Toast::Kind::success : Toast::Kind::error};
        m.toast.at = m.time;
    }
}
}  // namespace

App::Cmd App::update(Model& m, Output o) {
    // fed (whatever happens below): the reader may send the next chunk
    struct GiveBack {
        std::optional<mayag::CoOwned<OutputCredit>>& c;
        ~GiveBack() { if (c) (*c)->give_back(); }
    } give_back{o.credit};
    if (o.sid != kNoSession && o.sid != m.tab().sid) {   // a background tab (or a shell already gone)
        if (const auto i = m.tabs.find(o.sid)) feed_background(m, *i, o.bytes);
        return probe_soon(m);
    }
    m.tab().bytes += o.bytes.size();
    const int off0 = m.tab().term->scroll_offset(), hist0 = m.tab().term->history();
    const Effects fx = m.tab().term->feed(o.bytes, m.wall_ms);
    {   // toe keeps a scrolled-back view still by bumping its offset: follow it
        const int off1 = m.tab().term->scroll_offset();
        if (off0 > 0 && off1 != off0) {
            const float d = static_cast<float>(off1 - off0);
            m.tab().scroll_d += d; m.tab().scroll_goal += d;
            m.tab().scroll_applied = off1;
        } else if (off1 == 0 && off0 == 0) {
            const int grew = std::max(m.tab().term->history() - hist0, 0);
            if (m.tab().scroll_goal > 0.0f && !m.tab().term->alt_screen()) {
                // A glide up that has not reached its first whole row yet: toe is
                // still following the bottom, so anchor to the CONTENT the user is
                // moving toward -- otherwise streaming output resets every frame and
                // a trackpad can never leave the live view.
                const float top = static_cast<float>(m.tab().term->history());
                m.tab().scroll_d = std::min(m.tab().scroll_d + static_cast<float>(grew), top);
                m.tab().scroll_goal = std::min(m.tab().scroll_goal + static_cast<float>(grew), top);
                m.tab().scroll_applied = static_cast<int>(std::floor(m.tab().scroll_d));
                m.tab().term->set_scroll_offset(m.tab().scroll_applied);
                m.tab().new_below += grew;
            } else {
                m.tab().scroll_d = m.tab().scroll_goal = 0.0f;
                m.tab().scroll_applied = 0;
                // Enter at a prompt: the screen glides up a line instead of jumping.
                // Only a lone line from rest -- a stream of output snaps, so it never lags.
                if (m.cfg.motion && m.cfg.smooth_scroll && grew == 1 && m.tab().out_shift < 0.05f && !m.tab().term->alt_screen())
                    m.tab().out_shift = 1.0f;
                else if (grew > 0)
                    m.tab().out_shift = 0.0f;
            }
        }
        if (off1 > 0) m.tab().new_below += std::max(m.tab().term->history() - hist0, 0);
    }
    m.tab().input += fx.reply;   // query answers go back to the program
    if (fx.bell && (m.cfg.visual_bell || m.cfg.bell_shake)) m.tab().bell_at = m.time;
    m.cursor_on = true;   // output resets the blink, like foot
    m.tab().cur_a = 1.0f;
    m.tab().active_at = m.time;
    std::vector<Cmd> cmds;
    if (fx.title_set) { m.tab().title = fx.title.empty() ? "hand" : fx.title; cmds.push_back(SetTitle{m.tab().title}); }
    if (fx.bell) m.tab().bell = false;   // seen: it is the tab you are looking at
    if (fx.clipboard_set) cmds.push_back(WriteClipboard{fx.clipboard});
    // shell integration: a long command finished while you were away -> tell you
    for (const auto& c : m.tab().term->commands()) {
        if (!c.finished || !c.ran || c.id <= m.tab().notified) continue;
        m.tab().notified = c.id;
        const double secs = static_cast<double>(c.duration_ms) / 1000.0;
        if (m.cfg.notify_after <= 0.0f || secs < m.cfg.notify_after) continue;
        std::string cmd = m.tab().term->command_text(c);
        if (cmd.size() > 80) cmd = cmd.substr(0, 77) + "...";
        char took[32];
        std::snprintf(took, sizeof took, secs < 60 ? "%.1fs" : "%.0fm%02.0fs", secs < 60 ? secs : std::floor(secs / 60.0),
                      std::fmod(secs, 60.0));
        const std::string title = c.exit_code == 0 ? "\u2713 done in " + std::string{took}
                                                   : "\u2717 failed (" + std::to_string(c.exit_code) + ") after " + took;
        if (!m.focused && !m.cfg.demo)
            cmds.push_back(Cmd::run(fx::open_detached, std::vector<std::string>{"notify-send", "-a", "hand", "-u",
                                                                               c.exit_code == 0 ? "normal" : "critical", title,
                                                                               cmd.empty() ? "command finished" : cmd}));
        m.toast.card = Toast{.title = title, .body = cmd, .kind = c.exit_code == 0 ? Toast::Kind::success : Toast::Kind::error};
        m.toast.at = m.time;
    }
    if (m.pane.open) cmds.push_back(pane_sync(m));   // a cd lands in the output (OSC 7)
    cmds.push_back(probe_soon(m));                   // what runs there may have changed
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, Exited e) {
    // whose program ended (kNoSession: the front tab's, from a scripted run)
    const auto found = e.sid == kNoSession ? std::optional{m.active()} : m.tabs.find(e.sid);
    if (found) {
        const std::size_t i = *found;
        Session& se = m.at(i);
        // ssh's own failure (255: refused, timed out, the link died) on a tab hand
        // connected: keep the tab and its scrollback, offer to reconnect. A
        // logout (the remote shell's status) closes it like any shell.
        if (se.link.launched && e.code == 255 && m.cfg.ssh_reconnect && se.remote) {
            se.link.dropped = true;
            se.link.dropped_at = m.time;
            se.pty.reset();
            se.grid_key = 0;
            if (i != m.active()) {
                se.unread = true;
                m.toast.card = Toast{.title = "\u21C4 " + se.remote->alias + " disconnected", .body = "tab " + std::to_string(i + 1) + " \u00B7 enter there to reconnect",
                                .kind = Toast::Kind::warning};
                m.toast.at = m.time;
            }
            return {};
        }
        return update(m, CloseTab{TabIndex{i}, Confirm::given});   // the shell left: nothing to ask
    }
    return {};
}

// ── the seam ──────────────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, Msg msg, Time now) {
    // the step's time: jaal read the clock once, every handler agrees on it
    if (m.began == Time{}) m.began = now;
    m.now = now;
    m.wall_ms = m.env.wall_epoch_ms + std::chrono::duration_cast<std::chrono::milliseconds>(now - m.began).count();
    return step(m, std::move(msg));
}

App::Cmd App::step(Model& m, Msg msg) {
    Cmd c = std::visit([&](auto&& domain) -> Cmd {
        if constexpr (requires { update(m, std::move(domain)); }) {
            return update(m, std::move(domain));   // Edit: a leaf at the root
        } else {
            return std::visit([&](auto&& leaf) -> Cmd { return update(m, std::move(leaf)); }, std::move(domain));
        }
    }, static_cast<msg::AnyBase&&>(msg));
    return Cmd::batch(std::move(c), drain(m));
}

App::Cmd App::drain(Model& m) {
    std::vector<Cmd> cmds;
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        Session& se = m.at(i);
        if (auto l = std::exchange(se.launch, std::nullopt)) {
            // a new program for this session: whatever it had is gone with its sid
            se.pty.reset();
            se.starting = true;
            se.pty_cols = l->spec.cols;
            se.pty_rows = l->spec.rows;
            cmds.push_back(Cmd::run(fx::spawn, se.sid, std::move(*l)));
        }
        if (!se.pty) {   // nobody to tell yet: keys typed while it starts wait for it; with no program, they go nowhere
            if (!se.starting) se.input.clear();
            continue;
        }
        if (se.term && (se.term->cols() != se.pty_cols || se.term->rows() != se.pty_rows)) {
            se.pty_cols = se.term->cols();
            se.pty_rows = se.term->rows();
            cmds.push_back(Cmd::run(fx::pty_resize, se.pty.share(), se.pty_cols, se.pty_rows,
                                    static_cast<int>(m.size.x), static_cast<int>(m.size.y)));
        }
        if (!se.input.empty()) cmds.push_back(Cmd::run(fx::pty_write, se.pty.share(), std::exchange(se.input, {})));
    }
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, Spawned s) {
    const auto i = m.tabs.find(s.sid);
    if (!i) return {};   // closed while it started (its pty dies with this message)
    Session& se = m.at(*i);
    se.starting = false;
    if (!s.pty) {
        (void)se.term->feed(se.link.launched ? "\x1b[31mhand: could not start ssh\x1b[0m\r\n"
                                             : "\x1b[31mhand: could not start a shell\x1b[0m\r\n", m.wall_ms);
        se.grid_key = 0;
        return {};
    }
    se.pty = PtyRef{std::move(*s.pty)};
    if (!se.link.launched) se.term->set_prompt_protect(!s.reflow_aware);
    return probe_soon(m);   // the drain after this step sends what was typed meanwhile, and any new size
}

App::Cmd App::probe_soon(Model& m) {
    // throttled, not debounced: a firehose (`yes`) still gets a probe every 400 ms
    if (m.probe_due || m.cfg.demo) return {};
    m.probe_due = true;
    return Cmd::after(std::chrono::milliseconds(400), Msg{Probe{}});
}

App::Cmd App::update(Model& m, Probe) {
    m.probe_due = false;
    if (m.cfg.demo) return {};
    std::vector<fx::ProbeTab> tabs;
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Session& se = m.at(i);
        fx::ProbeTab t{.sid = se.sid, .was = se.proc, .watch_ssh = !se.link.launched, .had_remote = se.remote.has_value()};
        if (se.pty) t.pty = se.pty.share();
        tabs.push_back(std::move(t));
    }
    return Cmd::run(fx::probe, std::move(tabs));
}

App::Cmd App::update(Model& m, Probed p) {
    for (auto& t : p.tabs) {
        const auto i = m.tabs.find(t.sid);
        if (!i) continue;
        Session& se = m.at(*i);
        se.proc = std::move(t.proc);
        if (!se.link.launched) se.remote = std::move(t.remote);   // a launched tab knows its host
    }
    return {};
}

App::Cmd App::update(Model& m, Second) {
    ++m.clock;
    m.tab().grid_key = 0;
    // one activity sample a second per tab: bytes in, as a 0..1 rate
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        Session& se = m.at(i);
        se.activity.push_back(std::clamp(std::log1p(static_cast<float>(se.bytes)) / 9.0f, 0.0f, 1.0f));
        if (se.activity.size() > 20) se.activity.erase(se.activity.begin());
        se.bytes = 0;
    }
    return pane_sync(m);   // catches an `ssh` started inside the tab
}

App::Cmd App::update(Model& m, Blink) {
    m.cursor_on = !m.cursor_on || !m.cfg.cursor_blink;
    if (m.cfg.blink_style == BlinkStyle::hard) m.tab().cur_a = m.cursor_on ? 1.0f : 0.0f;
    return {};
}

App::Cmd App::update(Model& m, Tick t) {
    Session& se = m.tab();
    m.time += t.dt;
    const float mdt = static_cast<float>(t.dt) * m.cfg.motion_speed;   // the motion clock
    if (se.selecting && se.sel_edge != 0.0f) {   // drag-select past an edge: history scrolls under the pointer
        scroll_by(m, se.sel_edge * static_cast<float>(t.dt));
        const int col = cell_of(se.sel_pos.x + m.metrics.cell_w * 0.5f, m.metrics.origin_x, m.metrics.cell_w, se.term->cols() + 1);
        se.term->select_extend(col, view_row(m, se.sel_pos.y));
    }
    {   // the tab strip eases in/out (the grid refits as it does); a switched tab slides in
        const float goal = (m.cfg.strip_always() || m.tab_count() > 1) ? 1.0f : 0.0f;
        if (m.strip.t != goal) {
            m.strip.t += (goal - m.strip.t) * (1.0f - std::exp(-mdt * 18.0f));
            if (std::fabs(goal - m.strip.t) < 0.01f) m.strip.t = goal;
            fit(m);
        }
        if (m.strip.switch_t < 1.0f) m.strip.switch_t = std::min(1.0f, m.strip.switch_t + mdt * 6.0f);
        if (m.strip.show_numbers && m.time - m.strip.numbers_at > 1.2) m.strip.show_numbers = false;   // no key-up events: a timed hint
    }
    {   // the blink fades; the settings panel and new lines glide
        const auto toward = [&](float& v, float goal, float rate) {
            v += (goal - v) * (1.0f - std::exp(-static_cast<float>(t.dt) * rate));
            if (std::fabs(v - goal) < 0.01f) v = goal;
        };
        if (m.cfg.blink_style == BlinkStyle::fade)
            toward(se.cur_a, m.cursor_on || !m.focused || !m.cfg.cursor_blink ? 1.0f : 0.0f, 16.0f * m.cfg.motion_speed);
        toward(m.settings.t, m.settings.open ? 1.0f : 0.0f, m.cfg.motion ? 22.0f : 1000.0f);
        toward(m.ssh.manager.t, m.ssh.manager.open ? 1.0f : 0.0f, m.cfg.motion ? 22.0f : 1000.0f);
        toward(se.out_shift, 0.0f, 22.0f);
        toward(m.dialog.t, m.dialog.ask.has_value() ? 1.0f : 0.0f, m.cfg.motion ? 20.0f : 1000.0f);
    }
    {   // the peek card and the live tail fade / slide in and out
        const auto ease = [&](float& v, bool on, float rate) {
            v += ((on ? 1.0f : 0.0f) - v) * (1.0f - std::exp(-static_cast<float>(t.dt) * rate));
            if (std::fabs(v - (on ? 1.0f : 0.0f)) < 0.01f) v = on ? 1.0f : 0.0f;
        };
        ease(m.minimap.peek_t, m.cfg.peek && m.minimap.hover_y >= 0.0f, 24.0f);
        ease(se.tail_t, tail_wanted(m), 16.0f);
        const auto st = sticky_of(m);
        if (st && st->id != se.sticky_id) { se.sticky_id = st->id; se.sticky_t = std::min(se.sticky_t, 0.35f); }   // a new command: re-slide
        ease(se.sticky_t, st.has_value(), 20.0f);
    }
    {   // the minimap slides open / shut
        const float goal = m.minimap.open ? kMinimapOpen : kMinimapRest;
        m.minimap.width += (goal - m.minimap.width) * (1.0f - std::exp(-static_cast<float>(t.dt) * 22.0f));
        if (std::fabs(goal - m.minimap.width) < 0.3f) m.minimap.width = goal;
    }
    if (se.scroll_d != se.scroll_goal) {   // critically damped glide, ~120 ms, frame-rate independent
        const float k = 1.0f - std::exp(-static_cast<float>(t.dt) * 18.0f);
        se.scroll_d += (se.scroll_goal - se.scroll_d) * k;
        if (std::fabs(se.scroll_goal - se.scroll_d) < 0.004f) se.scroll_d = se.scroll_goal;
        const int whole = std::clamp(static_cast<int>(std::floor(se.scroll_d)), 0, se.term->history());
        if (whole != se.scroll_applied) { se.term->set_scroll_offset(whole); se.scroll_applied = whole; }
        if (se.scroll_d == 0.0f) se.new_below = 0;
    }
    const auto cur = se.term->cursor();
    const float tx = static_cast<float>(cur.col), ty = static_cast<float>(cur.row);
    const float cw = m.metrics.cell_w, chh = m.metrics.cell_h;
    // the cursor moved because you typed: the effect plays at its new cell
    if (cur.col != se.fx.last_col || cur.row != se.fx.last_row) {
        const bool typed = m.time - se.fx.typed_at < 0.35;
        if (typed && se.fx.last_col >= 0 && m.cfg.cursor_fx != CursorFx::none && se.term->scroll_offset() == 0 && cur.visible)
            motion::emit(se.fx, m.cfg.cursor_fx, m.metrics.origin_x + (tx + 0.5f) * cw, m.metrics.origin_y + (ty + 0.5f) * chh,
                         (tx - static_cast<float>(se.fx.last_col)) * cw, (ty - static_cast<float>(se.fx.last_row)) * chh, cw, chh,
                         m.time - se.fx.enter_at < 0.35, m.palette);
        se.fx.last_col = cur.col;
        se.fx.last_row = cur.row;
    }
    motion::step(se.fx, mdt);
    if (m.cfg.cursor_anim == CursorAnim::none || !se.cur_known) {
        se.cur_x = tx; se.cur_y = ty; se.cur_known = true;
        se.fx.tx = tx; se.fx.ty = ty;
        return {};
    }
    motion::move_cursor(se.fx, m.cfg.cursor_anim, se.cur_x, se.cur_y, tx, ty, mdt);
    {   // the tail follows the head (smear: the head IS the cell)
        const bool smear = m.cfg.cursor_anim == CursorAnim::smear;
        motion::move_tail(se.fx, smear ? tx : se.cur_x, smear ? ty : se.cur_y, m.cfg.cursor_trail, mdt);
    }
    if (!motion::cursor_settled(se.fx, m.cfg.cursor_anim, se.cur_x, se.cur_y, tx, ty) && se.term->scroll_offset() == 0)
        motion::travel(se.fx, m.cfg.cursor_fx, m.metrics.origin_x + (se.cur_x + 0.5f) * cw, m.metrics.origin_y + (se.cur_y + 0.5f) * chh,
                       chh, m.palette);
    return {};
}

}  // namespace hand
