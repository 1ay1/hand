// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SettingsPanel implementation: window-event -> glyph::Input translation and
// the form layout.

#include "hand/settings_panel.hpp"
#include "hand/config/config.hpp"

#include <cstdio>
#include <variant>

namespace hand {

namespace {
std::string hex(toe::Rgb c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}
toe::Rgb unhex(const std::string &h) {
    if (h.size() != 7 || h[0] != '#') return toe::rgb(200, 200, 200);
    auto d = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    auto v = [&](int i) { return std::uint8_t(d(h[i]) * 16 + d(h[i + 1])); };
    return toe::rgb(v(1), v(3), v(5));
}

// Decode the FIRST UTF-8 scalar of a byte string (settings text is one
// codepoint per key event).
char32_t decode_first_utf8(std::string_view s) {
    if (s.empty()) return 0;
    const unsigned char b0 = (unsigned char)s[0];
    if (b0 < 0x80) return b0;
    auto cont = [&](std::size_t k) { return k < s.size() && ((unsigned char)s[k] & 0xC0) == 0x80; };
    if ((b0 & 0xE0) == 0xC0 && cont(1))
        return ((b0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2))
        return ((b0 & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3))
        return ((b0 & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
               (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
    return 0xFFFD;
}
} // namespace

Settings Settings::from(const HandConfig &c) {
    Settings s;
    s.font_size = c.font.size;
    s.ligatures = c.font.ligatures;
    s.cursor_style = static_cast<int>(c.cursor.shape);
    s.font_family = c.font.family;
    s.fg = hex(c.colors.foreground);
    s.bg = hex(c.colors.background);
    s.scrollback = c.scroll.scrollback_lines;
    s.blink_cursor = c.cursor.blink;
    s.animate_cursor = c.cursor.animate;
    return s;
}

void Settings::into(HandConfig &c) const {
    c.font.size = font_size;
    c.font.ligatures = ligatures;
    c.cursor.shape = static_cast<CursorShape>(cursor_style);
    c.font.family = font_family;
    c.colors.foreground = unhex(fg);
    c.colors.background = unhex(bg);
    c.scroll.scrollback_lines = scrollback;
    c.cursor.blink = blink_cursor;
    c.cursor.animate = animate_cursor;
}

glyph::Input SettingsPanel::translate(const toe::win::Event &ev, bool &consumed) {
    using namespace toe;
    glyph::Input in{};
    consumed = false;

    if (const auto *kp = std::get_if<win::KeyPressed>(&ev)) {
        // Only act on the initial press or an auto-repeat, NEVER on release.
        // Backends emit KeyPressed for both press AND release (the Kitty
        // protocol needs the release form; the terminal encoder gates it). The
        // panel has no such gate, so without this check every keystroke would
        // fire twice — once on press, once on release.
        const KeyEvent &k = kp->key;
        if (k.kind == KeyEvent::Kind::release) return in; // consumed=false
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
            // treat as a typed character (decoded as a full UTF-8 codepoint).
            if (txt->utf8 == " ") { in.key = glyph::Key::Space; }
            else if (!txt->utf8.empty() && (unsigned char)txt->utf8[0] >= 0x20) {
                in.key = glyph::Key::Char;
                in.ch = decode_first_utf8(txt->utf8);
            } else consumed = false;
        } else consumed = false;
    } else if (const auto *te = std::get_if<win::TextEntered>(&ev)) {
        if (!te->utf8.empty() && (unsigned char)te->utf8[0] >= 0x20) {
            in.key = te->utf8 == " " ? glyph::Key::Space : glyph::Key::Char;
            in.ch = decode_first_utf8(te->utf8);
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
    if (in.key == glyph::Key::Escape) { active_ = false; queue_.clear(); return true; }
    // Queue it — several events may arrive between frames (fast typing, held
    // arrows); render() drains one per frame so nothing is dropped.
    queue_.push_back(in);
    return true;
}

void SettingsPanel::render(glyph::Buffer &buf, bool &changed, bool &save) {
    changed = false;
    save = false;
    want_save_ = false;

    // Drain ONE queued input this frame (IMGUI processes one event per pass).
    glyph::Input in{};
    if (!queue_.empty()) { in = queue_.front(); queue_.pop_front(); }

    glyph::Ctx ui(buf, in, &focus_);

    ui.begin_panel("hand · settings", 62, 28);

    ui.heading("Appearance");
    changed |= ui.slider_int("Font size", &s_.font_size, 6, 48);
    changed |= ui.toggle("Ligatures", &s_.ligatures);
    changed |= ui.select("Cursor", &s_.cursor_style, {"block", "bar", "underline"});
    changed |= ui.toggle("Blink cursor", &s_.blink_cursor);
    changed |= ui.toggle("Animate cursor", &s_.animate_cursor);
    // Font family: a real dropdown listing the installed monospace fonts.
    if (ui.dropdown("Font", &font_index_, fonts_, &dd_open_, &dd_sel_, &dd_top_, /*max_visible=*/6)) {
        if (font_index_ >= 0 && font_index_ < static_cast<int>(fonts_.size()))
            s_.font_family = fonts_[static_cast<std::size_t>(font_index_)];
        changed = true;
    }

    ui.heading("Colors");
    changed |= ui.color("Foreground", &s_.fg);
    changed |= ui.color("Background", &s_.bg);

    ui.heading("Behavior");
    changed |= ui.slider_int("Scrollback", &s_.scrollback, 0, 100000, 1000);

    ui.gap();
    // Save button label reflects state: unsaved edits, or a brief confirmation.
    const char *label = saved_flash_ ? "✓ Saved" : (dirty_ ? "Save changes *" : "Save to config");
    const bool hit = ui.button(label);
    saved_flash_ = false;
    if (hit || force_save_) want_save_ = true;
    force_save_ = false;

    ui.end_panel();

    if (changed) dirty_ = true;
    if (want_save_) {
        s_.into(cfg_);
        if (!save_path_.empty() && save_hand_config(cfg_, save_path_)) {
            dirty_ = false;
            saved_flash_ = true; // show "✓ Saved" on the next frame
        }
    }
    save = want_save_;
    // If more input is queued, keep repainting so it drains quickly.
    if (!queue_.empty()) { /* the overlay force-repaints every frame anyway */ }
}

// --- process-wide settings source ------------------------------------------
namespace {
HandConfig g_settings_cfg{};
std::string g_settings_path;
} // namespace

void set_settings_source(const HandConfig &cfg, std::string path) {
    g_settings_cfg = cfg;
    g_settings_path = std::move(path);
}
const HandConfig &settings_source_config() noexcept { return g_settings_cfg; }
const std::string &settings_source_path() noexcept { return g_settings_path; }

} // namespace hand
