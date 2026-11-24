// hand::App — the keyboard and the window around it: bytes to the shell,
// held modifiers, font zoom, the input method, focus, and the window's size.

#include "detail.hpp"

#include <algorithm>

namespace hand {

App::Cmd App::update(Model& m, Typed t) {
    Session& se = m.tab();
    se.fx.typed_at = se.active_at = m.time;   // the motion effects follow keystrokes, pty or not
    ++m.keys_typed;                           // wakes a resting cursor blink
    m.blinks_idle = 0;
    if (t.bytes.find('\r') != std::string::npos) se.fx.enter_at = m.time;
    if (!se.pty && !se.starting) return {};
    se.term->select_clear();
    se.term->scroll_to_bottom();   // typing snaps back to the live screen
    se.scroll_d = se.scroll_goal = 0.0f;
    se.scroll_applied = 0;
    se.new_below = 0;
    m.cursor_on = true;
    se.cur_a = 1.0f;
    se.input += t.bytes;
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
    if (z.ui) {   // the interface: tabs, panels, menus, the pane's header
        switch (z.zoom) {
            case Zoom::in: m.ui_font_size = std::min(m.ui_font_size + 0.5f, 20.0f); break;
            case Zoom::out: m.ui_font_size = std::max(m.ui_font_size - 0.5f, 9.0f); break;
            case Zoom::reset: m.ui_font_size = m.cfg.ui_font_size; break;
        }
        fit(m);
        return {};
    }
    switch (z.zoom) {
        case Zoom::in: m.font_size = std::min(m.font_size + 1.0f, 48.0f); break;
        case Zoom::out: m.font_size = std::max(m.font_size - 1.0f, 6.0f); break;
        case Zoom::reset: m.font_size = m.cfg.font_size; break;
    }
    fit(m);
    // the cursor is the size of the new cells: show it, don't leave the
    // answer to the next blink (an empty screen in the blink's off phase
    // drew the same frame, measured as 200 ms zooms)
    m.cursor_on = true;
    m.tab().cur_a = 1.0f;
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
    Session& se = m.tab();
    m.focused = f.focused;
    m.cursor_on = true;
    m.blinks_idle = 0;
    se.cur_a = 1.0f;
    if (!f.report.empty() && se.pty) se.input += f.report;
    return {};
}

App::Cmd App::update(Model& m, Resized r) {
    Session& se = m.tab();
    const int c0 = se.term ? se.term->cols() : 0, r0 = se.term ? se.term->rows() : 0;
    m.win = r.size;
    fit(m);
    if (m.sized_once && se.term && (se.term->cols() != c0 || se.term->rows() != r0)) {
        m.sized_once = true;
        m.hud_up = true;
        return Cmd::after(std::chrono::milliseconds(800), Msg{HudExpired{++m.hud_serial}});
    }
    m.sized_once = true;
    return {};
}

}  // namespace hand
