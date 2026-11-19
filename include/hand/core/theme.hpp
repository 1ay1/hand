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

[[nodiscard]] constexpr mayag::Color<mayag::Srgb> accent_of(const Appearance& a) noexcept {
    const Scheme& s = scheme(a.scheme);
    if (a.scheme_accent) return hex(s.accent);
    const bool dark = s.mode == mayag::Mode::dark;
    return mayag::Color<mayag::Oklch>{dark ? 0.76f : 0.52f, 0.15f, a.hue.get(), 1.0f}.to<mayag::Srgb>();
}

[[nodiscard]] constexpr mayag::Seed seed_of(const Appearance& a) noexcept {
    return mayag::Seed{
        .accent    = accent_of(a),
        .mode      = scheme(a.scheme).mode,
        .roundness = a.roundness.get(),
        .density   = a.density.get(),
        .type      = a.ui_scale.get() * 0.93f,
        .depth     = a.chrome == Chrome::terminal ? 0.0f : 1.0f,
        .motion    = a.motion.get(),
        .lines     = 1.0f,
    };
}

/// The chrome theme: one of mayag's looks, seeded by the Appearance, its
/// surfaces sat on the scheme's paper.
[[nodiscard]] inline mayag::Theme theme_of(const Appearance& a) {
    const Scheme& s = scheme(a.scheme);
    const mayag::Seed seed = seed_of(a);
    mayag::Theme t = [&] {
        switch (a.chrome) {
            case Chrome::standard: return mayag::looks::standard(seed);
            case Chrome::glass:    return mayag::looks::glass(seed);
            case Chrome::terminal:
            case Chrome::count_:   break;
        }
        return mayag::looks::terminal(seed);
    }();
    // Agreement with the terminal: the window background IS the paper, and
    // each surface keeps its distance from it.
    const auto paper = hex(s.paper);
    const bool dark = s.mode == mayag::Mode::dark;
    t.background     = paper;
    t.surface        = mayag::mix(paper, hex(s.ink), dark ? 0.035f : 0.03f);
    t.surface_raised = mayag::mix(paper, hex(s.ink), dark ? 0.07f : 0.05f);
    t.overlay        = mayag::mix(paper, hex(s.ink), dark ? 0.055f : 0.02f);
    t.text_primary   = hex(s.ink);
    t.text_secondary = mayag::mix(hex(s.ink), paper, 0.35f);
    t.text_disabled  = mayag::mix(hex(s.ink), paper, 0.58f);
    t.border         = mayag::mix(paper, hex(s.ink), dark ? 0.12f : 0.14f);
    t.border_strong  = mayag::mix(paper, hex(s.ink), dark ? 0.24f : 0.28f);
    t.success = hex(s.ansi[2]); t.warning = hex(s.ansi[3]); t.danger = hex(s.ansi[1]); t.info = hex(s.ansi[4]);
    return t;
}

/// The grid's colours, from the same theme: cursor and selection in the
/// accent, marks in the theme's status roles.
[[nodiscard]] inline mayag::dsl::TermTheme term_theme_of(const Appearance& a, const mayag::Theme& t) {
    const Scheme& s = scheme(a.scheme);
    const auto u = [](mayag::Color<mayag::Srgb> c) {
        const auto b = [](float x) { return static_cast<std::uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (b(c.c0) << 16) | (b(c.c1) << 8) | b(c.c2);
    };
    mayag::dsl::TermTheme th;
    th.fg = s.ink;
    th.bg = s.paper;
    th.cursor = u(t.accent);
    th.cursor_text = s.paper;
    th.selection_bg = u(mayag::mix(hex(s.paper), t.accent, 0.32f));
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
