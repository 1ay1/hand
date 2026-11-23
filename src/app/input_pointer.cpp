// hand::App — the pointer over the terminal into messages: the wheel, and
// press / move / release, which become a selection, mouse reports for an app
// that asked for them (xterm 1000/1002/1003), link hovers, or the minimap.

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

std::optional<App::Msg> App::on(const Model& m, const ScrollEvent& s_win) {
    const Session& se = m.tab();
    if (in_strip(m, s_win.position.y)) return std::nullopt;   // the tab strip's
    ScrollEvent s = s_win;
    s.position.y -= term_top(m);   // terminal coordinates from here on
    s.position.x -= term_left(m);
    if (m.settings.open || m.ssh.manager.open) return std::nullopt;   // the panel's scroll_view takes it
    if (in_pane(m, s_win.position.x)) return std::nullopt;   // the file pane's list
    if (se.file) return std::nullopt;   // a file tab: its editor scrolls itself (text_editor's handlers)
    const float ch = std::max(m.metrics.cell_h, 1.0f);
    const int lines = static_cast<int>(std::round(s.delta.y / ch * m.cfg.scroll_lines));
    if (lines == 0 && (se.term->mouse_reporting() || se.term->alt_screen())) return std::nullopt;
    if (se.term->mouse_reporting() && !s.mods.shift) {
        const int col = cell_of(s.position.x, m.metrics.origin_x, m.metrics.cell_w, se.term->cols());
        const int row = cell_of(s.position.y, m.metrics.origin_y, m.metrics.cell_h, se.term->rows());
        std::string out;
        for (int i = 0; i < std::min(std::abs(lines), 10); ++i)
            out += se.term->encode_mouse(lines > 0 ? 64 : 65, col, row, true, false, s.mods);
        return send(out);
    }
    if (se.term->alt_screen()) {   // less/man on the alt screen: wheel = arrows
        std::string out;
        for (int i = 0; i < std::min(std::abs(lines), 10); ++i) out += se.term->encode_key(lines > 0 ? Key::up : Key::down, {});
        return send(out);
    }
    const float rows = s.delta.y / ch * m.cfg.scroll_lines;   // fractional: trackpads scroll by the pixel
    return ScrollBy{rows};
}

