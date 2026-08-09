// SPDX-License-Identifier: LGPL-2.0-or-later
//
// User theme files: drop a `*.vibe` theme into ~/.config/hand/themes/ and it
// shows up in the picker next to the 600+ built-ins — no rebuild, no code.
// This is the whole point of easy authoring: a theme is ONE small, obvious file.
//
// Format (every field optional except a background+foreground; sensible
// fallbacks otherwise):
//
//   name       "My Theme"          # shown in the picker (file stem if omitted)
//   dark       true                # drives UI-chrome contrast (default: auto)
//   background "#1e1e2e"
//   foreground "#cdd6f4"
//   cursor     "#f5e0dc"
//   selection  "#585b70"
//   accent     "#89b4fa"           # UI brand colour (default: bright blue)
//   # the 16 ANSI slots, by NAME:
//   black "#.." red "#.." green "#.." yellow "#.."
//   blue "#.." magenta "#.." cyan "#.." white "#.."
//   bright_black "#.." bright_red "#.." ... bright_white "#.."
//
// The id (config key) is the file stem, prefixed "user:" so it can never
// collide with a built-in.

#ifndef HAND_THEME_USER_THEMES_HPP
#define HAND_THEME_USER_THEMES_HPP

#include <array>
#include <string>
#include <vector>

#include "hand/theme/named_theme.hpp"

namespace hand {

// Owns the strings a NamedTheme string_view's point at, plus the theme itself.
struct OwnedTheme {
    std::string id;
    std::string label;
    NamedTheme view; // id/label string_views point into this struct's strings
};

// The directory user themes live in: $XDG_CONFIG_HOME/hand/themes (or
// ~/.config/hand/themes). Returns empty if no home can be found.
[[nodiscard]] std::string user_themes_dir();

// Load + parse every *.vibe in the themes dir. Idempotent; call once at startup
// (and after writing a new theme) to (re)populate the registry so the picker
// and find_theme() see user themes alongside the built-ins.
void load_user_themes();

// Save `cfg`'s current colours as a user theme file named <stem>.vibe with the
// given display name, then reload the registry. Returns the written path (empty
// on failure). This is the "export my current colours as a theme" action.
struct ThemeColors {
    bool dark = true;
    toe::Rgb bg, fg, cursor, selection, accent;
    std::array<toe::Rgb, 16> ansi{};
};
[[nodiscard]] std::string save_user_theme(std::string_view display_name,
                                          const ThemeColors &colors);

} // namespace hand

#endif // HAND_THEME_USER_THEMES_HPP
