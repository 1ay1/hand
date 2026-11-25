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
    // The grid sits CENTRED in its space, the same gap on both sides. Left
    // had padding + the mark gutter, right had padding alone, the minimap
    // drawn over it, plus whatever part of a cell did not fit -- so a TUI's
    // background ran almost into the right edge while the left kept ~18 px.
    // Now both sides reserve padding + gutter, the minimap's resting strip is
    // its own (the configured minimap, not whether it shows: entering a
    // full-screen program must not resize it), and the leftover fraction of a
    // cell is split evenly.
    const float strip = m.cfg.minimap ? detail::kMinimapRest : 0.0f;
    const float side_gap = pad + gutter;
    const float avail = std::max(m.size.x - strip - 2.0f * side_gap, cw);
    const int cols = std::max(2, static_cast<int>(avail / cw));
    m.metrics.origin_x = side_gap + std::round((avail - static_cast<float>(cols) * cw) * 0.5f);
    m.metrics.origin_y = pad;
    const int rows = std::max(1, static_cast<int>((m.size.y - 2.0f * pad) / ch));
    m.metrics.fit_cols = cols;
    m.metrics.fit_rows = rows;
    // A pane edge being dragged: the layout AND the active grid follow the
    // pointer every frame, at any scrollback size -- toe keeps history as
    // logical lines (LineLog), so a re-wrap costs O(screen), not O(history):
    // ~0.1 ms with 10k lines (it was ~14 ms, when big histories had to wait
    // for the release). Background tabs fit when the drag ends.
    if (m.pane.dragging) {
        Session& se = m.tab();
        if (se.term && (cols != se.term->cols() || rows != se.term->rows())) {
            se.term->resize(cols, rows);
            se.grid_key = 0;
        }
        return;
    }
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
    if (c.ui_font_size != before.ui_font_size) m.ui_font_size = c.ui_font_size;
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
    // goes through this OS's command runner (sh -c; cmd /c on Windows), the
    // way every terminal's -e behaves. Which runner is the OS seam's call,
    // made where the pty starts (fx::spawn): update only says "a command line".
    const bool cmdline = m.cfg.shell_args.empty() && !m.cfg.shell_cli.empty() &&
                         m.cfg.shell_cli.find_first_of(" \t|;&<>$'\"") != std::string::npos;
    Launch l{.spec = {.cols = m.metrics.fit_cols, .rows = m.metrics.fit_rows,
                      .shell = cmdline ? std::string{} : m.cfg.shell_cli.empty() ? m.cfg.shell : m.cfg.shell_cli,
                      .args = m.cfg.shell_cli.empty() ? std::vector<std::string>{} : m.cfg.shell_args,
                      .cwd = cwd},
             .integrate = m.cfg.shell_integration && !cmdline && m.cfg.shell_args.empty()};
    if (cmdline) l.command_line = m.cfg.shell_cli;
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
    // the faces main preloaded into their roles (config.hpp): the choices
    // font.family and font.ui offer on this machine
    const auto found = [&](std::vector<std::string>& into, std::size_t n, auto role) {
        into.clear();
        if (!m.fonts) return;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string& name = m.fonts->family_name(role(i));
            if (name.empty() || std::ranges::find(into, name) != into.end()) break;
            into.push_back(name);
        }
    };
    found(m.cfg.fonts, kMonoRoles, mono_role);
    found(m.cfg.ui_fonts, kUiRoles, ui_role);
    if (std::string p = resolve_font_family(m.cfg); !p.empty()) {   // hand.vibe named a face this machine lacks
        m.toast.card = Toast{.title = "hand.vibe", .body = std::move(p), .kind = Toast::Kind::error};
        ++m.toast.serial;
    }
    m.palette = palette_of(m.cfg);
    m.font_size = m.cfg.font_size;
    m.ui_font_size = m.cfg.ui_font_size;
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
/// The echo of a keystroke: the cursor stepped a cell or two along its row,
/// just after you typed. It is THERE at once -- no glide, no tail. A glide
/// between keys keeps a frame drawing every refresh, and a compositor busy
/// with that frame shows the echo a whole refresh later (measured on
/// Hyprland: typing p50 15 ms gliding, 3.3 ms not). Bigger jumps -- an
/// editor's motions, a new prompt -- still travel.
void settle_typed_cursor(const Model& m, Session& se) {
    if (m.cfg.cursor_anim == CursorAnim::none || !se.cur_known || m.time - se.fx.typed_at > 0.35) return;
    const auto cur = se.term->cursor();
    const float tx = static_cast<float>(cur.col), ty = static_cast<float>(cur.row);
    if (ty != se.cur_y || std::fabs(tx - se.cur_x) > 2.5f) return;
    se.cur_x = tx;
    se.cur_y = ty;
    se.fx.vx = se.fx.vy = 0.0f;
    se.fx.tx = tx;
    se.fx.ty = ty;
}

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
        ++m.toast.serial;
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
    m.blinks_idle = 0;   // output wakes a resting cursor blink
    const int off0 = m.tab().term->scroll_offset(), hist0 = m.tab().term->history();
    const Effects fx = m.tab().term->feed(o.bytes, m.wall_ms);
    settle_typed_cursor(m, m.tab());
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
        ++m.toast.serial;
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
            // a link that was up a while dropped: start the schedule over; one
            // that died within seconds of a try: that try failed, back off
            const bool was_up = m.time - se.link.connected_at > 10.0;
            se.link.attempts = was_up ? 0 : se.link.attempts + 1;
            se.link.retry_at = m.time + Remote::backoff(se.link.attempts);
            if (was_up) se.link.dropped_at = m.time;   // the card's "how long" counts from the first drop, across failed tries
            se.link.dropped = true;
            se.pty.reset();
            se.grid_key = 0;
            if (i != m.active() && was_up) {
                se.unread = true;
                m.toast.card = Toast{.title = "\u21C4 " + se.remote->alias + " disconnected", .body = "tab " + std::to_string(i + 1) + " \u00B7 reconnecting by itself",
                                .kind = Toast::Kind::warning};
                ++m.toast.serial;
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
    const auto front = m.tab().sid;
    const auto toast_before = m.toast.serial;
    auto cmd = step(m, std::move(msg));
    if (m.tab().sid != front) m.blinks_idle = 0;   // another tab in front: its cursor blinks again
    // A new toast (any handler bumped the serial) leaves on a timer of its own:
    // one message in 3 s, instead of a frame subscription running until then.
    if (m.toast.serial != toast_before && m.toast.card)
        cmd = Cmd::batch(std::move(cmd), Cmd::after(std::chrono::milliseconds(ToastModel::kShowMs), Msg{ToastExpired{m.toast.serial}}));
    return cmd;
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
            // the pty hears a size once it has held Env::resize_settle_ms (a
            // drag's sizes are the grid's alone; the pty repaints once, at the end)
            if (se.term->cols() != se.size_cols || se.term->rows() != se.size_rows) {
                se.size_cols = se.term->cols();
                se.size_rows = se.term->rows();
                se.size_since = m.now;
            }
            const auto settle = std::chrono::milliseconds(m.env.resize_settle_ms);
            const auto held = m.now - se.size_since;
            if (held >= settle) {
                // the grid was reshaped frame by frame while the pty heard
                // nothing: reach this size from the model the pty last saw
                if (m.env.resize_settle_ms > 0) {
                    const int c = se.term->cols(), r = se.term->rows();
                    se.term->resize_for_pty(c, r);
                    se.grid_key = 0;
                }
                se.term->drop_pty_sized();
                se.pty_cols = se.term->cols();
                se.pty_rows = se.term->rows();
                cmds.push_back(Cmd::run(fx::pty_resize, se.pty.share(), se.pty_cols, se.pty_rows,
                                        static_cast<int>(m.size.x), static_cast<int>(m.size.y)));
            } else if (!m.resize_due) {
                m.resize_due = true;
                cmds.push_back(Cmd::after(std::chrono::duration_cast<std::chrono::milliseconds>(settle - held) + std::chrono::milliseconds(1),
                                          Msg{ResizeSettle{}}));
            }
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

/// The drain re-checks every tab's size (and reschedules one still moving).
App::Cmd App::update(Model& m, ResizeSettle) {
    m.resize_due = false;
    return {};
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
    std::vector<Cmd> cmds;
    for (auto& t : p.tabs) {
        const auto i = m.tabs.find(t.sid);
        if (!i) continue;
        Session& se = m.at(*i);
        se.proc = std::move(t.proc);
        if (se.link.launched) continue;   // a launched tab knows its host
        // `ssh host` typed in a tab: its files are about to be asked for (the
        // pane follows, ⌘P lists) -- bring up the channel now, not on that click
        const bool reached = t.remote && (!se.remote || se.remote->alias != t.remote->alias);
        se.remote = std::move(t.remote);
        if (reached && !m.cfg.demo) cmds.push_back(Cmd::task(fx::warm_remote, *se.remote, m.ssh.book));
    }
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, Second) {
    ++m.clock;
    m.tab().grid_key = 0;
    // one activity sample a second per tab: bytes in, as a 0..1 rate
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        Session& se = m.at(i);
        se.activity.push_back(std::clamp(std::log1p(static_cast<float>(se.bytes - se.bytes_sampled)) / 9.0f, 0.0f, 1.0f));
        if (se.activity.size() > 20) se.activity.erase(se.activity.begin());
        se.bytes_sampled = se.bytes;
    }
    return pane_sync(m);   // catches an `ssh` started inside the tab
}

