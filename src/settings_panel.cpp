// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SettingsPanel implementation: window-event -> glyph::Input translation and
// the form layout.

#include "hand/settings_panel.hpp"
#include "hand/platform/shells.hpp"
#include "hand/config/config.hpp"
#include "hand/theme/themes.hpp"
#include "hand/theme/user_themes.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <variant>

namespace hand {

namespace {
std::string hex(toe::Rgb c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}
// Parse "#rrggbb" into an Rgb, or std::nullopt if malformed. Delegates to the
// canonical, fully-validating HexColor::parse so an invalid digit can never be
// silently swallowed as 0 (the old hand-rolled decoder did exactly that, then
// wrote the corrupted colour back to disk on the next save).
std::optional<toe::Rgb> try_unhex(const std::string &h) {
    if (auto c = HexColor::parse(h)) return c->rgb();
    return std::nullopt;
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
    s.animate_trail_len = c.cursor.animate_trail_len;
    // Colors
    s.fg = hex(c.colors.foreground);
    s.bg = hex(c.colors.background);
    s.cursor_color = hex(c.colors.cursor);
    s.selection = hex(c.colors.selection_bg);
    s.selection_invert = c.colors.selection_invert;
    s.search_match = hex(c.colors.search_match);
    s.search_current = hex(c.colors.search_current);
    s.selection_contrast = static_cast<int>(c.colors.selection_contrast * 10.0f + 0.5f);
    s.selection_radius = static_cast<int>(c.colors.selection_radius * 100.0f + 0.5f);
    s.palette.clear();
    for (toe::Rgb col : c.colors.palette) s.palette.push_back(hex(col));
    // Chrome (rail + flyout)
    s.rail = c.chrome.rail;
    s.rail_width = c.chrome.rail_width;
    s.rail_ok = hex(c.chrome.rail_ok);
    s.rail_failed = hex(c.chrome.rail_failed);
    s.rail_running = hex(c.chrome.rail_running);
    s.rail_alpha = c.chrome.rail_alpha;
    s.flyout = c.chrome.flyout;
    s.flyout_rows = c.chrome.flyout_rows;
    s.flyout_accent = hex(c.chrome.flyout_accent);
    s.tab_position = c.tabs.position == "bottom" ? 1
                   : c.tabs.position == "left"   ? 2
                   : c.tabs.position == "right"  ? 3 : 0;
    s.tab_side_width = c.tabs.side_width;
    s.tab_controls = c.tabs.show_window_controls;
    s.tab_plus = c.tabs.show_new_tab_button;
    // Scroll
    s.scrollback = c.scroll.scrollback_lines;
    s.scroll_mult = c.scroll.wheel_lines;
    s.scroll_on_output = c.scroll.scroll_on_output;
    s.scroll_on_keystroke = c.scroll.scroll_on_keystroke;
    s.autoscroll_max = static_cast<int>(c.scroll.autoscroll_max + 0.5f);
    s.font_zoom_step = c.scroll.font_zoom_step;
    s.pointer_shapes = c.scroll.pointer_shapes;
    // Behavior
    s.audible_bell = c.behavior.audible_bell;
    s.visual_bell = c.behavior.visual_bell;
    s.copy_on_select = c.behavior.copy_on_select;
    s.confirm_close = c.behavior.confirm_close;
    s.word_separators = c.behavior.word_separators;
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
    c.cursor.animate_trail_len = animate_trail_len;
    // Colors — preserve the existing config colour when a field holds an
    // invalid/empty hex string, rather than stamping in a placeholder that
    // would then be persisted on the next save.
    if (auto v = try_unhex(fg)) c.colors.foreground = *v;
    if (auto v = try_unhex(bg)) c.colors.background = *v;
    if (auto v = try_unhex(cursor_color)) c.colors.cursor = *v;
    if (auto v = try_unhex(selection)) c.colors.selection_bg = *v;
    c.colors.selection_invert = selection_invert;
    if (auto v = try_unhex(search_match)) c.colors.search_match = *v;
    if (auto v = try_unhex(search_current)) c.colors.search_current = *v;
    c.colors.selection_contrast = static_cast<float>(selection_contrast) / 10.0f;
    c.colors.selection_radius = static_cast<float>(selection_radius) / 100.0f;
    c.colors.palette.clear();
    for (const auto &h : palette)
        if (auto v = try_unhex(h)) c.colors.palette.push_back(*v);
    // Chrome (rail + flyout)
    c.chrome.rail = rail;
    c.chrome.rail_width = rail_width;
    if (auto v = try_unhex(rail_ok)) c.chrome.rail_ok = *v;
    if (auto v = try_unhex(rail_failed)) c.chrome.rail_failed = *v;
    if (auto v = try_unhex(rail_running)) c.chrome.rail_running = *v;
    c.chrome.rail_alpha = rail_alpha;
    c.chrome.flyout = flyout;
    c.chrome.flyout_rows = flyout_rows;
    if (auto v = try_unhex(flyout_accent)) c.chrome.flyout_accent = *v;
    c.tabs.position = tab_position == 1 ? "bottom"
                    : tab_position == 2 ? "left"
                    : tab_position == 3 ? "right" : "top";
    c.tabs.side_width = tab_side_width;
    c.tabs.show_window_controls = tab_controls;
    c.tabs.show_new_tab_button = tab_plus;
    // Scroll
    c.scroll.scrollback_lines = scrollback;
    c.scroll.wheel_lines = scroll_mult;
    c.scroll.scroll_on_output = scroll_on_output;
    c.scroll.scroll_on_keystroke = scroll_on_keystroke;
    c.scroll.autoscroll_max = static_cast<float>(autoscroll_max);
    c.scroll.font_zoom_step = font_zoom_step;
    c.scroll.pointer_shapes = pointer_shapes;
    // Behavior
    c.behavior.audible_bell = audible_bell;
    c.behavior.visual_bell = visual_bell;
    c.behavior.copy_on_select = copy_on_select;
    c.behavior.confirm_close = confirm_close;
    c.behavior.word_separators = word_separators;
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
    // all_themes() already returns user/custom themes FIRST, then the built-ins.
    // Prefix the custom ones with a star so they're visually obvious at the top
    // of the picker ("★ My Theme") — the id (user:<stem>) is what we persist.
    for (const auto &t : all_themes()) {
        const bool custom = std::string_view(t.id).substr(0, 5) == "user:";
        theme_ids_.emplace_back(t.id);
        theme_labels_.emplace_back(custom ? "★ " + std::string(t.label)
                                          : std::string(t.label));
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
        "Theme", "Font", "Cursor", "Chrome", "Scroll", "Behavior", "Window", "Advanced"};
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

    // The panel auto-fits its CONTENT. This used to be a hand-maintained table
    // of per-section row counts, which silently drifted as sections gained and
    // lost widgets — the Theme tab claimed 11 rows while drawing 8, leaving a
    // band of dead space under every short section and making the card look
    // like a mostly-empty box. Deriving it from the widgets themselves means it
    // can never disagree with what is drawn.
    // Rows each section actually DRAWS. This must match the widget calls in
    // layout below; when it drifts the card either clips its last control or
    // floats in a band of empty space. Theme: dropdown + note + 4 colours +
    // "Save as" + button + status line.
    static const int kSectionRows[] = {14, 8, 7, 15, 7, 5, 6, 3};
    // STATIC PANEL: size the card once to the TALLEST section so switching
    // sections never resizes it, and an open dropdown renders WITHIN this fixed
    // area (the dropdown scrolls its list internally via dd_top_) instead of
    // growing the panel. The result: the panel frame stays put; only the
    // dropdown list scrolls.
    int max_rows = 0;
    for (int rr : kSectionRows) max_rows = std::max(max_rows, rr);
    // header band(1)+rule(1) + tab row(1)+rule(1) + content + footer rule(1)+
    // text(1) + frame(2).
    int panel_h = 2 + 2 + max_rows + 2 + 2;
    panel_h = std::clamp(panel_h, 12, buf.height() - 2);

    // Width: a left SIDEBAR (widest section label + accent bar + margins) plus
    // the content pane. Sidebar ~= max label + 4; content needs ~46 for the
    // widest control (colour swatch rows, sliders). Clamp to the screen.
    int side_w = 0;
    for (const auto &t : kSections)
        side_w = std::max(side_w, static_cast<int>(t.size()));
    side_w += 4;
    // Content pane must fit the WIDEST control: the "Save current colours as a
    // theme" button is ~35 cells incl. padding. Give the pane 52 so nothing
    // overflows into the sidebar/divider.
    const int panel_w = std::clamp(side_w + 3 + 52, 40, buf.width() - 2);

    // Overlay translucency comes from the config (Window tab): the scrim dims
    // the terminal lightly, the panel card stays near-opaque and readable.
    const auto a8 = [](float f) {
        return static_cast<std::uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    ui.begin_panel("hand · settings", panel_w, panel_h, a8(s_.overlay_scrim_opacity),
                   a8(s_.overlay_panel_opacity));

    // Section SIDEBAR down the left (focus row 0). Up/Down (or ←/→) switch
    // sections when the sidebar owns focus; each section shows only its own
    // options in the pane on the right.
    if (ui.sidebar(kSections, &section_)) {
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
        changed |= ui.toggle("Invert selection", &s_.selection_invert);
        changed |= ui.slider_int("Sel contrast x10", &s_.selection_contrast, 10, 70, 1);
        changed |= ui.slider_int("Sel corner %", &s_.selection_radius, 0, 50, 1);
        changed |= ui.color("Search match", &s_.search_match);
        changed |= ui.color("Search current", &s_.search_current);
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
        changed |= ui.slider_int("Trail length", &s_.animate_trail_len, 0, 6, 1);
        break;
    case 3: // Chrome (command minimap rail + hover flyout)
        changed |= ui.toggle("Command rail", &s_.rail);
        changed |= ui.slider_int("Rail width px", &s_.rail_width, 3, 24, 1);
        changed |= ui.slider_int("Rail opacity", &s_.rail_alpha, 40, 255, 5);
        changed |= ui.color("Rail ok", &s_.rail_ok);
        changed |= ui.color("Rail failed", &s_.rail_failed);
        changed |= ui.color("Rail running", &s_.rail_running);
        changed |= ui.toggle("Hover flyout", &s_.flyout);
        changed |= ui.slider_int("Flyout rows", &s_.flyout_rows, 4, 24, 1);
        changed |= ui.color("Flyout accent", &s_.flyout_accent);
        ui.note("Tab bar:");
        changed |= ui.select("Tab position", &s_.tab_position,
                             {"top", "bottom", "left", "right"});
        changed |= ui.slider_int("Side width", &s_.tab_side_width, 8, 40, 1);
        changed |= ui.toggle("Window buttons", &s_.tab_controls);
        changed |= ui.toggle("New-tab button", &s_.tab_plus);
        break;
    case 4: // Scroll
        changed |= ui.slider_int("Scrollback", &s_.scrollback, 0, 100000, 1000);
        changed |= ui.slider_int("Wheel lines", &s_.scroll_mult, 1, 20, 1);
        changed |= ui.toggle("Scroll on output", &s_.scroll_on_output);
        changed |= ui.toggle("Scroll on keystroke", &s_.scroll_on_keystroke);
        changed |= ui.slider_int("Autoscroll max", &s_.autoscroll_max, 5, 120, 5);
        changed |= ui.slider_int("Font zoom step", &s_.font_zoom_step, 1, 6, 1);
        changed |= ui.toggle("Pointer shapes", &s_.pointer_shapes);
        break;
    case 5: // Behavior
        changed |= ui.toggle("Audible bell", &s_.audible_bell);
        changed |= ui.toggle("Visual bell", &s_.visual_bell);
        changed |= ui.toggle("Copy on select", &s_.copy_on_select);
        changed |= ui.toggle("Confirm on close", &s_.confirm_close);
        changed |= ui.text_input("Word separators", &s_.word_separators);
        break;
    case 6: { // Window
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
    case 7: { // Advanced
        ui.note("Shell / TERM take effect on the NEXT window.");
        // Shell picker: "$SHELL (default)" + every installed shell (/etc/shells).
        static const std::vector<std::string> kShellPaths = installed_shells();
        static const std::vector<std::string> kShellOpts = [] {
            std::vector<std::string> o{"$SHELL (default)"};
            for (const auto &p : kShellPaths) o.push_back(shell_name(p));
            return o;
        }();
        int si = 0;
        for (std::size_t i = 0; i < kShellPaths.size(); ++i)
            if (kShellPaths[i] == s_.shell) { si = static_cast<int>(i) + 1; break; }
        if (ui.select("Shell", &si, kShellOpts)) {
            s_.shell = (si <= 0 || static_cast<std::size_t>(si - 1) >= kShellPaths.size())
                           ? std::string{}
                           : kShellPaths[static_cast<std::size_t>(si - 1)];
            changed = true;
        }
        changed |= ui.text_input("TERM", &s_.term_env);
        break;
    }
    default: break;
    }

    if (dd_open_ >= 0)
        ui.end_panel("type to filter   \u2191\u2193 preview   \u21b5 keep   esc close menu");
    else
        ui.end_panel("\u2191\u2193 move   \u2190\u2192/space edit   tab: switch section   applies live   esc close");

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
