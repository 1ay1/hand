// hand::App — the keyboard into messages: chords, text, the input method.
//
// The order of the checks is the order of who owns the keyboard: the settings
// panel, the host manager, the picker, the file pane, the dialog, copy mode,
// hints, search -- and only then the shell. A key that a layer above takes
// never reaches the one below; one it does not want falls through.

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
        if (auto op = ssh_manager_key(m.ssh.manager.ui, picker_hosts(m), unix_now(m), k.key, md, &m.ssh.manager.files)) return Manager{std::move(*op)};
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
        if (auto op = ssh_manager_text(m.ssh.manager.ui, picker_hosts(m), unix_now(m), t.text)) return Manager{std::move(*op)};
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

}  // namespace hand
