// hand::App — window input: keys, text, pointer and gestures, into messages.

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

/// The Mac's ⌘ shortcuts, as the chord hand already binds for the same
/// action -- one implementation each. ⌘ never reaches the shell (it has no
/// encoding), so a ⌘ key that maps to nothing is simply consumed.
static std::optional<KeyEvent> mac_chord(const KeyEvent& k) {
    const Mods md = k.mods;
    if (!md.super || md.ctrl) return std::nullopt;
    const auto as = [&](Key key, Mods mods) { return KeyEvent{key, mods, k.repeat}; };
    const Mods cs{.shift = true, .ctrl = true}, c{.ctrl = true};
    switch (k.key) {
        case Key::c: return as(Key::c, cs);                       // copy
        case Key::v: return as(Key::v, cs);                       // paste
        case Key::t: return as(Key::t, cs);                       // new tab
        case Key::w: return as(Key::w, cs);                       // close tab
        case Key::f: return as(Key::f, cs);                       // find
        case Key::g: return as(md.shift ? Key::up : Key::enter, {});   // find next / previous (in the find bar)
        case Key::comma: return as(Key::comma, c);                // settings
        case Key::equal: case Key::minus: case Key::n0:
            return as(k.key, c);                                  // zoom
        case Key::bracket_left: case Key::bracket_right:
            if (md.shift) return as(Key::tab, k.key == Key::bracket_left ? cs : c);   // ⌘⇧[ / ]: previous / next tab
            return as(k.key == Key::bracket_left ? Key::z : Key::x, cs);              // ⌘[ / ]: previous / next prompt
        case Key::up: case Key::down: return as(k.key == Key::up ? Key::z : Key::x, cs);   // ⌘↑/↓: jump prompts (Terminal.app)
        case Key::home: case Key::end: return as(k.key, {.shift = true});            // top / bottom of history
        case Key::page_up: case Key::page_down: return as(k.key, {.shift = true});
        default: break;
    }
    const auto v = static_cast<int>(k.key);
    if (v >= static_cast<int>(Key::n1) && v <= static_cast<int>(Key::n9)) return as(k.key, {.alt = true});   // ⌘1..9: tab n
    return KeyEvent{Key::unknown, md, k.repeat};   // consumed: ⌘ is never typed into the shell
}

