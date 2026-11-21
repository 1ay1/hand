// hand::App — what the program listens to: one reader stream per shell,
// the blink, the 1 Hz clock, and frames only while something animates.

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

App::Sub App::subscribe(const Model& m) {
    std::vector<Sub> s;
    // every tab's shell keeps running: one reader per session, keyed by its id
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Session& se = m.at(i);
        if (se.pty) s.push_back(Sub::stream("pty." + std::to_string(se.sid.get()), read_pty, se.pty.share(), se.sid));
    }
    if (m.tab_count() > 1) s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));   // activity sparklines
    // the host manager's live tunnel status: who listens on each -L / -D port
    if (m.ssh.manager.open) s.push_back(Sub::every(std::chrono::milliseconds(900), Msg{TunnelProbe{}}));
    // the file pane: re-read its folder (new and changed files glow), and follow `ssh` started in a tab
    if (m.pane.open && m.cfg.files_watch) s.push_back(Sub::every(std::chrono::milliseconds(m.pane.where.empty() ? 1500 : 5000), Msg{PaneWatch{}}));
    if (m.pane.open && m.tab_count() < 2) s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));
    if (m.focused && m.cfg.cursor_blink)
        s.push_back(Sub::every(std::chrono::milliseconds(static_cast<int>(m.cfg.blink_ms)), Msg{Blink{}}));
    const auto cur = m.tab().term->cursor();
    const bool gliding = m.cfg.cursor_anim != CursorAnim::none &&
                         (!m.tab().cur_known || !motion::cursor_settled(m.tab().fx, m.cfg.cursor_anim, m.tab().cur_x, m.tab().cur_y,
                                                                  static_cast<float>(cur.col), static_cast<float>(cur.row)));
    const bool breathing = m.cfg.blink_style == BlinkStyle::breathe && m.cfg.cursor_blink && m.focused;
    const bool tailing = m.cfg.cursor_anim != CursorAnim::none && m.cfg.cursor_trail > 0.0f &&
                         !motion::tail_settled(m.tab().fx, m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.col) : m.tab().cur_x,
                                               m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.row) : m.tab().cur_y);
    const bool strip_goal = m.cfg.strip_always() || m.tab_count() > 1;
    const bool animating = (m.pane.open && pane_glowing(m)) || gliding || breathing || tailing || m.tab().fx.busy() || m.dialog.ask.has_value() || m.dialog.t > 0.0f || m.time - m.tab().copy_flash_at < 0.5 || m.strip.motion.animating() ||
                           m.strip.t != (strip_goal ? 1.0f : 0.0f) || m.strip.switch_t < 1.0f || m.strip.drag_tab >= 0 || m.strip.show_numbers || m.tab().scroll_d != m.tab().scroll_goal || m.tab().out_shift != 0.0f ||
                           (m.tab().bell_at >= 0.0 && m.time - m.tab().bell_at < 0.45) ||
                           (m.cfg.blink_style == BlinkStyle::fade && m.tab().cur_a != (m.cursor_on || !m.focused || !m.cfg.cursor_blink ? 1.0f : 0.0f)) ||
                           m.settings.t != (m.settings.open ? 1.0f : 0.0f) || m.ssh.manager.t != (m.ssh.manager.open ? 1.0f : 0.0f) ||
                           (m.tab().selecting && m.tab().sel_edge != 0.0f) ||
                           m.minimap.width != (m.minimap.open ? kMinimapOpen : kMinimapRest) ||
                           m.minimap.peek_t != ((m.cfg.peek && m.minimap.hover_y >= 0.0f) ? 1.0f : 0.0f) ||
                           m.tab().tail_t != (tail_wanted(m) ? 1.0f : 0.0f) ||
                           m.tab().sticky_t != (sticky_of(m) ? 1.0f : 0.0f) || (m.tab().bell_at >= 0.0 && m.time - m.tab().bell_at < 0.2) || m.time - m.resized_at < 0.9 ||
                           (m.toast.card && m.time - m.toast.at < 3.2) || (m.tab().link.dropped && m.time - m.tab().link.dropped_at < 0.3);
    if (const auto st = sticky_of(m); st && st->ran && !st->finished)
        s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));
    if (animating) s.push_back(Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    return Sub::batch(std::move(s));
}

}  // namespace hand
