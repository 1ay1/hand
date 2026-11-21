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
    Session& se = m.tab();
    const bool block = s.shape == SelectShape::block;
    se.term->select_begin(s.col, s.row, s.clicks, block);   // alt+drag: a rectangle
    se.selecting = true;
    se.sel_clicks = block ? 0 : s.clicks;
    se.sel_down = s.at;
    se.sel_pos = s.at;
    se.sel_edge = 0.0f;
    return {};
}

App::Cmd App::update(Model& m, SelectExtend s) {
    Session& se = m.tab();
    se.term->select_extend(s.col, s.row);
    if (s.how == Extend::shift_click) se.selecting = true;
    se.sel_pos = s.at;
    se.sel_edge = s.edge;
    return {};
}

App::Cmd App::update(Model& m, SelectOutput s) {
    Session& se = m.tab();
    se.term->select_abs(s.first, 0, s.last, se.term->cols() - 1, 1);
    se.selecting = false;
    return {};
}

App::Cmd App::update(Model& m, SelectEnd s) {
    if (s.ending == SelectEnding::discard) {   // a click, not a selection
        m.tab().term->select_clear();
        m.tab().sel_clicks = 0;
    } else if (m.cfg.copy_on_select) {
        return update(m, Copy{});
    }
    m.tab().selecting = false;
    m.tab().sel_edge = 0.0f;
    return {};
}

App::Cmd App::update(Model& m, SelectClear) {
    m.tab().term->select_clear();
    return {};
}

// ── mouse reporting ────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, ReportPress r) {
    Session& se = m.tab();
    se.mouse_button = r.button;
    if (se.pty) se.pty->write(r.bytes);
    return {};
}

App::Cmd App::update(Model& m, ReportMotion r) {
    Session& se = m.tab();
    se.mouse_cell_col = r.col;
    se.mouse_cell_row = r.row;
    if (se.pty && !r.bytes.empty()) se.pty->write(r.bytes);
    return {};
}

App::Cmd App::update(Model& m, ReportRelease r) {
    Session& se = m.tab();
    se.mouse_button = -1;
    if (se.pty && !r.bytes.empty()) se.pty->write(r.bytes);
    return {};
}

// ── hovering ───────────────────────────────────────────────────────────────

App::Cmd App::update(Model& m, HoverLink h) {
    m.ctrl_held = h.armed;
    const bool none = h.url.empty();
    m.tab().hover_url = std::move(h.url);
    (void)m.tab().term->hover(none ? -1 : h.col, none ? -1 : h.row);
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
    m.tab().hover_url.clear();
    return {};
}

}  // namespace hand