std::optional<App::Msg> App::on(const Model& m, const KeyEvent& k_in) {
    const Session& se = m.tab();
    if (k_in.mods.super && !k_in.mods.ctrl && !k_in.mods.alt && !m.settings.open && !m.ssh.picker.open && !m.dialog.ask) {
        // ⌘K: clear screen and history (Terminal.app, iTerm), and the shell redraws its prompt
        if (k_in.key == Key::k)
            return ClearScrollback{};
        if (k_in.key == Key::g && !se.search_open) return std::nullopt;   // nothing to step through
        if (k_in.key >= Key::n1 && k_in.key <= Key::n9 && m.tab_count() < 2) return std::nullopt;
    }
    const auto mac = mac_chord(k_in);
    if (mac && mac->key == Key::unknown) return std::nullopt;
    const KeyEvent k = mac.value_or(k_in);
    const Mods md = k.mods;
    if (md.ctrl && !md.shift && !md.alt && k.key == Key::comma) return ToggleSettings{};
    if (m.settings.open) {
        if (auto op = settings_panel_key(m.settings.ui, setting_specs(m.cfg), k.key, md)) return Panel{std::move(*op)};
        return std::nullopt;
    }
    const bool cs = md.ctrl && md.shift && !md.alt;
    if (cs && k.key == Key::h) return ToggleManager{};
    if (m.ssh.manager.open) {   // the host manager has the keyboard
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt || k.key == Key::super) return std::nullopt;
        if (auto op = ssh_manager_key(m.ssh.manager.ui, picker_hosts(m), unix_now(), k.key, md, &m.ssh.manager.files)) return Manager{std::move(*op)};
        return Swallow{};   // never to the shell underneath
    }
    if (m.ssh.picker.open) {   // the host picker has the keyboard
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt || k.key == Key::super) return std::nullopt;
        if (cs && k.key == Key::s) return OpenHosts{};
        return HostsKey{k.key, md};
    }
    if (cs && k.key == Key::e && !m.dialog.ask) return TogglePane{};
    if (m.pane.open && m.pane.ui.focused && !m.dialog.ask) {   // the file pane has the keyboard
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt || k.key == Key::super) return std::nullopt;
        if (!md.ctrl && !md.super)
            if (auto op = file_pane_key(m.pane.ui, pane_props(m), k.key, md)) return PaneOp{std::move(*op)};
        if (!md.ctrl && !md.super) return Swallow{};   // plain keys never reach the shell
    }
    if (m.dialog.ask.has_value()) {   // the dialog has the keyboard
        const bool paste = m.dialog.ask->kind == Ask::Kind::paste;
        if (k.key == Key::escape) return Answer::dismiss();
        if (k.key == Key::enter) return Answer::choose(paste && md.shift ? 1 : m.dialog.ask->focus);   // shift+enter: one line
        if (k.key == Key::tab || k.key == Key::right || k.key == Key::left) {
            return DialogFocus{(k.key == Key::left || (k.key == Key::tab && md.shift)) ? Step::backward : Step::forward};
        }
        if (k.key == Key::space) return RememberToggle{};
        return Swallow{};
    }
    if (cs && k.key == Key::m)   // copy mode: vim keys through the scrollback
        return ToggleCopyMode{};
    if (se.copy) {
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt) return std::nullopt;
        if (md.ctrl && (k.key == Key::v || k.key == Key::u || k.key == Key::d)) return CopyKey{k.key, md, {}};
        switch (k.key) {   // named keys; letters arrive as text
            case Key::escape: case Key::enter: case Key::left: case Key::right: case Key::up: case Key::down:
            case Key::home: case Key::end: case Key::page_up: case Key::page_down:
                return CopyKey{k.key, md, {}};
            default: return Swallow{};   // nothing reaches the shell in copy mode
        }
    }
    if (k.key == Key::ctrl && !m.ctrl_held) return ModifierDown{Modifier::ctrl};
    // ── tabs: kitty/foot-style chords, plus the browser ones ──────────────
    if (k.key == Key::alt && m.tab_count() > 1 && !m.strip.show_numbers) return ModifierDown{Modifier::alt};
    if (cs && k.key == Key::t) return NewTab{};
    if (cs && k.key == Key::s) return OpenHosts{};
    if (cs && k.key == Key::w) return CloseTab{TabIndex{m.active()}};
    if (se.link.dropped) {   // a dropped ssh tab: enter reconnects, esc / q closes, nothing reaches a pty
        if (k.key == Key::enter || k.key == Key::r) return Reconnect{};
        if (k.key == Key::escape || k.key == Key::q) return CloseTab{TabIndex{m.active()}, Confirm::given};
        return Swallow{};
    }
    if (md.ctrl && k.key == Key::tab) return StepTab{md.shift ? Step::backward : Step::forward};
    if (cs && (k.key == Key::page_down || k.key == Key::page_up)) return MoveTab{k.key == Key::page_down ? Step::forward : Step::backward};
    if (md.ctrl && !md.shift && (k.key == Key::page_down || k.key == Key::page_up)) return StepTab{k.key == Key::page_down ? Step::forward : Step::backward};
    if (cs && (k.key == Key::right || k.key == Key::left)) return StepTab{k.key == Key::right ? Step::forward : Step::backward};
    if (md.alt && !md.ctrl && !md.shift && k.key >= Key::n1 && k.key <= Key::n9) {
        const auto i = static_cast<std::size_t>(static_cast<int>(k.key) - static_cast<int>(Key::n1));
        if (m.tab_count() > 1) return SelectTab{TabIndex{k.key == Key::n9 ? m.tab_count() - 1 : std::min(i, m.tab_count() - 1)}};
    }
    if (se.hint) {
        if (k.key == Key::escape || (cs && k.key == Key::space))
            return CloseHints{};
        if (k.key == Key::backspace)
            return HintBackspace{};
        return std::nullopt;   // letters arrive as text
    }
    if (cs && k.key == Key::space)
        return OpenHints{};
    if (cs && (k.key == Key::z || k.key == Key::x))   // foot's prompt-prev / prompt-next
        return JumpPrompt{k.key == Key::z ? Step::backward : Step::forward};
    if (cs && k.key == Key::g) return CopyOutput{};
    if (cs && k.key == Key::f) return SearchToggle{};
    if (se.search_open) {
        if (k.key == Key::escape) return SearchToggle{};
        if (k.key == Key::enter || k.key == Key::f3) return SearchStep{md.shift ? Step::backward : Step::forward};
        if (k.key == Key::up) return SearchStep{Step::backward};
        if (k.key == Key::down) return SearchStep{Step::forward};
        return SearchKey{k.key, k.mods};
    }
    // foot's bindings
    if (cs && k.key == Key::c) return Copy{};
    if ((cs && k.key == Key::v) || (md.shift && !md.ctrl && k.key == Key::insert)) return PasteReq{};
    if (md.ctrl && !md.alt && !md.shift && (k.key == Key::equal || k.key == Key::minus || k.key == Key::n0))
        return FontZoom{k.key == Key::equal ? Zoom::in : k.key == Key::minus ? Zoom::out : Zoom::reset};
    if (md.shift && !md.ctrl && (k.key == Key::page_up || k.key == Key::page_down) && !se.term->alt_screen()) {
        return ScrollBy{static_cast<float>((k.key == Key::page_up ? 1 : -1) * std::max(se.term->rows() - 2, 1))};
    }
    if (cs && (k.key == Key::up || k.key == Key::down) && !se.term->alt_screen()) {
        return ScrollBy{k.key == Key::up ? 1.0f : -1.0f};
    }
    if (md.shift && !md.ctrl && (k.key == Key::home || k.key == Key::end) && !se.term->alt_screen()) {
        if (k.key == Key::home) return ScrollTop{};
        return ScrollLive{};
    }
    return send(se.term->encode_key(k.key, md));
}