bool detail::blink_resting(const Model& m) {
    // A modal over the terminal has the keys (settings, the host manager, a
    // question, ⌘P, ⌘⇧F): the cursor under its scrim is not where you type, so
    // it holds solid. Its blink was 14 frames a cycle drawing a cursor the
    // scrim hid -- every second the manager stayed open.
    if (modal_open(m)) return true;
    const int after = std::max(4, static_cast<int>(10000.0f / std::max(m.cfg.blink_ms, 50.0f)));   // ~10 s of blinks
    return m.cfg.cursor_blink && m.blinks_idle >= after;
}

App::Cmd App::update(Model& m, Blink) {
    // Quiet for 20 blinks (10 s at the default rate): no keys, no output, the
    // cursor where it was -- stop blinking, solid, and the window draws
    // nothing more until something happens (then it blinks again).
    const Session& se = m.tab();
    const auto cur = se.term ? se.term->cursor() : Term::Cursor{};
    const std::uint64_t seen = (se.term ? se.term->epoch() : 0) * 0x9E3779B97F4A7C15ull ^ m.keys_typed * 0xBF58476D1CE4E5B9ull ^
                               (static_cast<std::uint64_t>(cur.row) << 32 | static_cast<std::uint32_t>(cur.col)) ^
                               static_cast<std::uint64_t>(m.active());
    m.blinks_idle = seen == m.blink_seen ? m.blinks_idle + 1 : 0;
    m.blink_seen = seen;
    if (blink_resting(m)) {
        m.cursor_on = true;
        m.tab().cur_a = 1.0f;
        return {};
    }
    m.cursor_on = !m.cursor_on || !m.cfg.cursor_blink;
    if (m.cfg.blink_style == BlinkStyle::hard) m.tab().cur_a = m.cursor_on ? 1.0f : 0.0f;
    return {};
}

