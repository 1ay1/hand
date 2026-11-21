// hand::App — the shells (spawn, output, exit) and the clocks.

#include "detail.hpp"

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
    if (m.pane.open) {   // the file pane's settings take effect now
        if (c.files_width != before.files_width) m.pane.width = c.files_width;
        if (c.files_sort != before.files_sort) m.pane.ui.sort = static_cast<mayag::dsl::FileSort>(std::clamp(c.files_sort, 0, 3));
        if (c.files_hidden != before.files_hidden) m.pane.ui.show_hidden = c.files_hidden;
        if (c.files_git != before.files_git || c.files_preview_lines != before.files_preview_lines) {
            m.pane.preview_path.clear();
            m.pane.where = "\x01";   // re-list (git marks) on the next sync
        }
    }
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
    se.sid = m.session_ids.next();
    se.opened_at = m.time;
    se.term = std::make_shared<Term>(std::max(m.metrics.fit_cols, 2), std::max(m.metrics.fit_rows, 1), m.palette,
                                     static_cast<int>(m.cfg.scrollback));
    if (m.cfg.demo) return se;
    se.pty = std::make_shared<Pty>();
    // `-e CMD`: a bare program runs as is; a command line (arguments, pipes)
    // goes through sh -c, the way every terminal's -e behaves
    const bool cmdline = !m.cfg.shell_cli.empty() && m.cfg.shell_cli.find_first_of(" \t|;&<>$'\"") != std::string::npos;
    const std::string shell = cmdline ? std::string{"/bin/sh"}
                                      : Pty::resolve_shell(m.cfg.shell_cli.empty() ? m.cfg.shell : m.cfg.shell_cli);
    ShellLaunch si = m.cfg.shell_integration && !cmdline ? shell_integration(shell) : ShellLaunch{};
    if (cmdline) si.args = {"-c", m.cfg.shell_cli};
    if (!cwd.empty()) si.env.emplace_back("PWD", cwd);
    se.term->set_prompt_protect(!si.reflow_aware);
    if (!se.pty->spawn(m.metrics.fit_cols, m.metrics.fit_rows, shell, si.env, si.args, cwd)) {
        (void)se.term->feed("\x1b[31mhand: could not start a shell\x1b[0m\r\n");
        se.pty.reset();
    }
    return se;
}

App::Session App::spawn_ssh(Model& m, const Host& h) {
    Session se;
    se.sid = m.session_ids.next();
    se.opened_at = m.time;
    se.term = std::make_shared<Term>(std::max(m.metrics.fit_cols, 2), std::max(m.metrics.fit_rows, 1), m.palette,
                                     static_cast<int>(m.cfg.scrollback));
    se.link.launched = true;
    se.remote = h;
    // what is about to happen, dimmed, so a slow handshake is not a blank screen
    (void)se.term->feed("\x1b[2m\u21C4 " + ssh_command(h) + "\x1b[0m\r\n");
    start_ssh(m, se, h);
    return se;
}

App::Cmd App::init(Model& m) {
    startup::mark("hand: init");
    m.cfg = config;
    m.palette = palette_of(m.cfg);
    m.font_size = m.cfg.font_size;
    m.strip.t = m.cfg.strip_always() ? 1.0f : 0.0f;
    fit(m);
    // the host book: prod detection on tabs needs it before the picker ever opens
    if (!m.cfg.demo) {
        m.ssh.sources = resolve(HostSources{.read_known = m.cfg.ssh_known_hosts});
        m.ssh.book = hosts_load(m.ssh.sources);
    }
    startup::mark("hand: init: host book");
    static_cast<Session&>(m) = spawn_session(m, "");
    startup::mark("hand: init: shell spawned");
    m.tabs.resize(1);
    m.active = 0;
    if (m.cfg.files_open) {
        std::vector<Cmd> cmds;
        cmds.push_back(SetTitle{"hand"});
        cmds.push_back(update(m, TogglePane{}));
        m.pane.ui.focused = false;   // the shell has the keyboard at start
        return Cmd::batch(std::move(cmds));
    }
    return SetTitle{"hand"};
}