std::optional<App::Msg> App::on(const Model& m, const ComposeEvent& c) {
    // the input method is composing (こん…, ´ before its e): shown at the
    // cursor, sent to the shell only when committed (a TextEvent)
    if (m.settings.open || m.ssh.picker.open || c.text == m.preedit) return std::nullopt;
    return Compose{c.text};
}

std::optional<App::Msg> App::on(const Model& m, const ComposeEndEvent&) {
    if (m.preedit.empty()) return std::nullopt;
    return ComposeEnd{};
}

std::optional<App::Msg> App::on(const Model& m, const TextEvent& t) {
    const Session& se = m.tab();
    if (m.ssh.manager.open) {
        if (auto op = ssh_manager_text(m.ssh.manager.ui, picker_hosts(m), unix_now(), t.text)) return Manager{std::move(*op)};
        return Swallow{};
    }
    if (m.ssh.picker.open)
        return PickerText{t.text};
    if (m.dialog.ask.has_value()) return Swallow{};
    if (m.pane.open && m.pane.ui.focused) {   // the pane reads keys; text only while filtering
        if (auto op = file_pane_text(m.pane.ui, t.text)) return PaneOp{std::move(*op)};
        return Swallow{};
    }
    if (se.link.dropped) return Swallow{};
    if (m.dialog.ask.has_value()) return Swallow{};
    if (se.copy && !m.settings.open) return CopyKey{Key::unknown, {}, t.text};
    if (se.hint) {
        std::string typed = se.hint->typed;
        bool upper = false;
        for (char ch : t.text) {
            if (ch >= 'A' && ch <= 'Z') { upper = true; ch = static_cast<char>(ch - 'A' + 'a'); }
            typed.push_back(ch);
        }
        for (std::size_t i = 0; i < se.hint->labels.size(); ++i)
            if (se.hint->labels[i] == typed) return HintPick{i, upper ? HintUse::insert : HintUse::act};
        return HintTyped{std::move(typed)};
    }
    if (m.settings.open) {
        if (auto op = settings_panel_text(m.settings.ui, setting_specs(m.cfg), t.text)) return Panel{std::move(*op)};
        return std::nullopt;
    }
    if (se.search_open)
        return SearchText{t.text};
    return send(se.term->encode_text(t.text, {}));
}

