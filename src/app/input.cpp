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
    if (k_in.mods.super && !k_in.mods.ctrl && !k_in.mods.alt && !m.settings_open && !m.hosts_open && !m.ask) {
        // ⌘K: clear screen and history (Terminal.app, iTerm), and the shell redraws its prompt
        if (k_in.key == Key::k)
            return E{[](Model& mm) {
                (void)mm.term->feed("\x1b[H\x1b[2J\x1b[3J");
                mm.scroll_goal = 0.0f; mm.scroll_d = 0.0f; mm.scroll_applied = 0; mm.grid_key = 0;
                if (mm.pty) mm.pty->write("\x0c");
            }};
        if (k_in.key == Key::g && !m.search_open) return std::nullopt;   // nothing to step through
        if (k_in.key >= Key::n1 && k_in.key <= Key::n9 && m.tab_count() < 2) return std::nullopt;
    }
    const auto mac = mac_chord(k_in);
    if (mac && mac->key == Key::unknown) return std::nullopt;
    const KeyEvent k = mac.value_or(k_in);
    const Mods md = k.mods;
    if (md.ctrl && !md.shift && !md.alt && k.key == Key::comma) return ToggleSettings{};
    if (m.settings_open) {
        if (auto op = settings_panel_key(m.panel, setting_specs(m.cfg), k.key, md)) return Panel{std::move(*op)};
        return std::nullopt;
    }
    const bool cs = md.ctrl && md.shift && !md.alt;
    if (m.hosts_open) {   // the host picker has the keyboard
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt || k.key == Key::super) return std::nullopt;
        if (cs && k.key == Key::s) return OpenHosts{};
        return HostsKey{k.key, md};
    }
    if (m.ask.has_value()) {   // the dialog has the keyboard
        const int n = (m.ask && m.ask->kind == Ask::Kind::paste) ? 3 : 2;
        if (k.key == Key::escape) return Answer{-1};
        if (k.key == Key::enter) return Answer{(m.ask && m.ask->kind == Ask::Kind::paste) && md.shift ? 1 : m.ask->focus};
        if (k.key == Key::tab || k.key == Key::right || k.key == Key::left) {
            const int d = (k.key == Key::left || (k.key == Key::tab && md.shift)) ? -1 : 1;
            return E{[d, n](Model& mm) { if (mm.ask) mm.ask->focus = ((mm.ask->focus + d) % n + n) % n; }};
        }
        if (k.key == Key::space) return Answer{100};
        return E{[](Model&) {}};
    }
    if (cs && k.key == Key::m)   // copy mode: vim keys through the scrollback
        return E{[](Model& mm) {
            mm.grid_key = 0;
            if (mm.copy) { mm.copy.reset(); mm.term->select_clear(); return; }
            // from the selection's end, else from the live cursor
            const auto si = mm.term->selection_info();
            mm.copy = si.any ? CopyMode{.row = si.r1, .col = si.c1} : CopyMode{.row = mm.term->cursor_abs(), .col = mm.term->cursor().col};
        }};
    if (m.copy) {
        if (k.key == Key::ctrl || k.key == Key::shift || k.key == Key::alt) return std::nullopt;
        if (md.ctrl && (k.key == Key::v || k.key == Key::u || k.key == Key::d)) return CopyKey{k.key, md, {}};
        switch (k.key) {   // named keys; letters arrive as text
            case Key::escape: case Key::enter: case Key::left: case Key::right: case Key::up: case Key::down:
            case Key::home: case Key::end: case Key::page_up: case Key::page_down:
                return CopyKey{k.key, md, {}};
            default: return E{[](Model&) {}};   // nothing reaches the shell in copy mode
        }
    }
    if (k.key == Key::ctrl && !m.ctrl_held) return E{[](Model& mm) { mm.ctrl_held = true; }};
    // ── tabs: kitty/foot-style chords, plus the browser ones ──────────────
    if (k.key == Key::alt && m.tab_count() > 1 && !m.show_numbers) return E{[](Model& mm) { mm.show_numbers = true; mm.numbers_at = mm.time; }};
    if (cs && k.key == Key::t) return NewTab{};
    if (cs && k.key == Key::s) return OpenHosts{};
    if (cs && k.key == Key::w) return CloseTab{m.active};
    if (m.link.dropped) {   // a dropped ssh tab: enter reconnects, esc / q closes, nothing reaches a pty
        if (k.key == Key::enter || k.key == Key::r) return Reconnect{};
        if (k.key == Key::escape || k.key == Key::q) return CloseTab{m.active, true};
        return E{[](Model&) {}};
    }
    if (md.ctrl && k.key == Key::tab) return StepTab{md.shift ? -1 : 1};
    if (cs && (k.key == Key::page_down || k.key == Key::page_up)) return MoveTab{k.key == Key::page_down ? 1 : -1};
    if (md.ctrl && !md.shift && (k.key == Key::page_down || k.key == Key::page_up)) return StepTab{k.key == Key::page_down ? 1 : -1};
    if (cs && (k.key == Key::right || k.key == Key::left)) return StepTab{k.key == Key::right ? 1 : -1};
    if (md.alt && !md.ctrl && !md.shift && k.key >= Key::n1 && k.key <= Key::n9) {
        const auto i = static_cast<std::size_t>(static_cast<int>(k.key) - static_cast<int>(Key::n1));
        if (m.tab_count() > 1) return SelectTab{k.key == Key::n9 ? m.tab_count() - 1 : std::min(i, m.tab_count() - 1)};
    }
    if (m.hint) {
        if (k.key == Key::escape || (cs && k.key == Key::space))
            return E{[](Model& mm) { mm.hint.reset(); mm.grid_key = 0; }};
        if (k.key == Key::backspace)
            return E{[](Model& mm) { if (mm.hint && !mm.hint->typed.empty()) mm.hint->typed.pop_back(); mm.grid_key = 0; }};
        return std::nullopt;   // letters arrive as text
    }
    if (cs && k.key == Key::space)
        return E{[](Model& mm) {
            HintMode h{.hints = mm.term->hints()};
            // home row first, then the rest; two letters once those run out
            static constexpr std::string_view keys = "asdfjklghqwertyuiopzxcvbnm";
            const std::size_t n = h.hints.size();
            for (std::size_t i = 0; i < n; ++i)
                h.labels.push_back(n <= keys.size() ? std::string(1, keys[i])
                                                    : std::string{keys[i / keys.size() % keys.size()], keys[i % keys.size()]});
            mm.grid_key = 0;
            if (!h.hints.empty()) mm.hint = std::move(h);
            else { mm.toast = Toast{.title = "nothing to pick", .body = "no links, paths, hashes or IPs on screen"}; mm.toast_at = mm.time; }
        }};
    if (cs && (k.key == Key::z || k.key == Key::x)) {   // foot's prompt-prev / prompt-next
        const int d = k.key == Key::z ? -1 : 1;
        return E{[d](Model& mm) {
            // from where the scroll is HEADING, so quick presses chain prompt to prompt
            mm.term->set_scroll_offset(static_cast<int>(std::lround(mm.scroll_goal)));
            mm.scroll_applied = mm.term->scroll_offset();
            (void)mm.term->jump_prompt(d);
            const int to = mm.term->scroll_offset();
            mm.term->set_scroll_offset(static_cast<int>(std::floor(mm.scroll_d)));
            mm.scroll_applied = mm.term->scroll_offset();
            scroll_by(mm, static_cast<float>(to) - mm.scroll_goal);
        }};
    }
    if (cs && k.key == Key::g) return CopyOutput{};
    if (cs && k.key == Key::f) return Search{0};
    if (m.search_open) {
        if (k.key == Key::escape) return Search{0};
        if (k.key == Key::enter || k.key == Key::f3) return Search{md.shift ? -1 : 1};
        if (k.key == Key::up) return Search{-1};
        if (k.key == Key::down) return Search{1};
        return E{[k](Model& mm) {
            if (mm.search_query.handle_key(k.key, k.mods)) (void)mm.term->search(mm.search_query.text);
        }};
    }
    // foot's bindings
    if (cs && k.key == Key::c) return Copy{};
    if ((cs && k.key == Key::v) || (md.shift && !md.ctrl && k.key == Key::insert)) return PasteReq{};
    if (md.ctrl && !md.alt && !md.shift && (k.key == Key::equal || k.key == Key::minus || k.key == Key::n0)) {
        const float d = k.key == Key::equal ? 1.0f : k.key == Key::minus ? -1.0f : 0.0f;
        return E{[d](Model& mm) {
            mm.font_size = d == 0.0f ? mm.cfg.font_size : std::clamp(mm.font_size + d, 6.0f, 48.0f);
            fit(mm);
        }};
    }
    if (md.shift && !md.ctrl && (k.key == Key::page_up || k.key == Key::page_down) && !m.term->alt_screen()) {
        const float d = static_cast<float>((k.key == Key::page_up ? 1 : -1) * std::max(m.term->rows() - 2, 1));
        return E{[d](Model& mm) { scroll_by(mm, d); }};
    }
    if (cs && (k.key == Key::up || k.key == Key::down) && !m.term->alt_screen()) {
        const float d = k.key == Key::up ? 1.0f : -1.0f;
        return E{[d](Model& mm) { scroll_by(mm, d); }};
    }
    if (md.shift && !md.ctrl && (k.key == Key::home || k.key == Key::end) && !m.term->alt_screen()) {
        const bool top = k.key == Key::home;
        return E{[top](Model& mm) { scroll_by(mm, top ? 1e9f : -1e9f); }};
    }
    return send(m.term->encode_key(k.key, md));
}

