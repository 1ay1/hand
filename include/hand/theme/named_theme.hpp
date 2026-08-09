// SPDX-License-Identifier: LGPL-2.0-or-later
//
// NamedTheme: a complete, self-contained colour theme — the SINGLE source of
// truth for both the terminal palette (16 ANSI colours + fg/bg/cursor/selection)
// AND the UI chrome (panels, borders, accents), which is DERIVED from it.
// One pick recolours everything cohesively. That's the "eye candy" contract.
//
// The concrete theme table lives in the generated themes.gen.hpp (transcribed
// from https://github.com/mbadolato/iterm2-color-schemes). This header holds
// only the struct + lookup helpers so the generated file stays pure data.

#ifndef HAND_THEME_NAMED_THEME_HPP
#define HAND_THEME_NAMED_THEME_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "toe/core/types.hpp"

namespace hand {

struct NamedTheme {
    std::string_view id;      // stable key persisted to config ("nord")
    std::string_view label;   // human name shown in the picker
    bool dark = true;         // dark vs light (drives chrome contrast math)

    toe::Rgb bg;              // default background
    toe::Rgb fg;              // default foreground
    toe::Rgb cursor;          // cursor colour
    toe::Rgb selection;       // selection highlight background
    toe::Rgb accent;          // UI "brand" accent (titles, active tab, focus)

    // 16 ANSI palette: [0..7] normal, [8..15] bright.
    std::array<toe::Rgb, 16> ansi;
};

// Declared here, defined (as a table view) in themes.hpp. Kept out of line so
// the huge generated array is included in exactly one place.
[[nodiscard]] std::span<const NamedTheme> all_themes() noexcept;

// Find a theme by its stable id (e.g. "catppuccin-mocha"). Returns nullptr if
// unknown, so callers can fall back to a default.
[[nodiscard]] const NamedTheme *find_theme(std::string_view id) noexcept;

// The default theme id used when config names none / an unknown one.
inline constexpr std::string_view kDefaultThemeId = "catppuccin-mocha";

} // namespace hand

#endif // HAND_THEME_NAMED_THEME_HPP
