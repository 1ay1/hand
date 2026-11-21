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
    const auto i = m.term->scroll_info();
    const float want = static_cast<float>(i.total - i.visible) - static_cast<float>(std::max<std::int64_t>(s.prompt_row - 1, 0));
    scroll_by(m, std::clamp(want, 0.0f, static_cast<float>(m.term->history())) - m.scroll_goal);
    return {};
}

App::Cmd App::update(Model& m, ScrollToLine s) {
    const auto i = m.term->scroll_info();
    scroll_by(m, static_cast<float>(i.total - i.visible - s.line) - m.scroll_goal);
    return {};
}

App::Cmd App::update(Model& m, JumpPrompt j) {
    // from where the scroll is HEADING, so quick presses chain prompt to prompt
    m.term->set_scroll_offset(static_cast<int>(std::lround(m.scroll_goal)));
    m.scroll_applied = m.term->scroll_offset();
    (void)m.term->jump_prompt(delta(j.step));
    const int to = m.term->scroll_offset();
    m.term->set_scroll_offset(static_cast<int>(std::floor(m.scroll_d)));
    m.scroll_applied = m.term->scroll_offset();
    scroll_by(m, static_cast<float>(to) - m.scroll_goal);
    return {};
}

App::Cmd App::update(Model& m, ClearScrollback) {
    (void)m.term->feed("\x1b[H\x1b[2J\x1b[3J");
    m.scroll_goal = 0.0f;
    m.scroll_d = 0.0f;
    m.scroll_applied = 0;
    m.grid_key = 0;
    if (m.pty) m.pty->write("\x0c");   // the shell redraws its prompt
    return {};
}

}  // namespace hand
