// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SettingsPanel implementation: window-event -> glyph::Input translation and
// the form layout.

#include "hand/settings_panel.hpp"

#include <variant>

namespace hand {

glyph::Input SettingsPanel::translate(const toe::win::Event &ev, bool &consumed) {
    using namespace toe;
    glyph::Input in{};
    consumed = false;

    if (const auto *kp = std::get_if<win::KeyPressed>(&ev)) {
        // Only act on the initial press (not repeat/release) for navigation, but
        // allow repeats for text editing / sliders (feels natural to hold).
        const KeyEvent &k = kp->key;
        consumed = true;
        if (const auto *sk = std::get_if<SpecialKey>(&k.key)) {
            switch (*sk) {
            case SpecialKey::Up:        in.key = glyph::Key::Up; break;
            case SpecialKey::Down:      in.key = glyph::Key::Down; break;
            case SpecialKey::Left:      in.key = glyph::Key::Left; break;
            case SpecialKey::Right:     in.key = glyph::Key::Right; break;
            case SpecialKey::Tab:       in.key = k.mods.shift ? glyph::Key::ShiftTab : glyph::Key::Tab; break;
            case SpecialKey::Enter:
            case SpecialKey::KpEnter:   in.key = glyph::Key::Enter; break;
            case SpecialKey::Escape:    in.key = glyph::Key::Escape; break;
            case SpecialKey::Backspace: in.key = glyph::Key::Backspace; break;
            case SpecialKey::Delete:    in.key = glyph::Key::Delete; break;
            case SpecialKey::Home:      in.key = glyph::Key::Home; break;
            case SpecialKey::End:       in.key = glyph::Key::End; break;
            case SpecialKey::PageUp:    in.key = glyph::Key::PageUp; break;
            case SpecialKey::PageDown:  in.key = glyph::Key::PageDown; break;
            default: consumed = false; break;
            }
        } else if (const auto *txt = std::get_if<TextInput>(&k.key)) {
            // A control-key text (e.g. space arrives as " ") — map space, else
            // treat as a typed character for text fields.
            if (txt->utf8 == " ") { in.key = glyph::Key::Space; }
            else if (!txt->utf8.empty() && (unsigned char)txt->utf8[0] >= 0x20) {
                in.key = glyph::Key::Char;
                in.ch = (char32_t)(unsigned char)txt->utf8[0]; // ASCII fast path
            } else consumed = false;
        } else consumed = false;
    } else if (const auto *te = std::get_if<win::TextEntered>(&ev)) {
        if (!te->utf8.empty()) {
            in.key = te->utf8 == " " ? glyph::Key::Space : glyph::Key::Char;
            in.ch = (char32_t)(unsigned char)te->utf8[0];
            consumed = true;
        }
    }
    return in;
}

bool SettingsPanel::handle(const toe::win::Event &ev) {
    if (!active_) return false;
    bool consumed = false;
    const glyph::Input in = translate(ev, consumed);
    if (!consumed) return false;
    if (in.key == glyph::Key::Escape) { active_ = false; return true; }
    pending_ = in; // processed on the next render()
    return true;
}

void SettingsPanel::render(glyph::Buffer &buf, bool &changed, bool &save) {
    changed = false;
    save = false;
    want_save_ = false;
    glyph::Ctx ui(buf, pending_);

    ui.begin_panel("hand · settings", 60, 22);

    ui.heading("Appearance");
    changed |= ui.slider_int("Font size", &s_.font_size, 6, 48);
    changed |= ui.toggle("Ligatures", &s_.ligatures);
    changed |= ui.select("Cursor", &s_.cursor_style, {"block", "bar", "underline"});
    changed |= ui.toggle("Blink cursor", &s_.blink_cursor);
    changed |= ui.text_input("Font family", &s_.font_family);

    ui.heading("Colors");
    changed |= ui.color("Foreground", &s_.fg);
    changed |= ui.color("Background", &s_.bg);

    ui.heading("Behavior");
    changed |= ui.slider_int("Scrollback", &s_.scrollback, 0, 100000, 1000);

    ui.heading("");
    if (ui.button("Save to config")) want_save_ = true;

    ui.end_panel();

    save = want_save_;
    pending_ = glyph::Input{}; // consume
}

} // namespace hand