App::Cmd App::update(Model& m, ToastExpired e) {
    if (e.serial == m.toast.serial) m.toast.card.reset();   // a newer toast keeps its own time
    return {};
}

App::Cmd App::update(Model& m, HudExpired e) {
    if (e.serial == m.hud_serial) m.hud_up = false;   // a later resize keeps it up
    return {};
}

App::Cmd App::update(Model& m, Tick t) {
    Session& se = m.tab();
    m.time += t.dt;
    const float mdt = static_cast<float>(t.dt) * m.cfg.motion_speed;   // the motion clock
    if (se.file && !se.file->loading) (void)text_editor_ops::step(se.file->st, mdt, m.cfg.motion);
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
        if (m.cfg.blink_style == BlinkStyle::fade)   // under a modal's scrim nothing shows the fade: it lands at once
            toward(se.cur_a, cursor_lit(m) ? 1.0f : 0.0f, modal_open(m) ? 1000.0f : 16.0f * m.cfg.motion_speed);
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
        // done once what is left is under a quarter of a logical pixel (half a
        // device pixel at 2x): 0.004 rows was 0.08 px, and the glide spent its
        // last five frames moving nothing on screen
        if (std::fabs(se.scroll_goal - se.scroll_d) < 0.25f / std::max(m.metrics.cell_h, 1.0f)) se.scroll_d = se.scroll_goal;
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
        motion::move_tail(se.fx, smear ? tx : se.cur_x, smear ? ty : se.cur_y, m.cfg.cursor_trail, mdt, cw, chh);
    }
    if (!motion::cursor_settled(se.fx, m.cfg.cursor_anim, se.cur_x, se.cur_y, tx, ty) && se.term->scroll_offset() == 0)
        motion::travel(se.fx, m.cfg.cursor_fx, m.metrics.origin_x + (se.cur_x + 0.5f) * cw, m.metrics.origin_y + (se.cur_y + 0.5f) * chh,
                       chh, m.palette);
    return {};
}

}  // namespace hand
