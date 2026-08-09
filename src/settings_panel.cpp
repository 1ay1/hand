// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SettingsPanel implementation: window-event -> glyph::Input translation and
// the form layout.

#include "hand/settings_panel.hpp"
#include "hand/config/config.hpp"
#include "hand/theme/themes.hpp"
#include "hand/theme/user_themes.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
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
    s.theme = c.theme_name;
    // Font
    s.font_family = c.font.family;
    s.font_file = c.font.file;
    s.font_fallback = c.font.fallback;
    s.font_bold = c.font.file_bold;
    s.font_italic = c.font.file_italic;
    s.font_bold_italic = c.font.file_bold_italic;
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
    s.palette.clear();
    for (toe::Rgb col : c.colors.palette) s.palette.push_back(hex(col));
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
    s.title = c.window.title;
    s.padding = c.window.padding;
    s.opacity = c.window.opacity;
    s.overlay_panel_opacity = c.window.overlay_panel_opacity;
    s.overlay_scrim_opacity = c.window.overlay_scrim_opacity;
    s.decorations = c.window.decorations;
    // Advanced
    s.shell = c.behavior.shell;
    s.term_env = c.behavior.term;
    return s;
}

void Settings::into(HandConfig &c) const {
    c.theme_name = theme;
    // Font
    c.font.family = font_family;
    c.font.file = font_file;
    c.font.fallback = font_fallback;
    c.font.file_bold = font_bold;
    c.font.file_italic = font_italic;
    c.font.file_bold_italic = font_bold_italic;
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
    c.colors.palette.clear();
    for (const auto &h : palette) c.colors.palette.push_back(unhex(h));
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
    c.window.title = title;
    c.window.padding = padding;
    c.window.opacity = opacity;
    c.window.overlay_panel_opacity = overlay_panel_opacity;
    c.window.overlay_scrim_opacity = overlay_scrim_opacity;
    c.window.decorations = decorations;
    // Advanced
    c.behavior.shell = shell;
    c.behavior.term = term_env;
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
    // Escape closes the pane — UNLESS a dropdown is open, in which case it must
    // close the dropdown first (queue it so render() feeds it to the widget).
    if (in.key == glyph::Key::Escape && dd_open_ < 0) {
        active_ = false;
        queue_.clear();
        return true;
    }
    // Queue it — several events may arrive between frames (fast typing, held
    // arrows); render() drains one per frame so nothing is dropped.
    queue_.push_back(in);
    return true;
}

void SettingsPanel::ensure_themes() {
    if (!theme_ids_.empty()) return;
    for (const auto &t : all_themes()) {
        theme_ids_.emplace_back(t.id);
        theme_labels_.emplace_back(t.label);
    }
    sync_theme_index();
}

glyph::Theme SettingsPanel::ui_theme() const {
    if (const NamedTheme *t = find_theme(s_.theme)) return to_glyph_theme(*t);
    if (const NamedTheme *d = find_theme(kDefaultThemeId)) return to_glyph_theme(*d);
    return glyph::Theme{};
}

std::string SettingsPanel::export_current_theme() {
    auto unhex = [](const std::string &h) -> toe::Rgb {
        if (auto c = HexColor::parse(h)) return c->rgb();
        return toe::rgb(0, 0, 0);
    };
    ThemeColors tc;
    tc.bg = unhex(s_.bg);
    tc.fg = unhex(s_.fg);
    tc.cursor = unhex(s_.cursor_color);
    tc.selection = unhex(s_.selection);
    // Dark/light inferred from background brightness.
    tc.dark = (0.2126f * tc.bg.r + 0.7152f * tc.bg.g + 0.0722f * tc.bg.b) < 128.0f;
    // Palette: fall back to the active theme's if the user hasn't set 16.
    std::array<toe::Rgb, 16> ansi{};
    if (s_.palette.size() >= 16) {
        for (int i = 0; i < 16; ++i) ansi[static_cast<std::size_t>(i)] = unhex(s_.palette[static_cast<std::size_t>(i)]);
    } else if (const NamedTheme *base = find_theme(s_.theme)) {
        ansi = base->ansi;
    }
    tc.ansi = ansi;
    tc.accent = ansi[12]; // bright blue as the brand accent

    const std::string path = save_user_theme(export_name_, tc);
    if (!path.empty()) {
        // Refresh the picker so the new theme appears immediately, and select it.
        theme_ids_.clear();
        theme_labels_.clear();
        ensure_themes();
        // The new theme's id is "user:<file-stem>"; derive it from the path.
        std::string stem = std::filesystem::path(path).stem().string();
        const std::string id = "user:" + stem;
        if (find_theme(id)) { s_.theme = id; sync_theme_index(); }
    }
    return path;
}

