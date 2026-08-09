// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The canonical names for the 16 ANSI palette slots. Authoring a theme by
// index ("palette[9]") is cryptic; authoring by NAME ("bright_red") is obvious.
// Both the config parser and the theme-file loader use this one table so the
// vocabulary is identical everywhere a user writes colours.
//
//   0 black    1 red     2 green   3 yellow
//   4 blue     5 magenta 6 cyan    7 white
//   8-15 = the same eight, "bright_" prefixed.

#ifndef HAND_THEME_PALETTE_NAMES_HPP
#define HAND_THEME_PALETTE_NAMES_HPP

#include <array>
#include <string_view>

namespace hand {

// Index -> canonical name. Index i in [0,16).
inline constexpr std::array<std::string_view, 16> kAnsiNames = {
    "black",        "red",        "green",        "yellow",
    "blue",         "magenta",    "cyan",         "white",
    "bright_black", "bright_red", "bright_green", "bright_yellow",
    "bright_blue",  "bright_magenta", "bright_cyan", "bright_white",
};

// Name -> index, or -1 if unknown. Accepts a few friendly aliases so authors
// aren't tripped up ("grey" == "bright_black", "purple" == "magenta").
[[nodiscard]] inline int ansi_index_of(std::string_view name) noexcept {
    for (int i = 0; i < 16; ++i)
        if (kAnsiNames[static_cast<std::size_t>(i)] == name) return i;
    if (name == "grey" || name == "gray") return 8;         // bright_black
    if (name == "bright_grey" || name == "bright_gray") return 7; // white-ish
    if (name == "purple") return 5;                          // magenta
    if (name == "bright_purple") return 13;
    return -1;
}

} // namespace hand

#endif // HAND_THEME_PALETTE_NAMES_HPP
