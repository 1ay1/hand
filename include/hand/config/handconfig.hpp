// SPDX-License-Identifier: LGPL-2.0-or-later
//
// HandConfig — the complete, documented configuration schema for hand.
//
// Config best practices this follows:
//   * ONE source of truth. Every runtime option lives here with a sensible
//     default; nothing is configured by scattered magic numbers.
//   * Every option is ACTIONABLE. We only expose settings that actually change
//     behavior (font, colors, cursor, scrollback, padding, opacity, shell,
//     keybinds) — no dead knobs.
//   * Layered resolution: built-in defaults -> config file -> CLI. Missing or
//     malformed values keep the default (never a hard failure).
//   * Round-trips: the same schema loads AND saves (the settings panel edits
//     it and persists to the VIBE file).
//   * Grouped + documented: sections mirror the VIBE file's objects so the file
//     and the struct read the same.

#ifndef HAND_HANDCONFIG_HPP
#define HAND_HANDCONFIG_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "toe/core/types.hpp" // Rgb, rgb()
#include "toe/terminal.hpp"   // toe::Config (the engine subset)

#include "hand/theme/named_theme.hpp" // NamedTheme, find_theme, kDefaultThemeId

namespace hand {

// Cursor shape drawn when the app hasn't chosen one via DECSCUSR.
enum class CursorShape { Block, Bar, Underline };

// How the window opens.
struct WindowConfig {
    int width = 800;             // initial width in logical points
    int height = 500;            // initial height in logical points
    std::string title = "hand";  // window title (apps override via OSC 0/2)
    int padding = 0;             // inner padding in pixels around the grid
    float opacity = 1.0f;        // 0.0 (clear) .. 1.0 (opaque) window background
    bool decorations = true;     // native title bar
    // Settings/help OVERLAY translucency (0..1). The pane composites over the
    // terminal: the scrim (area around the panel) is faint so the terminal
    // stays visible; the panel body is near-opaque so its text is crisp.
    float overlay_panel_opacity = 0.95f; // the pane card
    float overlay_scrim_opacity = 0.25f; // the dim around it
};

struct FontConfig {
    std::string family = "monospace"; // family substring; "" -> system mono
    std::string file{};               // explicit .ttf/.otf/.ttc path (skips discovery)
    std::string fallback{};           // fallback face for CJK/emoji/symbols
    // Optional REAL styled faces. When set, bold/italic/bold-italic text renders
    // from these actual files (far better than synthesised embolden/shear).
    // Empty -> synthesise that style from the regular face.
    std::string file_bold{};
    std::string file_italic{};
    std::string file_bold_italic{};
    int size = 13;                    // logical point size (host DPI-scales it)
    bool ligatures = false;           // GSUB calt/liga shaping
};

struct ColorsConfig {
    toe::Rgb foreground = toe::rgb(220, 220, 220);
    toe::Rgb background = toe::rgb(23, 23, 28);
    toe::Rgb cursor = toe::rgb(220, 220, 220);
    toe::Rgb selection_bg = toe::rgb(60, 70, 110);
    // Reverse-video selection: swap each selected cell's fg/bg (classic terminal
    // look) instead of a coloured highlight. On by default — the crisp default.
    bool selection_invert = true;
    // Scrollback-search match highlights: every match uses `search_match`, the
    // one you're ON uses the brighter `search_current`.
    toe::Rgb search_match = toe::rgb(120, 96, 40);
    toe::Rgb search_current = toe::rgb(255, 176, 32);
    // --- selection fine-tuning (advanced) ------------------------------------
    // Minimum WCAG contrast ratio the selected text must keep against the
    // highlight; below it the glyph flips to black/white. Higher = crisper.
    float selection_contrast = 3.0f;
    // Selection corner rounding as a fraction of the cell's smaller extent
    // (0 = square corners, ~0.5 = pill). Clamped to a sane pixel range.
    float selection_radius = 0.28f;
    // How close (0..1 luma) a selection colour may sit to the background before
    // it's nudged to stay visible (both the auto-visible solid colour and the
    // inverted-block fallback use this).
    float selection_min_visibility = 0.11f;
    // The 16 ANSI palette colors (0-7 normal, 8-15 bright). Empty vector = use
    // toe's built-in palette; a full 16 overrides it.
    std::vector<toe::Rgb> palette{};
};

struct CursorConfig {
    CursorShape shape = CursorShape::Block;
    bool blink = true;
    int blink_ms = 530;          // blink half-period
    bool animate = true;         // smoothly glide the caret to its new cell
    int animate_ms = 55;         // glide time constant (smaller = snappier)
    bool animate_trail = true;   // fading comet trail on long jumps
    int animate_trail_len = 3;   // number of trail ghosts on a long jump (0..6)
};

struct TabsConfig {
    // Where the tab bar sits: "top" (default), "bottom", "left", "right".
    std::string position = "top";
    // Width of the tab column for left/right placement, in cells (auto-fit but
    // clamped to this max so it never eats too much of the screen).
    int side_width = 18;
    bool show_window_controls = true; // the min/max/close buttons on the bar
    bool show_new_tab_button = true;  // the + button
    // Auto-hide the whole tab bar; only the window controls (– □ ✕) stay,
    // floating in the top-right corner, and the terminal fills the window.
    bool auto_hide = false;
};

// The command minimap rail + its hover flyout (OSC-133 shell integration).
struct ChromeConfig {
    bool rail = true;            // show the command-minimap rail on the right edge
    int rail_width = 7;          // rail width in px (expands ~1.6x on hover)
    toe::Rgb rail_ok = toe::rgb(80, 200, 130);      // succeeded command segment
    toe::Rgb rail_failed = toe::rgb(235, 90, 90);   // failed command segment
    toe::Rgb rail_running = toe::rgb(240, 190, 70); // in-flight command segment
    bool flyout = true;          // show the command-list flyout on rail hover
    int flyout_rows = 7;         // command rows shown at once (auto-scrolls on hover)
    int flyout_width = 44;       // max flyout card width in cells
    toe::Rgb flyout_accent = toe::rgb(122, 168, 255); // title / pointer / bar
    toe::Rgb flyout_bg = toe::rgb(22, 24, 33);        // card background
    toe::Rgb flyout_border = toe::rgb(58, 64, 90);    // card frame
    // Rail segment opacity (0..255): resting vs. the hovered segment's halo.
    int rail_alpha = 210;
    int rail_hover_halo = 90;
};

struct ScrollConfig {
    int scrollback_lines = 10000; // history retained (0 = none)
    int wheel_lines = 3;          // rows per wheel notch
    bool scroll_on_output = false; // jump to bottom when new output arrives
    bool scroll_on_keystroke = true; // jump to bottom when you type
    // Drag-selection autoscroll: when you drag a selection past the top/bottom
    // edge the view scrolls so you can select across scrollback. Speed ramps
    // from `autoscroll_min` (just past the edge) to `autoscroll_max` rows/sec.
    float autoscroll_min = 3.0f;
    float autoscroll_max = 45.0f;
    int font_zoom_step = 2;        // px added/removed per Ctrl+= / Ctrl+- notch
    // Context-aware mouse pointer (I-beam over text, hand over links/rail).
    bool pointer_shapes = true;
};

struct BehaviorConfig {
    std::string shell{};          // "" -> $SHELL then /bin/sh
    std::string term = "xterm-256color"; // TERM advertised to the child
    bool audible_bell = false;    // ring the system bell on BEL
    bool visual_bell = true;      // flash on BEL
    bool copy_on_select = false;  // auto-copy a selection to the clipboard
    bool confirm_close = false;   // ask before closing with a running child
    std::string word_separators{}; // extra word-joining chars for double-click select
    bool tabs = true;             // multi-terminal Activity Tabs UI (own chrome)
};

// The complete config. Loaded from VIBE, edited by the settings panel, saved
// back. `to_toe()` projects the engine-relevant subset into a toe::Config.
struct HandConfig {
    // The active built-in theme id (see hand/theme). It is the BASE layer: on
    // load, its 16-colour palette + fg/bg/cursor/selection seed ColorsConfig,
    // then any explicit `colors { }` keys in the file override on top. So a
    // theme gives instant premium colours, and per-colour tweaks still win.
    std::string theme_name = std::string(kDefaultThemeId);