void SettingsPanel::render(glyph::Buffer &buf, bool &changed) {
    changed = false;

    // Drain ONE queued input this frame (IMGUI processes one event per pass).
    glyph::Input in{};
    if (!queue_.empty()) { in = queue_.front(); queue_.pop_front(); }

    ensure_themes();

    static const std::vector<std::string> kSections = {
        "Theme", "Font", "Cursor", "Scroll", "Behavior", "Window", "Advanced"};
    const int nsec = static_cast<int>(kSections.size());

    // Tab / Shift-Tab switch SECTIONS (unless a dropdown is capturing input).
    // Handled BEFORE the Ctx sees the input so it's fully consumed here; ←/→ on
    // the tab row still works, and ↑↓ move focus between the section's fields.
    if (dd_open_ < 0 && (in.key == glyph::Key::Tab || in.key == glyph::Key::ShiftTab)) {
        section_ = (section_ + (in.key == glyph::Key::Tab ? 1 : nsec - 1)) % nsec;
        focus_ = 1;
        in = glyph::Input{}; // swallow: don't let it also move focus
    }

    glyph::Ctx ui(buf, in, &focus_, ui_theme());

    // Rows each section shows (drives the auto-fitted panel height so there's no
    // vast empty space under a short tab). Index matches kSections.
    static const int kSectionRows[] = {11, 7, 6, 4, 4, 6, 3};
    const int rows = kSectionRows[std::clamp(section_, 0, nsec - 1)];
    // header band(1)+rule(1)+gap(1) + tab row(1)+rule(1) + content + gap +
    // footer rule(1)+text(1) + frame(2). A roomy but tight card.
    int panel_h = 3 + 2 + rows + 1 + 2 + 2 + 1;
    // A dropdown open in this section needs extra room for the popup below it.
    if (dd_open_ >= 0) panel_h += 9;
    panel_h = std::clamp(panel_h, 12, buf.height() - 2);

    // Width must fit the WHOLE tab strip so no tab is clipped off. Each tab
    // occupies (label + 2 caps/pad + 1 gap) cells (see Ctx::tab_bar); add the
    // frame + inner margins. Clamp to the screen; if the terminal is too narrow
    // the tab_bar will scroll, but on any normal window all 8 tabs show.
    int tabs_w = 1; // leading margin
    for (const auto &t : kSections)
        tabs_w += static_cast<int>(t.size()) + 3;
    const int panel_w = std::clamp(std::max(66, tabs_w + 5), 40, buf.width() - 2);

    // Overlay translucency comes from the config (Window tab): the scrim dims
    // the terminal lightly, the panel card stays near-opaque and readable.
    const auto a8 = [](float f) {
        return static_cast<std::uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    ui.begin_panel("hand · settings", panel_w, panel_h, a8(s_.overlay_scrim_opacity),
                   a8(s_.overlay_panel_opacity));

    // Section tabs (focus row 0). ←/→ switch when the tab row is focused; each
    // section shows only its own options so the panel stays scannable.
    if (ui.tab_bar(kSections, &section_)) {
        // Moved to another section: park focus on its first field and close any
        // open dropdown so state doesn't leak between tabs.
        focus_ = 1;
        dd_open_ = -1;
    }

    switch (section_) {
    case 0: { // Theme — pick a theme, then tweak its colours, then save your own.
        sync_theme_index();
        if (ui.dropdown("Theme", &theme_index_, theme_labels_, &dd_open_,
                        &theme_dd_sel_, &theme_dd_top_, 10, &theme_filter_)) {
            if (theme_index_ >= 0 && theme_index_ < static_cast<int>(theme_ids_.size())) {
                // Refill the colour fields from the chosen theme so the swatches
                // below mirror it and the host applies the full palette live.
                s_.theme = theme_ids_[static_cast<std::size_t>(theme_index_)];
                if (const NamedTheme *t = find_theme(s_.theme)) {
                    auto hex = [](toe::Rgb c) {
                        char b[8];
                        std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
                        return std::string(b);
                    };
                    s_.fg = hex(t->fg);
                    s_.bg = hex(t->bg);
                    s_.cursor_color = hex(t->cursor);
                    s_.selection = hex(t->selection);
                    s_.palette.clear();
                    for (toe::Rgb c : t->ansi) s_.palette.push_back(hex(c));
                }
                changed = true;
            }
        }
        // The theme's key colours, editable in place — tweaks layer over the
        // theme (they persist as `colors { }` overrides). Editing a swatch marks
        // the config custom without leaving the theme.
        ui.note("tweak the theme's colours — or save your own below");
        changed |= ui.color("Foreground", &s_.fg);
        changed |= ui.color("Background", &s_.bg);
        changed |= ui.color("Cursor", &s_.cursor_color);
        changed |= ui.color("Selection", &s_.selection);
        // Author a THEME from the current colours: name it + save. It lands in
        // ~/.config/hand/themes/<slug>.vibe and instantly joins the picker above
        // (and is shareable — just send the file).
        ui.text_input("Save as", &export_name_);
        if (ui.button("⬇ Save current colours as a theme")) {
            const std::string path = export_current_theme();
            export_status_ = path.empty() ? "export failed (no config dir?)"
                                          : "saved → " + path;
        }
        if (!export_status_.empty()) ui.note(export_status_);
        break;
    }
    case 1: // Font
        if (ui.dropdown("Family", &font_index_, fonts_, &dd_open_, &dd_sel_, &dd_top_, 6,
                        &font_filter_)) {
            if (font_index_ >= 0 && font_index_ < static_cast<int>(fonts_.size()))
                s_.font_family = fonts_[static_cast<std::size_t>(font_index_)];
            changed = true;
        }
        changed |= ui.slider_int("Size", &s_.font_size, 6, 48);
        changed |= ui.toggle("Ligatures", &s_.ligatures);
        changed |= ui.text_input("Fallback (CJK/emoji)", &s_.font_fallback);
        changed |= ui.text_input("File override", &s_.font_file);
        changed |= ui.text_input("Bold face file", &s_.font_bold);
        changed |= ui.text_input("Italic face file", &s_.font_italic);
        changed |= ui.text_input("Bold-italic file", &s_.font_bold_italic);
        break;
    case 2: // Cursor
        changed |= ui.select("Shape", &s_.cursor_style, {"block", "bar", "underline"});
        changed |= ui.toggle("Blink", &s_.blink_cursor);
        changed |= ui.slider_int("Blink rate ms", &s_.blink_ms, 100, 2000, 10);
        changed |= ui.toggle("Animate (glide)", &s_.animate_cursor);
        changed |= ui.slider_int("Glide ms", &s_.animate_ms, 10, 300, 5);
        changed |= ui.toggle("Comet trail", &s_.animate_trail);
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
        changed |= ui.text_input("Title", &s_.title);
        changed |= ui.slider_int("Padding", &s_.padding, 0, 64, 1);
        int op = static_cast<int>(s_.opacity * 100.0f + 0.5f);
        if (ui.slider_int("Opacity %", &op, 20, 100, 1)) {
            s_.opacity = static_cast<float>(op) / 100.0f;
            changed = true;
        }
        int pop = static_cast<int>(s_.overlay_panel_opacity * 100.0f + 0.5f);
        if (ui.slider_int("Overlay panel %", &pop, 40, 100, 1)) {
            s_.overlay_panel_opacity = static_cast<float>(pop) / 100.0f;
            changed = true;
        }
        int sop = static_cast<int>(s_.overlay_scrim_opacity * 100.0f + 0.5f);
        if (ui.slider_int("Overlay dim %", &sop, 0, 80, 1)) {
            s_.overlay_scrim_opacity = static_cast<float>(sop) / 100.0f;
            changed = true;
        }
        changed |= ui.toggle("Decorations", &s_.decorations);
        break;
    }
    case 6: // Advanced
        ui.note("Shell / TERM take effect on the NEXT window.");
        changed |= ui.text_input("Shell (empty = $SHELL)", &s_.shell);
        changed |= ui.text_input("TERM", &s_.term_env);
        break;
    default: break;
    }

    if (dd_open_ >= 0)
        ui.end_panel("type to filter   \u2191\u2193 preview   \u21b5 keep   esc close menu");
    else
        ui.end_panel("tab section   \u2191\u2193 move   \u2190\u2192/space edit   applies live   esc close");

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
