#pragma once
// hand::theme — the Appearance, as mayag's theme system.
//
// hand has no colour system of its own. The chrome is a mayag Theme built
// by one of mayag's LOOKS (looks::terminal / standard / glass), seeded from
// the user's Appearance:
//
//     Appearance ──seed_of──▶ mayag::Seed ──looks::X──▶ mayag::Theme
//                                                         │ sheet: every widget's shape
//                                                         │ var:: tokens, springs, morph
//                                                         ▼
//                         term_theme_of ──▶ mayag::dsl::TermTheme (the grid)
//
// Because App::theme(model) returns it, every change in the settings view
// MORPHS the whole window (mayag animates theme changes in Oklab), every
// mayag widget restyles through its Sheet, and contrast is mayag's proven
// derivation, not something hand re-invents.
//
// The only thing hand adds is AGREEMENT: the theme's surfaces are pulled
// toward the scheme's paper so chrome and terminal read as one material.

#include "appearance.hpp"

#include <mayag/mayag.hpp>
#include <mayag/widget/sheet.hpp>
#include <mayag/widget/terminal_view.hpp>

namespace hand {

/// The terminal palette after the polarity knob: a dark scheme forced light
/// (or vice versa) swaps paper and ink and keeps the ANSI colours, which is
/// what `light` means in every terminal that offers it.
[[nodiscard]] constexpr Scheme effective_scheme(const Appearance& a) noexcept {
    Scheme s = scheme(a.scheme);
    const bool dark = s.mode == mayag::Mode::dark;
    const bool want_dark = a.polarity == Polarity::scheme ? dark : a.polarity == Polarity::dark;
    if (want_dark != dark) {
        std::swap(s.paper, s.ink);
        s.mode = want_dark ? mayag::Mode::dark : mayag::Mode::light;
        s.selection = 0;   // re-derive below
    }
    return s;
}

[[nodiscard]] constexpr mayag::Color<mayag::Srgb> accent_of(const Appearance& a) noexcept {
    const Scheme s = effective_scheme(a);
    switch (a.accent_from) {
        case AccentFrom::hue: {
            const bool dark = s.mode == mayag::Mode::dark;
            return mayag::Color<mayag::Oklch>{dark ? 0.76f : 0.52f, a.chroma.get(), a.hue.get(), 1.0f}.to<mayag::Srgb>();
        }
        case AccentFrom::ansi: return hex(s.ansi[static_cast<std::size_t>(a.accent_slot.get())]);
        case AccentFrom::scheme:
        case AccentFrom::count_: break;
    }
    return hex(s.accent);
}

[[nodiscard]] constexpr mayag::Seed seed_of(const Appearance& a) noexcept {
    return mayag::Seed{
        .accent    = accent_of(a),
        .mode      = effective_scheme(a).mode,
        .roundness = a.roundness.get(),
        .density   = a.density.get(),
        .type      = a.ui_scale.get() * 0.93f,
        .depth     = a.depth.get(),
        .motion    = a.motion.get(),
        .lines     = a.lines.get(),
    };
}

/// The chrome theme: one of mayag's looks (its Sheet shapes every widget),
/// seeded by the Appearance, its surfaces sat on the scheme's paper, its
/// chrome geometry from the Appearance.
[[nodiscard]] inline mayag::Theme theme_of(const Appearance& a) {
    const Scheme s = effective_scheme(a);
    const mayag::Seed seed = seed_of(a);
    mayag::Theme t = [&] {
        switch (a.chrome) {
            case Chrome::standard: return mayag::looks::standard(seed);
            case Chrome::glass:    return mayag::looks::glass(seed);
            case Chrome::brutal:   return mayag::looks::brutal(seed);
            case Chrome::terminal: return mayag::looks::terminal(seed);
            case Chrome::refined:
            case Chrome::count_:   break;
        }
        return mayag::looks::refined(seed);
    }();
    // Agreement with the terminal: the window background IS the paper; the
    // chrome is the paper shaded (chrome_shade) and tinted toward the accent
    // (chrome_tint); text and lines are steps between paper and ink.
    const bool dark = s.mode == mayag::Mode::dark;
    const auto paper = hex(s.paper), ink = hex(s.ink);
    const float sh = a.chrome_shade.get();
    const auto shade = [&](float k) {
        const auto base = sh >= 0.0f ? mayag::mix(paper, ink, k + sh) : mayag::mix(paper, mayag::colors::black, -sh * 2.0f);
        return a.chrome_tint.get() > 0.0f ? mayag::mix(base, t.accent, a.chrome_tint.get()) : base;
    };
    const float bs = a.border_strength.get();
    t.background     = paper;
    t.surface        = shade(dark ? 0.035f : 0.03f);
    t.surface_raised = shade(dark ? 0.07f : 0.05f);
    t.overlay        = shade(dark ? 0.055f : 0.02f);
    t.text_primary   = ink;
    t.text_secondary = mayag::mix(ink, paper, 0.35f);
    t.text_disabled  = mayag::mix(ink, paper, 0.58f);
    t.border         = mayag::mix(paper, ink, std::min(0.9f, (dark ? 0.12f : 0.14f) * bs));
    t.border_strong  = mayag::mix(paper, ink, std::min(0.95f, (dark ? 0.24f : 0.28f) * bs));
    t.success = hex(s.ansi[2]); t.warning = hex(s.ansi[3]); t.danger = hex(s.ansi[1]); t.info = hex(s.ansi[4]);
    t.focus_ring     = mayag::mix(paper, t.accent, 0.75f);
    t.icons = a.nerd_icons ? mayag::IconSet::nerd : mayag::IconSet::letters;
    // chrome geometry
    t.tab_shape = static_cast<mayag::TabShape>(a.tabs);
    t.bar_height = a.bar_height.get();
    t.status_height = a.status_height.get();
    t.chrome_type = a.chrome_type.get();
    t.tab_width = a.tab_width.get();
    t.tab_numbers = a.tab_numbers;
    t.tab_close_on_hover = a.tab_close_on_hover;
    return t;
}

/// The grid's colours, from the same theme: cursor and selection in the
/// accent, marks in the theme's status roles.
[[nodiscard]] inline mayag::dsl::TermTheme term_theme_of(const Appearance& a, const mayag::Theme& t) {
    const Scheme s = effective_scheme(a);
    const auto u = [](mayag::Color<mayag::Srgb> c) {
        const auto b = [](float x) { return static_cast<std::uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (b(c.c0) << 16) | (b(c.c1) << 8) | b(c.c2);
    };
    mayag::dsl::TermTheme th;
    th.fg = s.ink;
    th.bg = s.paper;
    th.cursor = u(t.accent);
    th.cursor_text = s.paper;
    th.selection_bg = s.selection && a.accent_from == AccentFrom::scheme ? s.selection
                                                                         : u(mayag::mix(hex(s.paper), t.accent, 0.32f));
    th.bold_brighten = a.bold_is_bright ? 0.25f : 0.0f;
    th.match_bg = u(mayag::mix(hex(s.paper), t.warning, 0.30f));
    th.match_current_bg = u(t.warning);
    th.match_current_fg = s.paper;
    th.hint_bg = u(t.accent);
    th.hint_fg = s.paper;
    th.mark_prompt = s.ansi[8];
    th.mark_ok = s.ansi[2];
    th.mark_error = s.ansi[1];
    th.mark_running = u(t.accent);
    th.link = s.ansi[6];
    return th;
}

}  // namespace hand
