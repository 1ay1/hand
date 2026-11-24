// hand::App — what the program listens to: one reader stream per shell,
// the blink, the 1 Hz clock, and frames only while something animates.

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

void App::subscribe_rest(const Model& m, std::vector<Sub>& s) {
    // every tab's shell keeps running: one reader per session, keyed by its id
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Session& se = m.at(i);
        if (se.pty) s.push_back(Sub::stream("pty." + std::to_string(se.sid.get()), fx::read_pty, se.pty.share(), se.sid));
    }
    if (m.tab_count() > 1) s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Second{}}));   // activity sparklines
    // a dropped ssh tab reconnects by itself, on a schedule
    if (std::any_of(m.tabs.begin(), m.tabs.end(), [](const Session& se) { return se.link.dropped && se.link.retry_at >= 0.0; }))
        s.push_back(Sub::every(std::chrono::milliseconds(250), Msg{RetryTick{}}));
    // the host manager's live tunnel status: who listens on each -L / -D port
    if (m.ssh.manager.open) s.push_back(Sub::every(std::chrono::milliseconds(900), Msg{TunnelProbe{}}));
    // the file pane: re-read its folder (new and changed files glow), and follow `ssh` started in a tab
    if (m.pane.open && m.cfg.files_watch) s.push_back(Sub::every(std::chrono::milliseconds(m.pane.where.empty() ? 1500 : 5000), Msg{PaneWatch{}}));
    // the file in front: did someone else change it? (a formatter, git checkout, another editor)
    if (m.tab().file && !m.tab().file->loading && m.tab().file->error.empty())
        s.push_back(Sub::every(std::chrono::milliseconds(1500), Msg{FileWatch{}}));
    // ...and its syntax lexed ahead in slices, only while it is not all lexed:
    // the subscription ends by itself once the cache covers the file
    if (m.tab().file && !m.tab().file->loading && !m.tab().file->st.ed.syntax_warm())
        s.push_back(Sub::every(std::chrono::milliseconds(10), Msg{WarmSyntax{}}));
    if (m.pane.open && m.tab_count() < 2) s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Second{}}));
    // the blink, until it rests (then keys, output and focus wake it: an idle
    // window has no timer left and draws nothing)
    if (m.focused && m.cfg.cursor_blink && !blink_resting(m))
        s.push_back(Sub::every(std::chrono::milliseconds(static_cast<int>(m.cfg.blink_ms)), Msg{Blink{}}));
}

std::string frames_wanted_by(const Model& m) {
    const auto cur = m.tab().term->cursor();
    const bool gliding = m.cfg.cursor_anim != CursorAnim::none &&
                         (!m.tab().cur_known || !motion::cursor_settled(m.tab().fx, m.cfg.cursor_anim, m.tab().cur_x, m.tab().cur_y,
                                                                  static_cast<float>(cur.col), static_cast<float>(cur.row)));
    const bool breathing = m.cfg.blink_style == BlinkStyle::breathe && m.cfg.cursor_blink && m.focused && !blink_resting(m);
    const bool tailing = m.cfg.cursor_anim != CursorAnim::none && m.cfg.cursor_trail > 0.0f &&
                         !motion::tail_settled(m.tab().fx, m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.col) : m.tab().cur_x,
                                               m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.row) : m.tab().cur_y,
                                               m.metrics.cell_w, m.metrics.cell_h);
    const bool strip_goal = m.cfg.strip_always() || m.tab_count() > 1;
    const bool editing = m.tab().file && !m.tab().file->loading && text_editor_ops::animating(m.tab().file->st);   // the caret glides, a jump slides
    // Every reason the model needs frames, by name: --motion prints the ones
    // that held on a frame that changed no pixel (a fade that ran past
    // visible, a timer counted in frames).
    const std::pair<const char*, bool> reasons[] = {
        {"editor", editing},
        {"pane glow", m.pane.open && pane_glowing(m)},
        {"cursor glide", gliding},
        {"cursor breathe", breathing},
        {"cursor trail", tailing},
        {"cursor fx", m.tab().fx.busy()},
        // the cursor moved: the next Tick decides whether a typed move fires its
        // effect (and records where it is). Without this an effect fired only
        // if something ELSE was animating -- a toast's frames had masked it.
        {"cursor moved", m.cfg.cursor_fx != CursorFx::none && (cur.col != m.tab().fx.last_col || cur.row != m.tab().fx.last_row)},
        {"dialog", m.dialog.ask.has_value() || m.dialog.t > 0.0f},
        {"copy flash", m.cfg.sel_flash && m.time - m.tab().copy_flash_at < 0.45},
        // (the tab pill is a Tracked: the host steps it and asks for its frames
        //  while the strip is drawn -- no reason of ours)
        {"strip slide", m.strip.t != (strip_goal ? 1.0f : 0.0f)},
        {"tab switch", m.strip.switch_t < 1.0f},
        {"tab drag", m.strip.drag_tab >= 0},
        {"tab numbers", m.strip.show_numbers},
        {"scroll glide", m.tab().scroll_d != m.tab().scroll_goal},
        {"output glide", m.tab().out_shift != 0.0f},
        {"bell", m.tab().bell_at >= 0.0 && m.time - m.tab().bell_at < (m.cfg.bell_shake ? 0.4 : m.cfg.visual_bell ? 0.2 : 0.0)},
        {"blink fade", m.cfg.blink_style == BlinkStyle::fade && m.tab().cur_a != (cursor_lit(m) ? 1.0f : 0.0f)},
        {"settings", m.settings.t != (m.settings.open ? 1.0f : 0.0f)},
        {"host manager", m.ssh.manager.t != (m.ssh.manager.open ? 1.0f : 0.0f)},
        {"edge scroll", m.tab().selecting && m.tab().sel_edge != 0.0f},
        {"minimap", m.minimap.width != (m.minimap.open ? kMinimapOpen : kMinimapRest)},
        {"peek", m.minimap.peek_t != ((m.cfg.peek && m.minimap.hover_y >= 0.0f) ? 1.0f : 0.0f)},
        {"live tail", m.tab().tail_t != (tail_wanted(m) ? 1.0f : 0.0f)},
        {"sticky header", m.tab().sticky_t != (sticky_of(m) ? 1.0f : 0.0f)},
        {"link dropped", m.tab().link.dropped && m.time - m.tab().link.dropped_at < 0.3},
    };
    std::string why;
    for (const auto& [name, on] : reasons)
        if (on) why += (why.empty() ? "" : ",") + std::string{name};
    return why;
}

App::Sub App::subscribe(const Model& m) {
    std::vector<Sub> s;
    subscribe_rest(m, s);
    if (const auto st = sticky_of(m); st && st->ran && !st->finished)
        s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Second{}}));
    if (!frames_wanted_by(m).empty()) s.push_back(Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    return Sub::batch(std::move(s));
}

}  // namespace hand