std::optional<App::Msg> App::on(const Model& m, const MouseDown& e_win) {
    const Session& se = m.tab();
    if (in_strip(m, e_win.position.y) || in_corner(m, e_win.position)) return std::nullopt;   // the tab strip's / the buttons'
    MouseDown e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    e.position.x -= term_left(m);
    if (m.settings.open || m.ssh.manager.open) return std::nullopt;
    if (in_pane(m, e_win.position.x)) return std::nullopt;   // the file pane's
    if (m.pane.open && m.pane.ui.focused)   // a click on the terminal takes the keyboard back
        return PaneBlur{};
    if (se.file) return std::nullopt;   // a file tab: the editor's own press handler
    if (minimap_on(m) && e.position.x > m.size.x - std::max(m.minimap.width, kMinimapRest)) return std::nullopt;   // the minimap's
    const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, se.term->cols());
    const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, se.term->rows());
    const int btn = e.button == MouseButton::left ? 0 : e.button == MouseButton::middle ? 1 : e.button == MouseButton::right ? 2 : -1;
    if (btn < 0) return std::nullopt;
    if (se.term->mouse_reporting() && !e.mods.shift) {
        auto bytes = se.term->encode_mouse(btn, col, row, true, false, e.mods);
        return ReportPress{btn, std::move(bytes)};
    }
    if (btn == 1) return PasteReq{};   // middle click pastes, as everywhere on Linux
    if (btn == 0 && e.click_count == 1 && (e.mods.primary() || !m.cfg.links_need_ctrl)) {   // ⌘-click on the Mac, ctrl+click on the PC
        if (auto url = se.term->link_at(col, row); !url.empty()) return OpenUrl{std::move(url)};
    }
    if (btn == 0) {
        const int vr = view_row(m, e.position.y);
        // the selection toolbar is a control, not text
        if (const auto hit = e_win.control; hit != 0 &&
            (hit == node_id("sel") || hit == node_id("sel.copy") || hit == node_id("sel.find") || hit == node_id("sel.open") ||
             hit == node_id("sel.clear")))
            return std::nullopt;
        // shift+click: extend what is selected to here
        if (e.mods.shift && se.term->has_selection() && e.click_count == 1)
            return SelectExtend{col, vr, e.position, 0.0f, Extend::shift_click};
        // a click in the gutter on a prompt, or a fourth click: the whole command's output
        const bool gutter = m.cfg.shell_marks && e.position.x < m.metrics.origin_x;
        if (gutter || e.click_count >= 4) {
            const std::int64_t abs = se.term->view_to_abs(vr);
            for (const auto& c : se.term->commands()) {
                if (!c.ran || c.prompt_row < 0) continue;
                const std::int64_t end = c.finished && c.end_row > c.prompt_row ? c.end_row : se.term->total_rows();
                const bool hit = gutter ? abs == c.prompt_row : abs >= c.prompt_row && abs < end;
                if (!hit || c.output_row < 0 || end - 1 < c.output_row) continue;
                return SelectOutput{c.output_row, end - 1};
            }
            if (gutter) return std::nullopt;
        }
        return SelectBegin{col, vr, e.click_count, e.position,
                           e.mods.alt ? SelectShape::block : SelectShape::linear};   // alt+drag: a rectangle
    }
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const MouseMove& e_win) {
    const Session& se = m.tab();
    if (in_strip(m, e_win.position.y) && se.mouse_button < 0 && !se.selecting) return std::nullopt;   // the tab strip's
    MouseMove e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    e.position.x -= term_left(m);
    if (m.settings.open || m.ssh.manager.open) return std::nullopt;
    if (se.file) return std::nullopt;   // no links, minimap or reporting over an editor
    if (in_pane(m, e_win.position.x) && se.mouse_button < 0 && !se.selecting) {   // the file pane's
        // the pointer left the terminal: the scrollback map and its peek fold away
        if (m.minimap.open || m.minimap.hover_y >= 0.0f || !se.hover_url.empty())
            return PointerLeft{};
        return std::nullopt;
    }
    if (!minimap_on(m) && (m.minimap.open || m.minimap.hover_y >= 0.0f))
        return MinimapHover{false, std::nullopt};
    if (minimap_on(m) && se.mouse_button < 0 && !se.selecting) {
        const bool adjacent = !in_corner(m, e_win.position)   // the buttons beside it are theirs
                           && e.position.x > m.size.x - (m.minimap.open ? kMinimapOpen + 8.0f : kMinimapRest + 4.0f);
        const bool over = adjacent && e.position.x > m.size.x - std::max(m.minimap.width, kMinimapRest);
        const float y = over ? e.position.y : -1.0f;
        if (adjacent != m.minimap.open || y != m.minimap.hover_y) return MinimapHover{adjacent, y >= 0.0f ? std::optional<float>{y} : std::nullopt};
        if (adjacent) return std::nullopt;
    }
    // The app wants the mouse: motion is the app's, reported once per CELL
    // (a TUI redraws per report; per pixel floods the pty). Any-motion mode
    // (1003: htop, lazygit, a TUI with hover) gets buttonless motion -- with
    // button 3, "no button" -- the others only drags.
    if (se.term->mouse_reporting() && !se.selecting && !e.mods.shift) {
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, se.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, se.term->rows());
        const bool inside = e.position.x >= 0.0f && e.position.y >= 0.0f && e.position.x < m.size.x && e.position.y < m.size.y;
        if (se.mouse_button >= 0 || (se.term->mouse_any_motion() && inside)) {
            if (col == se.mouse_cell_col && row == se.mouse_cell_row) return std::nullopt;
            auto bytes = se.term->encode_mouse(se.mouse_button >= 0 ? se.mouse_button : 3, col, row, true, true, e.mods);
            return ReportMotion{col, row, std::move(bytes)};
        }
    }
    if (!se.selecting && se.mouse_button < 0) {
        // links read as clickable while the accelerator is held (⌘ / ctrl), or
        // always when they need none
        const bool armed = e.mods.primary() || !m.cfg.links_need_ctrl;
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, se.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, se.term->rows());
        std::string url = armed ? se.term->link_at(col, row) : std::string{};
        if (url == se.hover_url && armed == m.ctrl_held) return std::nullopt;
        return HoverLink{col, row, armed, std::move(url)};
    }
    const int col = cell_of(e.position.x + m.metrics.cell_w * 0.5f, m.metrics.origin_x, m.metrics.cell_w, se.term->cols() + 1);
    if (se.mouse_button >= 0) return std::nullopt;   // a held button with reporting off (shift): nothing to send
    return SelectExtend{col, view_row(m, e.position.y), e.position, se.term->alt_screen() ? 0.0f : edge_speed(m, e.position.y)};
}

std::optional<App::Msg> App::on(const Model& m, const MouseUp& e_win) {
    const Session& se = m.tab();
    if (in_strip(m, e_win.position.y) && se.mouse_button < 0 && !se.selecting) return std::nullopt;   // the tab strip's
    MouseUp e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    e.position.x -= term_left(m);
    if (se.file) return std::nullopt;
    if (se.mouse_button >= 0) {
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, se.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, se.term->rows());
        auto bytes = se.term->encode_mouse(se.mouse_button, col, row, false, false, e.mods);
        return ReportRelease{std::move(bytes)};
    }
    if (se.selecting) {
        // a click that selected no text -- it never left its cell, jittered by a
        // pixel, or landed on blank space -- selects nothing: no highlight, no
        // toolbar, no copy
        const auto si = se.term->selection_info();
        const float dx = e.position.x - se.sel_down.x, dy = e.position.y - se.sel_down.y;
        const float slop = std::max(6.0f, 0.75f * m.metrics.cell_w);   // hand jitter, not a drag
        const bool still = dx * dx + dy * dy < slop * slop;
        if (si.any && (si.chars == 0 || (se.sel_clicks == 1 && (still || (si.r0 == si.r1 && si.c0 == si.c1)))))
            return SelectEnd{SelectEnding::discard};
        return SelectEnd{SelectEnding::keep};
    }
    return std::nullopt;
}

/// Overlay dismissals arrive as Left on the overlay's id.

}  // namespace hand
