// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Theme table lookups + UI-chrome derivation. The generated built-in array is
// included in exactly one TU (here) to keep its 270 KB out of every other
// object file.

#include "hand/theme/themes.hpp"
#include "hand/theme/themes.gen.hpp"

#include <algorithm>
#include <cstdint>

namespace hand {

std::span<const NamedTheme> all_themes() noexcept {
    return {themes::kBuiltins, static_cast<std::size_t>(themes::kBuiltinCount)};
}

const NamedTheme *find_theme(std::string_view id) noexcept {
    for (const auto &t : themes::kBuiltins)
        if (t.id == id) return &t;
    return nullptr;
}

namespace {

// Blend a -> b by t in [0,1].
toe::Rgb mix(toe::Rgb a, toe::Rgb b, float t) noexcept {
    auto c = [t](std::uint8_t x, std::uint8_t y) {
        const float v = static_cast<float>(x) + (static_cast<float>(y) - x) * t;
        return static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    };
    return toe::Rgb{c(a.r, b.r), c(a.g, b.g), c(a.b, b.b)};
}

} // namespace

glyph::Theme to_glyph_theme(const NamedTheme &t) noexcept {
    glyph::Theme g;
    g.bg = t.bg;
    // Panel sits slightly above the terminal bg: lift toward fg on dark themes,
    // sink toward fg (i.e. darker) on light themes for a subtle raised card.
    g.panel_bg = mix(t.bg, t.fg, t.dark ? 0.06f : 0.05f);
    g.fg = t.fg;
    g.dim = mix(t.fg, t.bg, 0.45f);           // hints / secondary text
    g.accent = t.accent;
    g.accent_fg = t.bg;                        // text on the accent = the bg
    g.border = mix(t.bg, t.fg, t.dark ? 0.18f : 0.22f);
    g.focus_bg = mix(t.bg, t.accent, t.dark ? 0.22f : 0.16f); // tinted select band
    g.ok = t.ansi[2];                          // green
    g.warn = t.ansi[3];                        // yellow
    return g;
}

} // namespace hand
