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
};

struct ScrollConfig {
    int scrollback_lines = 10000; // history retained (0 = none)
    int wheel_lines = 3;          // rows per wheel notch
    bool scroll_on_output = false; // jump to bottom when new output arrives
    bool scroll_on_keystroke = true; // jump to bottom when you type
};

struct BehaviorConfig {
    std::string shell{};          // "" -> $SHELL then /bin/sh
    std::string term = "xterm-256color"; // TERM advertised to the child
    bool audible_bell = false;    // ring the system bell on BEL
    bool visual_bell = true;      // flash on BEL
    bool copy_on_select = false;  // auto-copy a selection to the clipboard
    bool confirm_close = false;   // ask before closing with a running child
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
        // Blink OFF => steady cursor (period 0); else the configured half-period.
        c.cursor_blink_ms = cursor.blink ? cursor.blink_ms : 0;
        c.cursor_shape = static_cast<int>(cursor.shape);
        c.wheel_lines = scroll.wheel_lines;
        c.scroll_on_output = scroll.scroll_on_output;
        c.scroll_on_keystroke = scroll.scroll_on_keystroke;
        c.copy_on_select = behavior.copy_on_select;
        c.padding = window.padding;
        c.opacity = window.opacity;
        c.cursor_anim.enabled = cursor.animate;
        c.cursor_anim.time_ms = cursor.animate_ms;
        c.cursor_anim.trail = cursor.animate_trail;
        return c;
    }
};

} // namespace hand

#endif // HAND_HANDCONFIG_HPP
