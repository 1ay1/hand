// hand::App — the keyboard and the window around it: bytes to the shell,
// held modifiers, font zoom, the input method, focus, and the window's size.

#include "detail.hpp"

#include <algorithm>

namespace hand {

App::Cmd App::update(Model& m, Typed t) {
    m.fx.typed_at = m.active_at = m.time;   // the motion effects follow keystrokes, pty or not
    if (t.bytes.find('\r') != std::string::npos) m.fx.enter_at = m.time;
    if (!m.pty) return {};
    m.term->select_clear();
    m.term->scroll_to_bottom();   // typing snaps back to the live screen
    m.scroll_d = m.scroll_goal = 0.0f;
    m.scroll_applied = 0;
    m.new_below = 0;
    m.cursor_on = true;
    m.cur_a = 1.0f;
    m.pty->write(t.bytes);
    return {};
}

App::Cmd App::update(Model& m, ModifierDown d) {
    switch (d.mod) {
        case Modifier::ctrl: m.ctrl_held = true; break;   // links arm
        case Modifier::alt:                               // alt+N hints on the tabs
            m.strip.show_numbers = true;
            m.strip.numbers_at = m.time;
            break;
    }
    return {};
}

App::Cmd App::update(Model& m, FontZoom z) {
    switch (z.zoom) {
        case Zoom::in: m.font_size = std::min(m.font_size + 1.0f, 48.0f); break;
        case Zoom::out: m.font_size = std::max(m.font_size - 1.0f, 6.0f); break;
        case Zoom::reset: m.font_size = m.cfg.font_size; break;
    }
    fit(m);
    return {};
}

App::Cmd App::update(Model& m, Compose c) {
    m.preedit = std::move(c.preedit);
    return {};
}

App::Cmd App::update(Model& m, ComposeEnd) {
    m.preedit.clear();
    return {};
}

App::Cmd App::update(Model& m, Focused f) {
    m.focused = f.focused;
    m.cursor_on = true;
    m.cur_a = 1.0f;
    if (!f.report.empty() && m.pty) m.pty->write(f.report);
    return {};
}

App::Cmd App::update(Model& m, Resized r) {
    const int c0 = m.term ? m.term->cols() : 0, r0 = m.term ? m.term->rows() : 0;
    m.win = r.size;
    fit(m);
    if (m.sized_once && m.term && (m.term->cols() != c0 || m.term->rows() != r0)) m.resized_at = m.time;
    m.sized_once = true;
    return {};
}

}  // namespace hand