std::optional<App::Msg> App::on(const Model& m, const ComposeEvent& c) {
    // the input method is composing (こん…, ´ before its e): shown at the
    // cursor, sent to the shell only when committed (a TextEvent)
    if (m.settings_open || m.hosts_open || c.text == m.preedit) return std::nullopt;
    return E{[t = c.text](Model& mm) { mm.preedit = t; }};
}

std::optional<App::Msg> App::on(const Model& m, const ComposeEndEvent&) {
    if (m.preedit.empty()) return std::nullopt;
    return E{[](Model& mm) { mm.preedit.clear(); }};
}

std::optional<App::Msg> App::on(const Model& m, const TextEvent& t) {
    if (m.hosts_open)
        return E{[s = t.text](Model& mm) { mm.hosts_query.insert(s); mm.hosts_sel = 0; }};
    if (m.ask.has_value()) return E{[](Model&) {}};
    if (m.link.dropped) return E{[](Model&) {}};
    if (m.ask.has_value()) return E{[](Model&) {}};
    if (m.copy && !m.settings_open) return CopyKey{Key::unknown, {}, t.text};
    if (m.hint) {
        std::string typed = m.hint->typed;
        bool upper = false;
        for (char ch : t.text) {
            if (ch >= 'A' && ch <= 'Z') { upper = true; ch = static_cast<char>(ch - 'A' + 'a'); }
            typed.push_back(ch);
        }
        for (std::size_t i = 0; i < m.hint->labels.size(); ++i)
            if (m.hint->labels[i] == typed) return HintPick{i, upper};
        return E{[typed](Model& mm) { if (mm.hint) mm.hint->typed = typed; mm.grid_key = 0; }};
    }
    if (m.settings_open) {
        if (auto op = settings_panel_text(m.panel, setting_specs(m.cfg), t.text)) return Panel{std::move(*op)};
        return std::nullopt;
    }
    if (m.search_open)
        return E{[s = t.text](Model& mm) { mm.search_query.insert(s); (void)mm.term->search(mm.search_query.text); }};
    return send(m.term->encode_text(t.text, {}));
}