    WindowConfig window{};
    FontConfig font{};
    ColorsConfig colors{};
    CursorConfig cursor{};
    ChromeConfig chrome{};
    TabsConfig tabs{};
    ScrollConfig scroll{};
    BehaviorConfig behavior{};

    // Seed the colours from a named theme (the base layer). Unknown ids fall
    // back to the default theme. Called at load BEFORE explicit `colors { }`
    // overrides, and on a live theme switch.
    void apply_theme(std::string_view id) {
        const NamedTheme *t = find_theme(id);
        if (!t) t = find_theme(kDefaultThemeId);
        if (!t) return;
        theme_name = std::string(t->id);
        colors.foreground = t->fg;
        colors.background = t->bg;
        colors.cursor = t->cursor;
        colors.selection_bg = t->selection;
        colors.palette.assign(t->ansi.begin(), t->ansi.end());
    }

    // Project into the engine config (font, colors — the bits toe consumes at
    // Terminal::create). Host-only options (window, keybinds, bell) are applied
    // by hand itself.
    [[nodiscard]] toe::Config to_toe() const {
        toe::Config c;
        c.font_family = font.family;
        c.font_file = font.file;
        c.font_fallback = font.fallback;
        c.font_file_bold = font.file_bold;
        c.font_file_italic = font.file_italic;
        c.font_file_bold_italic = font.file_bold_italic;
        c.ligatures = font.ligatures;
        c.font_pixel_size = font.size; // logical; host scales by DPI before create
        c.default_fg = colors.foreground;
        c.default_bg = colors.background;
        c.selection_bg = colors.selection_bg;
        c.selection_invert = colors.selection_invert;
        // Blink OFF => steady cursor (period 0); else the configured half-period.
        c.cursor_blink_ms = cursor.blink ? cursor.blink_ms : 0;
        c.cursor_shape = static_cast<int>(cursor.shape);
        c.wheel_lines = scroll.wheel_lines;
        c.scroll_on_output = scroll.scroll_on_output;
        c.scroll_on_keystroke = scroll.scroll_on_keystroke;
        c.copy_on_select = behavior.copy_on_select;
        c.word_separators = behavior.word_separators;
        c.padding = window.padding;
        c.opacity = window.opacity;
        c.cursor_anim.enabled = cursor.animate;
        c.cursor_anim.time_ms = cursor.animate_ms;
        c.cursor_anim.trail = cursor.animate_trail;
        c.cursor_anim.trail_len = std::clamp(cursor.animate_trail_len, 0, 6);
        // Search-match highlight colours (all matches / current match).
        c.search_match_bg = colors.search_match;
        c.search_current_bg = colors.search_current;
        // Command-minimap rail + flyout.
        c.rail_enabled = chrome.rail;
        c.rail_width = std::clamp(chrome.rail_width, 3, 24);
        c.rail_ok = chrome.rail_ok;
        c.rail_failed = chrome.rail_failed;
        c.rail_running = chrome.rail_running;
        c.rail_alpha = std::clamp(chrome.rail_alpha, 0, 255);
        c.rail_hover_halo = std::clamp(chrome.rail_hover_halo, 0, 255);
        c.selection_contrast = colors.selection_contrast;
        c.selection_radius = colors.selection_radius;
        c.selection_min_visibility = colors.selection_min_visibility;
        return c;
    }
};

} // namespace hand

#endif // HAND_HANDCONFIG_HPP