App::Cmd App::update(Model& m, Output o) {
    struct Credit { SessionId sid; ~Credit() { detail::pty_fed(sid); } } credit{o.sid};   // fed: the reader may send the next
    if (o.sid != kNoSession && o.sid != m.sid) {   // a background tab: feed it, mark it
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
                m.toast.card = Toast{.title = (c.exit_code == 0 ? "\u2713 tab " : "\u2717 tab ") + std::to_string(i + 1) + " " +
                                         (c.exit_code == 0 ? "done" : "failed (" + std::to_string(c.exit_code) + ")"),
                                .body = se.term->command_text(c),
                                .kind = c.exit_code == 0 ? Toast::Kind::success : Toast::Kind::error};
                m.toast.at = m.time;
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
        m.toast.card = Toast{.title = title, .body = cmd, .kind = c.exit_code == 0 ? Toast::Kind::success : Toast::Kind::error};
        m.toast.at = m.time;
    }
    if (m.pane.open) cmds.push_back(pane_sync(m));   // a cd lands in the output (OSC 7)
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, Exited e) {
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (m.at(i).sid != e.sid && !(e.sid == kNoSession && i == m.active)) continue;
        Session& se = m.at(i);
        // ssh's own failure (255: refused, timed out, the link died) on a tab hand
        // connected: keep the tab and its scrollback, offer to reconnect. A
        // logout (the remote shell's status) closes it like any shell.
        if (se.link.launched && e.code == 255 && m.cfg.ssh_reconnect && se.remote) {
            se.link.dropped = true;
            se.link.dropped_at = m.time;
            se.pty.reset();
            se.grid_key = 0;
            if (i != m.active) {
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
    return pane_sync(m);   // catches an `ssh` started inside the tab
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
            toward(m.cur_a, m.cursor_on || !m.focused || !m.cfg.cursor_blink ? 1.0f : 0.0f, 16.0f * m.cfg.motion_speed);
        toward(m.settings.t, m.settings.open ? 1.0f : 0.0f, m.cfg.motion ? 22.0f : 1000.0f);
        toward(m.ssh.manager.t, m.ssh.manager.open ? 1.0f : 0.0f, m.cfg.motion ? 22.0f : 1000.0f);
        toward(m.out_shift, 0.0f, 22.0f);
        toward(m.dialog.t, m.dialog.ask.has_value() ? 1.0f : 0.0f, m.cfg.motion ? 20.0f : 1000.0f);
    }
    {   // the peek card and the live tail fade / slide in and out
        const auto ease = [&](float& v, bool on, float rate) {
            v += ((on ? 1.0f : 0.0f) - v) * (1.0f - std::exp(-static_cast<float>(t.dt) * rate));
            if (std::fabs(v - (on ? 1.0f : 0.0f)) < 0.01f) v = on ? 1.0f : 0.0f;
        };
        ease(m.minimap.peek_t, m.cfg.peek && m.minimap.hover_y >= 0.0f, 24.0f);
        ease(m.tail_t, tail_wanted(m), 16.0f);
        const auto st = sticky_of(m);
        if (st && st->id != m.sticky_id) { m.sticky_id = st->id; m.sticky_t = std::min(m.sticky_t, 0.35f); }   // a new command: re-slide
        ease(m.sticky_t, st.has_value(), 20.0f);
    }
    {   // the minimap slides open / shut
        const float goal = m.minimap.open ? kMinimapOpen : kMinimapRest;
        m.minimap.width += (goal - m.minimap.width) * (1.0f - std::exp(-static_cast<float>(t.dt) * 22.0f));
        if (std::fabs(goal - m.minimap.width) < 0.3f) m.minimap.width = goal;
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

}  // namespace hand
