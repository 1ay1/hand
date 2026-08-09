// SPDX-License-Identifier: LGPL-2.0-or-later
//
// themes_test — guards the built-in theme table + the terminal->UI derivation:
//   * all themes are present, non-empty, and have unique ids
//   * every theme's ANSI palette is fully populated (16 slots)
//   * the default theme id resolves
//   * to_glyph_theme() produces a sane chrome (accent_fg == bg, ok/warn wired)
//   * a theme name survives the full config round-trip AND its palette reaches
//     ColorsConfig as the base layer.
#include "hand/config/config.hpp"
#include "hand/settings_panel.hpp"
#include "hand/theme/themes.hpp"

#include <cstdio>
#include <set>
#include <string>

int main() {
    int fails = 0;
    auto ck = [&](bool ok, const char *n) {
        if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
    };

    auto themes = hand::all_themes();
    ck(!themes.empty(), "table non-empty");
    ck(themes.size() > 100, "table has the full iterm2 set (>100)");

    // Unique, non-empty ids + labels; full palette.
    std::set<std::string> ids;
    bool dup = false, empty = false, short_pal = false;
    for (const auto &t : themes) {
        if (t.id.empty() || t.label.empty()) empty = true;
        if (!ids.insert(std::string(t.id)).second) dup = true;
        // std::array<Rgb,16> is fixed-size; just sanity-check it's addressable.
        if (t.ansi.size() != 16) short_pal = true;
    }
    ck(!empty, "no empty id/label");
    ck(!dup, "ids are unique");
    ck(!short_pal, "every palette has 16 colours");

    // Lookups.
    ck(hand::find_theme("catppuccin-mocha") != nullptr, "find catppuccin-mocha");
    ck(hand::find_theme("nord") != nullptr, "find nord");
    ck(hand::find_theme("does-not-exist") == nullptr, "unknown id -> nullptr");
    ck(hand::find_theme(hand::kDefaultThemeId) != nullptr, "default id resolves");

    // Chrome derivation.
    const hand::NamedTheme *moc = hand::find_theme("catppuccin-mocha");
    ck(moc != nullptr, "mocha present");
    if (moc) {
        auto g = hand::to_glyph_theme(*moc);
        ck(g.bg == moc->bg, "chrome bg == theme bg");
        ck(g.fg == moc->fg, "chrome fg == theme fg");
        ck(g.accent_fg == moc->bg, "accent_fg reads as bg");
        ck(g.ok == moc->ansi[2], "ok == palette green");
        ck(g.warn == moc->ansi[3], "warn == palette yellow");
        // panel_bg must differ from bg (the subtle raised card) yet stay close.
        ck(!(g.panel_bg == g.bg), "panel_bg lifts off bg");
    }

    // Config round-trip: theme name persists AND seeds the colour base layer.
    hand::HandConfig c;
    c.apply_theme("tokyonight");
    ck(c.theme_name == "tokyonight", "apply_theme sets name");
    ck(c.colors.palette.size() == 16, "apply_theme fills palette");
    const hand::NamedTheme *tn = hand::find_theme("tokyonight");
    ck(tn && c.colors.background == tn->bg, "apply_theme sets bg");

    hand::save_hand_config(c, "/tmp/theme_rt.vibe");
    auto d = hand::load_hand_config("/tmp/theme_rt.vibe");
    ck(d.theme_name == "tokyonight", "theme name survives round-trip");
    ck(d.colors.palette.size() == 16, "palette survives round-trip");

    // Settings mirror carries the theme too.
    auto s = hand::Settings::from(d);
    ck(s.theme == "tokyonight", "Settings::from carries theme");
    ck(s.palette.size() == 16, "Settings::from carries palette");

    std::printf(fails ? "%d THEME CHECK(S) FAILED\n" : "ALL THEME CHECKS PASS\n", fails);
    return fails ? 1 : 0;
}
