// hand::App — the pointer over the terminal: selecting text, reporting the
// mouse to an app that asked for it (xterm modes), hovering links, the
// minimap's reveal.
//
//   SelectBegin ─▶ SelectExtend* ─▶ SelectEnd{keep | discard}
//   ReportPress ─▶ ReportMotion* ─▶ ReportRelease        (mouse reporting on)
//
// input.cpp decides WHICH of these a gesture is (and encodes the bytes);
// this file only applies them.

#include "detail.hpp"

namespace hand {

// ── the selection ──────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, SelectBegin s) {
    const bool block = s.shape == SelectShape::block;
    m.term->select_begin(s.col, s.row, s.clicks, block);   // alt+drag: a rectangle
    m.selecting = true;
    m.sel_clicks = block ? 0 : s.clicks;
    m.sel_down = s.at;
    m.sel_pos = s.at;
    m.sel_edge = 0.0f;
    return {};
}

App::Cmd App::update(Model& m, SelectExtend s) {
    m.term->select_extend(s.col, s.row);
    if (s.how == Extend::shift_click) m.selecting = true;
    m.sel_pos = s.at;
    m.sel_edge = s.edge;
    return {};
}

App::Cmd App::update(Model& m, SelectOutput s) {
    m.term->select_abs(s.first, 0, s.last, m.term->cols() - 1, 1);
    m.selecting = false;
    return {};
}

App::Cmd App::update(Model& m, SelectEnd s) {
    if (s.ending == SelectEnding::discard) {   // a click, not a selection
        m.term->select_clear();
        m.sel_clicks = 0;
    } else if (m.cfg.copy_on_select) {
        return update(m, Copy{});
    }
    m.selecting = false;
    m.sel_edge = 0.0f;
    return {};
}

App::Cmd App::update(Model& m, SelectClear) {
    m.term->select_clear();
    return {};
}

// ── mouse reporting ────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, ReportPress r) {
    m.mouse_button = r.button;
    if (m.pty) m.pty->write(r.bytes);
    return {};
}

App::Cmd App::update(Model& m, ReportMotion r) {
    m.mouse_cell_col = r.col;
    m.mouse_cell_row = r.row;
    if (m.pty && !r.bytes.empty()) m.pty->write(r.bytes);
    return {};
}

App::Cmd App::update(Model& m, ReportRelease r) {
    m.mouse_button = -1;
    if (m.pty && !r.bytes.empty()) m.pty->write(r.bytes);
    return {};
}

// ── hovering ───────────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, HoverLink h) {
    m.ctrl_held = h.armed;
    const bool none = h.url.empty();
    m.hover_url = std::move(h.url);
    (void)m.term->hover(none ? -1 : h.col, none ? -1 : h.row);
    return {};
}

App::Cmd App::update(Model& m, MinimapHover h) {
    m.minimap.open = h.open;
    m.minimap.hover_y = h.y.value_or(-1.0f);
    return {};
}

App::Cmd App::update(Model& m, PointerLeft) {
    m.minimap.open = false;
    m.minimap.hover_y = -1.0f;
    m.hover_url.clear();
    return {};
}

}  // namespace hand
