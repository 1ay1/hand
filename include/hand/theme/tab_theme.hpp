// SPDX-License-Identifier: LGPL-2.0-or-later
//
// TabTheme — every colour the tab bar uses, DERIVED from the active theme (the
// single source of truth for colour). Nothing in the tab bar is hardcoded: bar
// surface, tab text, the focused tab, the accent, per-command status colours and
// the window buttons all come from the theme's bg / fg / accent / ANSI palette.
//
// derive() runs once per theme change (cheap); ChromeBar reads the result.

#ifndef HAND_THEME_TAB_THEME_HPP
#define HAND_THEME_TAB_THEME_HPP

#include <algorithm>

#include "hand/theme/named_theme.hpp"
#include "toe/core/types.hpp"

namespace hand {

struct TabTheme {
    toe::Rgb bar_bg;      // the strip background
    toe::Rgb inactive_fg; // inactive tab label
    toe::Rgb inactive_bg; // inactive tab background (== bar_bg by default)
    toe::Rgb active_fg;   // focused tab label
    toe::Rgb active_bg;   // focused tab background
    toe::Rgb accent;      // the focus accent bar / + button
    toe::Rgb status_ok;   // command succeeded
    toe::Rgb status_fail; // command failed
    toe::Rgb status_run;  // command running
    toe::Rgb pulse_ok;    // done-ok attention tint
    toe::Rgb pulse_fail;  // done-fail attention tint
    toe::Rgb unseen;      // the "new output" dot
    toe::Rgb btn_min, btn_max, btn_close; // window controls
};

namespace detail {
inline float luma_(toe::Rgb c) {
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
}
inline toe::Rgb mix_(toe::Rgb a, toe::Rgb b, float t) {
    const auto m = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::clamp(x + (static_cast<int>(y) - x) * t, 0.0f, 255.0f));
    };
    return toe::rgb(m(a.r, b.r), m(a.g, b.g), m(a.b, b.b));
}
} // namespace detail

// Build the tab palette from a theme. `dark` themes push surfaces darker; light
// themes lighter, so the bar always sits a touch off the terminal background.
inline TabTheme derive_tab_theme(const NamedTheme &t) {
    using detail::mix_;
    const bool dark = t.dark;
    const toe::Rgb black = toe::rgb(0, 0, 0);
    const toe::Rgb white = toe::rgb(255, 255, 255);

    TabTheme tt;
    // Bar surface: the terminal bg nudged toward black (dark) / white (light)
    // so the strip reads as distinct chrome, not part of the grid.
    tt.bar_bg = mix_(t.bg, dark ? black : white, dark ? 0.18f : 0.06f);
    tt.inactive_bg = tt.bar_bg;
    // Inactive label: fg dimmed toward the bar so it recedes.
    tt.inactive_fg = mix_(t.fg, tt.bar_bg, 0.45f);
    // Focused tab: lift the surface toward the accent, keep full-strength fg.
    tt.active_bg = mix_(t.bg, t.accent, dark ? 0.22f : 0.16f);
    tt.active_fg = t.fg;
    tt.accent = t.accent;
    // Status colours from the ANSI palette (the theme's own green/red/yellow),
    // so a green tab means "ok" in the theme's language.
    tt.status_ok = t.ansi[2];    // green
    tt.status_fail = t.ansi[1];  // red
    tt.status_run = t.ansi[3];   // yellow
    // Attention pulse = a low tint of the status colour over the bar.
    tt.pulse_ok = mix_(tt.bar_bg, tt.status_ok, 0.30f);
    tt.pulse_fail = mix_(tt.bar_bg, tt.status_fail, 0.30f);
    tt.unseen = t.accent;
    // Window buttons: yellow / green / red from ANSI.
    tt.btn_min = t.ansi[3];
    tt.btn_max = t.ansi[2];
    tt.btn_close = t.ansi[1];
    return tt;
}

// A safe default (used before a theme is resolved).
inline TabTheme default_tab_theme() {
    const NamedTheme *t = find_theme(kDefaultThemeId);
    if (t) return derive_tab_theme(*t);
    TabTheme tt{};
    tt.bar_bg = toe::rgb(24, 24, 32);
    tt.inactive_bg = tt.bar_bg;
    tt.inactive_fg = toe::rgb(150, 150, 165);
    tt.active_bg = toe::rgb(44, 44, 58);
    tt.active_fg = toe::rgb(235, 235, 240);
    tt.accent = toe::rgb(120, 170, 255);
    tt.status_ok = toe::rgb(120, 210, 130);
    tt.status_fail = toe::rgb(230, 120, 120);
    tt.status_run = toe::rgb(220, 200, 120);
    tt.pulse_ok = toe::rgb(30, 70, 40);
    tt.pulse_fail = toe::rgb(80, 34, 34);
    tt.unseen = toe::rgb(120, 170, 230);
    tt.btn_min = toe::rgb(180, 180, 120);
    tt.btn_max = toe::rgb(120, 180, 120);
    tt.btn_close = toe::rgb(210, 110, 110);
    return tt;
}

} // namespace hand

#endif // HAND_THEME_TAB_THEME_HPP
