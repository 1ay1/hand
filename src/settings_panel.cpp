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
    // Font
    s.font_family = c.font.family;
    s.font_file = c.font.file;
    s.font_fallback = c.font.fallback;
    s.font_size = c.font.size;
    s.ligatures = c.font.ligatures;
    // Cursor
    s.cursor_style = static_cast<int>(c.cursor.shape);
    s.blink_cursor = c.cursor.blink;
    s.blink_ms = c.cursor.blink_ms;
    s.animate_cursor = c.cursor.animate;
    s.animate_ms = c.cursor.animate_ms;
    s.animate_trail = c.cursor.animate_trail;
    // Colors
    s.fg = hex(c.colors.foreground);
    s.bg = hex(c.colors.background);
    s.cursor_color = hex(c.colors.cursor);
    s.selection = hex(c.colors.selection_bg);
    // Scroll
    s.scrollback = c.scroll.scrollback_lines;
    s.scroll_mult = c.scroll.wheel_lines;
    s.scroll_on_output = c.scroll.scroll_on_output;
    s.scroll_on_keystroke = c.scroll.scroll_on_keystroke;
    // Behavior
    s.audible_bell = c.behavior.audible_bell;
    s.visual_bell = c.behavior.visual_bell;
    s.copy_on_select = c.behavior.copy_on_select;
    s.confirm_close = c.behavior.confirm_close;
    // Window
    s.padding = c.window.padding;
    s.opacity = c.window.opacity;
    s.decorations = c.window.decorations;
    return s;
}

void Settings::into(HandConfig &c) const {
    // Font
    c.font.family = font_family;
    c.font.file = font_file;
    c.font.fallback = font_fallback;
    c.font.size = font_size;
    c.font.ligatures = ligatures;
    // Cursor
    c.cursor.shape = static_cast<CursorShape>(cursor_style);
    c.cursor.blink = blink_cursor;
    c.cursor.blink_ms = blink_ms;
    c.cursor.animate = animate_cursor;
    c.cursor.animate_ms = animate_ms;
    c.cursor.animate_trail = animate_trail;
    // Colors
    c.colors.foreground = unhex(fg);
    c.colors.background = unhex(bg);
    c.colors.cursor = unhex(cursor_color);
    c.colors.selection_bg = unhex(selection);
    // Scroll
    c.scroll.scrollback_lines = scrollback;
    c.scroll.wheel_lines = scroll_mult;
    c.scroll.scroll_on_output = scroll_on_output;
    c.scroll.scroll_on_keystroke = scroll_on_keystroke;
    // Behavior
    c.behavior.audible_bell = audible_bell;
    c.behavior.visual_bell = visual_bell;
    c.behavior.copy_on_select = copy_on_select;
    c.behavior.confirm_close = confirm_close;
    // Window
    c.window.padding = padding;
    c.window.opacity = opacity;
    c.window.decorations = decorations;
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

void SettingsPanel::render(glyph::Buffer &buf, bool &changed) {
    changed = false;

    // Drain ONE queued input this frame (IMGUI processes one event per pass).
    glyph::Input in{};
    if (!queue_.empty()) { in = queue_.front(); queue_.pop_front(); }

    glyph::Ctx ui(buf, in, &focus_);

    // Wider panel to fit the tab bar + two-column rows comfortably.
    ui.begin_panel("hand · settings", 66, 26);

    // Section tabs (focus row 0). Left/Right switch; each section shows only its
    // own options, so the panel stays scannable instead of a 30-row wall.
    static const std::vector<std::string> kSections = {
        "Font", "Cursor", "Colors", "Scroll", "Behavior", "Window"};
    if (ui.tab_bar(kSections, &section_)) {
        // Moved to another section: park focus on its first field and close any
        // open dropdown so state doesn't leak between tabs.
        focus_ = 1;
        dd_open_ = -1;
    }

    switch (section_) {
    case 0: // Font
        if (ui.dropdown("Family", &font_index_, fonts_, &dd_open_, &dd_sel_, &dd_top_, 6)) {
            if (font_index_ >= 0 && font_index_ < static_cast<int>(fonts_.size()))
                s_.font_family = fonts_[static_cast<std::size_t>(font_index_)];
            changed = true;
        }
        changed |= ui.slider_int("Size", &s_.font_size, 6, 48);
        changed |= ui.toggle("Ligatures", &s_.ligatures);
        changed |= ui.text_input("Fallback", &s_.font_fallback);
        changed |= ui.text_input("File override", &s_.font_file);
        break;
    case 1: // Cursor
        changed |= ui.select("Shape", &s_.cursor_style, {"block", "bar", "underline"});
        changed |= ui.toggle("Blink", &s_.blink_cursor);
        changed |= ui.slider_int("Blink rate ms", &s_.blink_ms, 100, 2000, 10);
        changed |= ui.toggle("Animate (glide)", &s_.animate_cursor);
        changed |= ui.slider_int("Glide ms", &s_.animate_ms, 10, 300, 5);
        changed |= ui.toggle("Comet trail", &s_.animate_trail);
        break;
    case 2: // Colors
        changed |= ui.color("Foreground", &s_.fg);
        changed |= ui.color("Background", &s_.bg);
        changed |= ui.color("Cursor", &s_.cursor_color);
        changed |= ui.color("Selection", &s_.selection);
        break;
    case 3: // Scroll
        changed |= ui.slider_int("Scrollback", &s_.scrollback, 0, 100000, 1000);
        changed |= ui.slider_int("Wheel lines", &s_.scroll_mult, 1, 20, 1);
        changed |= ui.toggle("Scroll on output", &s_.scroll_on_output);
        changed |= ui.toggle("Scroll on keystroke", &s_.scroll_on_keystroke);
        break;
    case 4: // Behavior
        changed |= ui.toggle("Audible bell", &s_.audible_bell);
        changed |= ui.toggle("Visual bell", &s_.visual_bell);
        changed |= ui.toggle("Copy on select", &s_.copy_on_select);
        changed |= ui.toggle("Confirm on close", &s_.confirm_close);
        break;
    case 5: { // Window
        changed |= ui.slider_int("Padding", &s_.padding, 0, 64, 1);
        int op = static_cast<int>(s_.opacity * 100.0f + 0.5f);
        if (ui.slider_int("Opacity %", &op, 20, 100, 1)) {
            s_.opacity = static_cast<float>(op) / 100.0f;
            changed = true;
        }
        changed |= ui.toggle("Decorations", &s_.decorations);
        break;
    }
    default: break;
    }

    ui.end_panel("\u2190\u2192 section / edit   \u2191\u2193 move   changes apply + save live   esc close");

    // Live config: any edit is scheduled for persistence. We debounce so a
    // slider drag writes the file once it settles, not on every tick; close()
    // flushes unconditionally. The edit is ALREADY applied live by the host
    // (via `changed`), so the disk write is just durability.
    if (changed) { pending_save_ = true; edited_ms_ = now_ms(); }
    if (pending_save_ && now_ms() - edited_ms_ >= kSaveDebounceMs) flush_pending();
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