std::optional<App::Msg> App::on(const Model& m, const ScrollEvent& s_win) {
    if (in_strip(m, s_win.position.y)) return std::nullopt;   // the tab strip's
    ScrollEvent s = s_win;
    s.position.y -= term_top(m);   // terminal coordinates from here on
    if (m.settings_open) return std::nullopt;   // the panel's scroll_view takes it
    const float ch = std::max(m.metrics.cell_h, 1.0f);
    const int lines = static_cast<int>(std::round(s.delta.y / ch * m.cfg.scroll_lines));
    if (lines == 0 && (m.term->mouse_reporting() || m.term->alt_screen())) return std::nullopt;
    if (m.term->mouse_reporting() && !s.mods.shift) {
        const int col = cell_of(s.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(s.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        std::string out;
        for (int i = 0; i < std::min(std::abs(lines), 10); ++i)
            out += m.term->encode_mouse(lines > 0 ? 64 : 65, col, row, true, false, s.mods);
        return send(out);
    }
    if (m.term->alt_screen()) {   // less/man on the alt screen: wheel = arrows
        std::string out;
        for (int i = 0; i < std::min(std::abs(lines), 10); ++i) out += m.term->encode_key(lines > 0 ? Key::up : Key::down, {});
        return send(out);
    }
    const float rows = s.delta.y / ch * m.cfg.scroll_lines;   // fractional: trackpads scroll by the pixel
    return E{[rows](Model& mm) { scroll_by(mm, rows); }};
}

std::optional<App::Msg> App::on(const Model&, const ResizeEvent& r) {
    return E{[s = r.size](Model& mm) {
        const int c0 = mm.term ? mm.term->cols() : 0, r0 = mm.term ? mm.term->rows() : 0;
        mm.win = s;
        fit(mm);
        if (mm.sized_once && mm.term && (mm.term->cols() != c0 || mm.term->rows() != r0)) mm.resized_at = mm.time;
        mm.sized_once = true;
    }};
}

std::optional<App::Msg> App::on(const Model& m, const FocusEvent& f) {
    return E{[on = f.focused, rep = m.term->encode_focus(f.focused)](Model& mm) {
        mm.focused = on;
        mm.cursor_on = true;
        mm.cur_a = 1.0f;
        if (!rep.empty() && mm.pty) mm.pty->write(rep);
    }};
}

std::optional<App::Msg> App::on(const Model& m, const MouseDown& e_win) {
    if (in_strip(m, e_win.position.y) || in_corner(m, e_win.position)) return std::nullopt;   // the tab strip's / the buttons'
    MouseDown e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    if (m.settings_open) return std::nullopt;
    if (minimap_on(m) && e.position.x > m.size.x - std::max(m.mm_w, kMinimapRest)) return std::nullopt;   // the minimap's
    const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
    const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
    const int btn = e.button == MouseButton::left ? 0 : e.button == MouseButton::middle ? 1 : e.button == MouseButton::right ? 2 : -1;
    if (btn < 0) return std::nullopt;
    if (m.term->mouse_reporting() && !e.mods.shift) {
        auto bytes = m.term->encode_mouse(btn, col, row, true, false, e.mods);
        return E{[btn, b = std::move(bytes)](Model& mm) { mm.mouse_button = btn; if (mm.pty) mm.pty->write(b); }};
    }
    if (btn == 1) return PasteReq{};   // middle click pastes, as everywhere on Linux
    if (btn == 0 && e.click_count == 1 && (e.mods.ctrl || !m.cfg.links_need_ctrl)) {
        if (auto url = m.term->link_at(col, row); !url.empty()) return OpenUrl{std::move(url)};
    }
    if (btn == 0) {
        const int vr = view_row(m, e.position.y);
        // the selection toolbar is a control, not text
        if (m.sel_chip.size.x > 0.0f && m.sel_chip.contains(e.position)) return std::nullopt;
        // shift+click: extend what is selected to here
        if (e.mods.shift && m.term->has_selection() && e.click_count == 1)
            return E{[col, vr, p = e.position](Model& mm) {
                mm.term->select_extend(col, vr);
                mm.selecting = true; mm.sel_pos = p; mm.sel_edge = 0.0f;
            }};
        // a click in the gutter on a prompt, or a fourth click: the whole command's output
        const bool gutter = m.cfg.shell_marks && e.position.x < m.metrics.origin_x;
        if (gutter || e.click_count >= 4) {
            const std::int64_t abs = m.term->view_to_abs(vr);
            for (const auto& c : m.term->commands()) {
                if (!c.ran || c.prompt_row < 0) continue;
                const std::int64_t end = c.finished && c.end_row > c.prompt_row ? c.end_row : m.term->total_rows();
                const bool hit = gutter ? abs == c.prompt_row : abs >= c.prompt_row && abs < end;
                if (!hit || c.output_row < 0 || end - 1 < c.output_row) continue;
                return E{[o0 = c.output_row, o1 = end - 1](Model& mm) {
                    mm.term->select_abs(o0, 0, o1, mm.term->cols() - 1, 1);
                    mm.selecting = false;
                }};
            }
            if (gutter) return std::nullopt;
        }
        return E{[col, row = vr, n = e.click_count, p = e.position, block = e.mods.alt](Model& mm) {
            mm.term->select_begin(col, row, n, block);   // alt+drag: a rectangle
            mm.selecting = true;
            mm.sel_clicks = block ? 0 : n;
            mm.sel_pos = p;
            mm.sel_edge = 0.0f;
        }};
    }
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const MouseMove& e_win) {
    if (in_strip(m, e_win.position.y) && m.mouse_button < 0 && !m.selecting) return std::nullopt;   // the tab strip's
    MouseMove e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    if (m.settings_open) return std::nullopt;
    if (!minimap_on(m) && (m.mm_open || m.mm_hover_y >= 0.0f))
        return E{[](Model& mm) { mm.mm_open = false; mm.mm_hover_y = -1.0f; }};
    if (minimap_on(m) && m.mouse_button < 0 && !m.selecting) {
        const bool near = !in_corner(m, e_win.position)   // the buttons beside it are theirs
                       && e.position.x > m.size.x - (m.mm_open ? kMinimapOpen + 8.0f : kMinimapRest + 4.0f);
        const bool over = near && e.position.x > m.size.x - std::max(m.mm_w, kMinimapRest);
        const float y = over ? e.position.y : -1.0f;
        if (near != m.mm_open || y != m.mm_hover_y) return E{[near, y](Model& mm) { mm.mm_open = near; mm.mm_hover_y = y; }};
        if (near) return std::nullopt;
    }
    if (!m.selecting && m.mouse_button < 0) {
        // links read as clickable while ctrl is held (or always, when they need no ctrl)
        const bool armed = e.mods.ctrl || !m.cfg.links_need_ctrl;
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        std::string url = armed ? m.term->link_at(col, row) : std::string{};
        if (url == m.hover_url && armed == m.ctrl_held) return std::nullopt;
        return E{[col, row, armed, u = std::move(url)](Model& mm) {
            mm.ctrl_held = armed;
            mm.hover_url = u;
            (void)mm.term->hover(u.empty() ? -1 : col, u.empty() ? -1 : row);
        }};
    }
    const int col = cell_of(e.position.x + m.metrics.cell_w * 0.5f, m.metrics.origin_x, m.metrics.cell_w, m.term->cols() + 1);
    const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
    if (m.mouse_button >= 0) {
        auto bytes = m.term->encode_mouse(m.mouse_button, std::min(col, m.term->cols() - 1), row, true, true, e.mods);
        if (bytes.empty()) return std::nullopt;
        return E{[b = std::move(bytes)](Model& mm) { if (mm.pty) mm.pty->write(b); }};
    }
    return E{[col, row = view_row(m, e.position.y), p = e.position, speed = m.term->alt_screen() ? 0.0f : edge_speed(m, e.position.y)](Model& mm) {
        mm.term->select_extend(col, row);
        mm.sel_pos = p;
        mm.sel_edge = speed;
    }};
}

std::optional<App::Msg> App::on(const Model& m, const MouseUp& e_win) {
    if (in_strip(m, e_win.position.y) && m.mouse_button < 0 && !m.selecting) return std::nullopt;   // the tab strip's
    MouseUp e = e_win;
    e.position.y -= term_top(m);   // terminal coordinates from here on
    if (m.mouse_button >= 0) {
        const int col = cell_of(e.position.x, m.metrics.origin_x, m.metrics.cell_w, m.term->cols());
        const int row = cell_of(e.position.y, m.metrics.origin_y, m.metrics.cell_h, m.term->rows());
        auto bytes = m.term->encode_mouse(m.mouse_button, col, row, false, false, e.mods);
        return E{[b = std::move(bytes)](Model& mm) { mm.mouse_button = -1; if (mm.pty && !b.empty()) mm.pty->write(b); }};
    }
    if (m.selecting) {
        // a plain click that never left its cell selects nothing: no highlight, no toolbar, no copy
        if (m.sel_clicks == 1) {
            const auto si = m.term->selection_info();
            if (si.any && si.r0 == si.r1 && si.c0 == si.c1)
                return E{[](Model& mm) { mm.term->select_clear(); mm.selecting = false; mm.sel_edge = 0.0f; mm.sel_clicks = 0; }};
        }
        if (m.cfg.copy_on_select) return Copy{};
        return E{[](Model& mm) { mm.selecting = false; mm.sel_edge = 0.0f; }};
    }
    return std::nullopt;
}

/// Overlay dismissals arrive as Left on the overlay's id.
std::optional<App::Msg> App::on(const Model& m, const Left& l) {
    if (m.hosts_open && l.node == part_id("hosts", part_menu)) return E{[](Model& mm) { mm.hosts_open = false; }};
    if (m.settings_open)
        if (auto op = settings_panel_left(m.panel, "settings", l.node)) return Panel{std::move(*op)};
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
            return E{[i](Model& mm) {
                (void)update(mm, SelectTab{i});
                mm.drag_tab = static_cast<int>(mm.active);
                mm.drag_dx = 0.0f;
            }};
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const Dragged& d) {
    if (m.drag_tab < 0) return std::nullopt;
    return E{[dx = d.delta.x](Model& mm) {
        mm.drag_dx += dx;
        // past half a tab: swap with the neighbour (the pill slides after it)
        const float tab_w = std::clamp(mm.win.x / static_cast<float>(mm.tab_count()), 96.0f, 236.0f);
        if (mm.drag_dx > tab_w * 0.5f && mm.active + 1 < mm.tab_count()) { (void)update(mm, MoveTab{1}); mm.drag_dx -= tab_w; }
        else if (mm.drag_dx < -tab_w * 0.5f && mm.active > 0) { (void)update(mm, MoveTab{-1}); mm.drag_dx += tab_w; }
        mm.drag_tab = static_cast<int>(mm.active);
    }};
}

std::optional<App::Msg> App::on(const Model& m, const Released&) {
    if (m.drag_tab < 0) return std::nullopt;
    return E{[](Model& mm) { mm.drag_tab = -1; mm.drag_dx = 0.0f; }};
}

/// The tab strip's buttons, and the search bar's (mayag term_search parts).
std::optional<App::Msg> App::on(const Model& m, const Clicked& c) {
    if (m.hosts_open) {
        if (c.node == part_id("hosts", part_inc)) return ConnectTyped{};
        const auto ranked = picker_rank(m);
        for (std::size_t k = 0; k < ranked.size(); ++k)
            if (c.node == part_id("hosts", k)) return ConnectHost{m.book.hosts[static_cast<std::size_t>(ranked[k])]};
    }
    if (c.node == node_id("ssh.reconnect")) return Reconnect{};
    if (c.node == node_id("ssh.close")) return CloseTab{m.active, true};
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (c.node == part_id("tabs", tab_part_close + i)) return CloseTab{i};
        if (c.node == part_id("tabs", i) && c.button == MouseButton::middle) return CloseTab{i};
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
    if (m.ask.has_value()) {
        for (int i = 0; i < 3; ++i) if (c.node == part_id("ask", static_cast<std::size_t>(i))) return Answer{i};
        if (c.node == part_id("ask", 100)) return Answer{100};
    }
    if (c.node == node_id("sel.copy")) return Copy{};
    if (c.node == node_id("sel.find")) return Search{0};
    if (c.node == node_id("sel.open")) return OpenUrl{m.term->selection_text()};
    if (c.node == node_id("sel.clear")) return E{[](Model& mm) { mm.term->select_clear(); }};
    if (!m.search_open) return std::nullopt;
    if (c.node == part_id("search", 5)) return Search{-1};
    if (c.node == part_id("search", 6)) return Search{1};
    if (c.node == part_id("search", 9)) return Search{0};
    return std::nullopt;
}

App::Sub App::subscribe(const Model& m) {
    std::vector<Sub> s;
    // every tab's shell keeps running: one reader per session, keyed by its id
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Session& se = m.at(i);
        if (se.pty) s.push_back(Sub::stream("pty." + std::to_string(se.sid), read_pty, se.pty->fd(),
                                            static_cast<int>(se.pty->pid()), se.sid));
    }
    if (m.tab_count() > 1) s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));   // activity sparklines
    if (m.focused && m.cfg.cursor_blink)
        s.push_back(Sub::every(std::chrono::milliseconds(static_cast<int>(m.cfg.blink_ms)), Msg{Blink{}}));
    const auto cur = m.term->cursor();
    const bool gliding = m.cfg.cursor_anim != CursorAnim::none &&
                         (!m.cur_known || !motion::cursor_settled(m.fx, m.cfg.cursor_anim, m.cur_x, m.cur_y,
                                                                  static_cast<float>(cur.col), static_cast<float>(cur.row)));
    const bool breathing = m.cfg.blink_style == BlinkStyle::breathe && m.cfg.cursor_blink && m.focused;
    const bool tailing = m.cfg.cursor_anim != CursorAnim::none && m.cfg.cursor_trail > 0.0f &&
                         !motion::tail_settled(m.fx, m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.col) : m.cur_x,
                                               m.cfg.cursor_anim == CursorAnim::smear ? static_cast<float>(cur.row) : m.cur_y);
    const bool strip_goal = m.cfg.strip_always() || m.tab_count() > 1;
    const bool animating = gliding || breathing || tailing || m.fx.busy() || m.ask.has_value() || m.ask_t > 0.0f || m.time - m.copy_flash_at < 0.5 || m.tab_motion.animating() ||
                           m.strip_t != (strip_goal ? 1.0f : 0.0f) || m.switch_t < 1.0f || m.drag_tab >= 0 || m.show_numbers || m.scroll_d != m.scroll_goal || m.out_shift != 0.0f ||
                           (m.bell_at >= 0.0 && m.time - m.bell_at < 0.45) ||
                           (m.cfg.blink_style == BlinkStyle::fade && m.cur_a != (m.cursor_on || !m.focused || !m.cfg.cursor_blink ? 1.0f : 0.0f)) ||
                           m.settings_t != (m.settings_open ? 1.0f : 0.0f) || (m.selecting && m.sel_edge != 0.0f) ||
                           m.mm_w != (m.mm_open ? kMinimapOpen : kMinimapRest) ||
                           m.peek_t != ((m.cfg.peek && m.mm_hover_y >= 0.0f) ? 1.0f : 0.0f) ||
                           m.tail_t != (tail_wanted(m) ? 1.0f : 0.0f) ||
                           m.sticky_t != (sticky_of(m) ? 1.0f : 0.0f) || (m.bell_at >= 0.0 && m.time - m.bell_at < 0.2) || m.time - m.resized_at < 0.9 ||
                           (m.toast && m.time - m.toast_at < 3.2) || (m.link.dropped && m.time - m.link.dropped_at < 0.3);
    if (const auto st = sticky_of(m); st && st->ran && !st->finished)
        s.push_back(Sub::every(std::chrono::milliseconds(1000), Msg{Clock{}}));
    if (animating) s.push_back(Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    return Sub::batch(std::move(s));
}

}  // namespace hand