std::optional<App::Msg> App::on(const Model& m, const ScrollEvent& s_win) {
    const Session& se = m.tab();
    if (in_strip(m, s_win.position.y)) return std::nullopt;   // the tab strip's
    ScrollEvent s = s_win;
    s.position.y -= term_top(m);   // terminal coordinates from here on
    s.position.x -= term_left(m);
    if (m.settings.open || m.ssh.manager.open) return std::nullopt;   // the panel's scroll_view takes it
    if (in_pane(m, s_win.position.x)) return std::nullopt;   // the file pane's list
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

std::optional<App::Msg> App::on(const Model&, const ResizeEvent& r) {
    return Resized{r.size};
}

std::optional<App::Msg> App::on(const Model& m, const FocusEvent& f) {
    return Focused{f.focused, m.tab().term->encode_focus(f.focused)};
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
    if (btn == 0 && e.click_count == 1 && (e.mods.ctrl || !m.cfg.links_need_ctrl)) {
        if (auto url = se.term->link_at(col, row); !url.empty()) return OpenUrl{std::move(url)};
    }
    if (btn == 0) {
        const int vr = view_row(m, e.position.y);
        // the selection toolbar is a control, not text
        if (m.sel_chip.size.x > 0.0f && m.sel_chip.contains(e.position)) return std::nullopt;
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
        // links read as clickable while ctrl is held (or always, when they need no ctrl)
        const bool armed = e.mods.ctrl || !m.cfg.links_need_ctrl;
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
std::optional<App::Msg> App::on(const Model& m, const Left& l) {
    if (m.ssh.picker.open && l.node == part_id("hosts", part_menu)) return PickerClose{};
    if (m.settings.open)
        if (auto op = settings_panel_left(m.settings.ui, "settings", l.node)) return Panel{std::move(*op)};
    return std::nullopt;
}

/// The tab strip: press selects (instantly, like a browser), drag reorders.
std::optional<App::Msg> App::on(const Model& m, const Pressed& p) {
    // no system title bar: the strip's empty space is the title bar. Decided on
    // the PRESS -- a window drag swallows the release, so no click follows it.
    if (!m.cfg.decorations && p.node == node_id("tabs") && p.button == MouseButton::left)
        return WindowAct{p.count >= 2 ? WindowAction::Kind::toggle_maximize : WindowAction::Kind::move};
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        if (p.node == part_id("tabs", i) && p.button == MouseButton::left)
            return TabPress{TabIndex{i}};
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const Dragged& d) {
    if (m.strip.drag_tab < 0) return std::nullopt;
    return TabDrag{d.delta.x};
}

std::optional<App::Msg> App::on(const Model& m, const Released&) {
    if (m.strip.drag_tab < 0) return std::nullopt;
    return TabRelease{};
}

/// The tab strip's buttons, and the search bar's (mayag term_search parts).
std::optional<App::Msg> App::on(const Model& m, const Clicked& c) {
    if (m.ssh.picker.open) {
        if (c.node == part_id("hosts", part_inc)) return ConnectTyped{};
        const auto ranked = picker_rank(m);
        for (std::size_t k = 0; k < ranked.size(); ++k)
            if (c.node == part_id("hosts", k)) return ConnectHost{m.ssh.book.hosts[static_cast<std::size_t>(ranked[k])]};
    }
    if (c.node == node_id("ssh.reconnect")) return Reconnect{};
    if (c.node == node_id("ssh.close")) return CloseTab{TabIndex{m.active()}, Confirm::given};
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (c.node == part_id("tabs", tab_part_close + i)) return CloseTab{TabIndex{i}};
        if (c.node == part_id("tabs", i) && c.button == MouseButton::middle) return CloseTab{TabIndex{i}};
        if (c.node == part_id("tabs", i) && c.count == 2) return NewTab{};   // double-click a tab: a new one beside it
    }
    // the window's own buttons (no system title bar)
    for (const char* bar : {"tabs", "corner"}) {
        if (c.node == part_id(bar, tab_part_window + 0)) return WindowAct{WindowAction::Kind::minimize};
        if (c.node == part_id(bar, tab_part_window + 1)) return WindowAct{WindowAction::Kind::toggle_maximize};
        if (c.node == part_id(bar, tab_part_window + 2)) return RequestQuit{};
    }
    if (c.node == part_id("tabs", part_inc) || (c.node == node_id("tabs") && c.count == 2 && m.cfg.decorations)) return NewTab{};
    if (c.node == part_id("tabs", part_menu)) return OpenHosts{};   // the ⌄ beside + : connect somewhere
    if (m.dialog.ask.has_value()) {
        for (std::size_t i = 0; i < 3; ++i) if (c.node == part_id("ask", i)) return Answer::choose(i);
        if (c.node == part_id("ask", 100)) return RememberToggle{};
    }
    if (c.node == node_id("sel.copy")) return Copy{};
    if (c.node == node_id("sel.find")) return SearchToggle{};
    if (c.node == node_id("sel.open")) return OpenUrl{m.tab().term->selection_text()};
    if (c.node == node_id("sel.clear")) return SelectClear{};
    if (!m.tab().search_open) return std::nullopt;
    if (c.node == part_id("search", 5)) return SearchStep{Step::backward};
    if (c.node == part_id("search", 6)) return SearchStep{Step::forward};
    if (c.node == part_id("search", 9)) return SearchToggle{};
    return std::nullopt;
}

App::Sub App::subscribe(const Model& m) {
    std::vector<Sub> s;
    // every tab's shell keeps running: one reader per session, keyed by its id
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Session& se = m.at(i);
        if (se.pty) s.push_back(Sub::stream("pty." + std::to_string(se.sid.get()), read_pty, se.pty->fd(),
                                            static_cast<int>(se.pty->pid()), se.sid));
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
