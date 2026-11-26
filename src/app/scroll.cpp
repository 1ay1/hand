// hand::App — the scrollback: where the view is, and every way to move it.
//
//   ScrollBy / ScrollLive / ScrollTop     the wheel, shift+pgup, shift+home, "back to live"
//   ScrollToPrompt / ScrollToLine         the sticky header, the minimap
//   JumpPrompt                            ctrl+shift+z / x: prompt to prompt
//   ClearScrollback                       ⌘K
//
// Every move sets a GOAL; the Tick eases the drawn offset toward it
// (App::scroll_by / settle_scroll in session.cpp).

#include "detail.hpp"

#include <algorithm>
#include <cmath>

namespace hand {

namespace {
/// Far enough to clamp to either end of any history.
constexpr float kFar = 1e9f;
}  // namespace

App::Cmd App::update(Model& m, ScrollBy s) {
    scroll_by(m, s.rows);
    return {};
}

App::Cmd App::update(Model& m, ScrollLive) {
    scroll_by(m, -kFar);
    return {};
}

App::Cmd App::update(Model& m, ScrollTop) {
    scroll_by(m, kFar);
    return {};
}

App::Cmd App::update(Model& m, ScrollToPrompt s) {
    Session& se = m.tab();
    const auto i = se.term->scroll_info();
    const float want = static_cast<float>(i.total - i.visible) - static_cast<float>(std::max<std::int64_t>(s.prompt_row - 1, 0));
    scroll_by(m, std::clamp(want, 0.0f, static_cast<float>(se.term->history())) - se.scroll_goal);
    return {};
}

App::Cmd App::update(Model& m, ScrollToLine s) {
    Session& se = m.tab();
    const auto i = se.term->scroll_info();
    const float want = static_cast<float>(static_cast<double>(i.total - i.visible) - s.line);
    if (!s.follow) {
        scroll_by(m, want - se.scroll_goal);
        return {};
    }
    // A scrollbar drag: the content is the pointer's, at the exact
    // (fractional) row under it, every frame -- no glide toward a goal that
    // jumps a whole line per pixel of travel (that stepped, then eased, then
    // stepped: the drag jerked). The glide state lands where the drag is, so
    // releasing leaves nothing to settle.
    const float top = static_cast<float>(se.term->history());
    se.scroll_goal = se.scroll_d = std::clamp(want, 0.0f, top);
    const int whole = static_cast<int>(std::floor(se.scroll_d));
    if (whole != se.scroll_applied) { se.term->set_scroll_offset(whole); se.scroll_applied = whole; }
    if (se.scroll_d == 0.0f) se.new_below = 0;
    return {};
}

App::Cmd App::update(Model& m, JumpPrompt j) {
    Session& se = m.tab();
    // from where the scroll is HEADING, so quick presses chain prompt to prompt
    se.term->set_scroll_offset(static_cast<int>(std::lround(se.scroll_goal)));
    se.scroll_applied = se.term->scroll_offset();
    (void)se.term->jump_prompt(delta(j.step));
    const int to = se.term->scroll_offset();
    se.term->set_scroll_offset(static_cast<int>(std::floor(se.scroll_d)));
    se.scroll_applied = se.term->scroll_offset();
    scroll_by(m, static_cast<float>(to) - se.scroll_goal);
    return {};
}

App::Cmd App::update(Model& m, ClearScrollback) {
    Session& se = m.tab();
    (void)se.term->feed("\x1b[H\x1b[2J\x1b[3J");
    se.scroll_goal = 0.0f;
    se.scroll_d = 0.0f;
    se.scroll_applied = 0;
    se.grid_key = 0;
    if (se.pty) se.input += "\x0c";   // the shell redraws its prompt
    return {};
}

}  // namespace hand
