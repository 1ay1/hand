// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Public theme API: the built-in NamedTheme table + lookups, plus the DERIVED
// glyph::Theme (UI chrome) so panels, borders and accents follow the active
// terminal theme. Include this (not the generated table) from app code.

#ifndef HAND_THEME_THEMES_HPP
#define HAND_THEME_THEMES_HPP

#include "hand/glyph/glyph.hpp"
#include "hand/theme/named_theme.hpp"

namespace hand {

// Derive the UI-chrome theme (panels/borders/accents) from a terminal theme so
// the whole app recolours cohesively from one pick. The math:
//   panel_bg  = bg nudged toward fg (a hair lighter on dark, darker on light)
//   border    = a low-contrast blend of fg into bg
//   dim       = a mid blend (comments/hints)
//   focus_bg  = accent blended heavily into bg (a tinted selection band)
//   accent_fg = bg (text drawn ON the accent reads as the background)
//   ok/warn   = the palette's green / yellow (indices 2 / 3)
[[nodiscard]] glyph::Theme to_glyph_theme(const NamedTheme &t) noexcept;

} // namespace hand

#endif // HAND_THEME_THEMES_HPP
