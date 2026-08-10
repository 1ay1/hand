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
#include "hand/theme/user_themes.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

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

    // A real temp dir: "/tmp" is not a valid path for the native Windows file
    // APIs, and fopen() there returns NULL (which then crashes on fputs).
    const std::filesystem::path tmp = std::filesystem::temp_directory_path();
    const std::string theme_rt = (tmp / "theme_rt.vibe").string();
    hand::save_hand_config(c, theme_rt);
    auto d = hand::load_hand_config(theme_rt);
    ck(d.theme_name == "tokyonight", "theme name survives round-trip");
    ck(d.colors.palette.size() == 16, "palette survives round-trip");

    // Settings mirror carries the theme too.
    auto s = hand::Settings::from(d);
    ck(s.theme == "tokyonight", "Settings::from carries theme");
    ck(s.palette.size() == 16, "Settings::from carries palette");

    // --- named palette colours in config -----------------------------------
    {
        const std::string named = (tmp / "named_colors.vibe").string();
        FILE *f = std::fopen(named.c_str(), "w");
        ck(f != nullptr, "temp config file opens");
        if (!f) return 1;
        std::fputs("theme catppuccin-mocha\n"
                   "colors { red \"#ff0000\" bright_blue \"#0000ff\" }\n", f);
        std::fclose(f);
        auto nc = hand::load_hand_config(named);
        ck(nc.colors.palette.size() == 16, "named colours build a 16-slot palette");
        ck(nc.colors.palette[1].r == 0xff && nc.colors.palette[1].g == 0,
           "colors.red -> slot 1");
        ck(nc.colors.palette[12].b == 0xff && nc.colors.palette[12].r == 0,
           "colors.bright_blue -> slot 12");
        // Unspecified slots keep the theme's palette (not zeroed).
        const hand::NamedTheme *moc2 = hand::find_theme("catppuccin-mocha");
        ck(moc2 && nc.colors.palette[2] == moc2->ansi[2],
           "unset named slot keeps the theme colour");
    }

    // --- user theme files --------------------------------------------------
    {
        // Point the loader at a temp themes dir and drop one theme in it.
        // _putenv_s is the Windows spelling of setenv; the temp root comes from
        // the OS rather than a hardcoded /tmp.
        const std::filesystem::path cfg_root =
            std::filesystem::temp_directory_path() / "hand_theme_test_cfg";
        const std::string cfg_root_s = cfg_root.string();
#if defined(_WIN32)
        _putenv_s("XDG_CONFIG_HOME", cfg_root_s.c_str());
#else
        setenv("XDG_CONFIG_HOME", cfg_root_s.c_str(), 1);
#endif
        const std::filesystem::path themes_dir = cfg_root / "hand" / "themes";
        std::filesystem::create_directories(themes_dir);
        FILE *f = std::fopen((themes_dir / "probe.vibe").string().c_str(), "w");
        ck(f != nullptr, "temp theme file opens");
        if (!f) return 1;
        std::fputs("name \"Probe Theme\"\nbackground \"#101010\"\n"
                   "foreground \"#e0e0e0\"\naccent \"#00ffcc\"\n"
                   "green \"#00ff00\"\n", f);
        std::fclose(f);
        hand::load_user_themes();
        const hand::NamedTheme *u = hand::find_theme("user:probe");
        ck(u != nullptr, "user theme file loads under id user:<stem>");
        // User/custom themes must come FIRST in all_themes() so the picker lists
        // them on top.
        auto merged = hand::all_themes();
        ck(!merged.empty() &&
               std::string_view(merged.front().id).substr(0, 5) == "user:",
           "user themes sort to the top of all_themes()");
        if (u) {
            ck(std::string(u->label) == "Probe Theme", "user theme label = name");
            ck(u->bg.r == 0x10, "user theme bg parsed");
            ck(u->accent.g == 0xff && u->accent.b == 0xcc, "user theme accent parsed");
            ck(u->ansi[2].g == 0xff, "user theme named green -> slot 2");
        }
        // Export round-trip: save colours -> reload -> find it.
        hand::ThemeColors tc;
        tc.dark = true;
        tc.bg = toe::rgb(0x22, 0x22, 0x22); tc.fg = toe::rgb(0xdd, 0xdd, 0xdd);
        tc.cursor = tc.fg; tc.selection = toe::rgb(0x44, 0x44, 0x44);
        tc.accent = toe::rgb(0xff, 0x88, 0x00);
        for (int i = 0; i < 16; ++i) tc.ansi[static_cast<std::size_t>(i)] = toe::rgb(i * 16, i * 8, i * 4);
        const std::string p = hand::save_user_theme("Exported One", tc);
        ck(!p.empty(), "save_user_theme writes a file");
        ck(hand::find_theme("user:exported-one") != nullptr,
           "exported theme is immediately findable");
    }

    std::printf(fails ? "%d THEME CHECK(S) FAILED\n" : "ALL THEME CHECKS PASS\n", fails);
    return fails ? 1 : 0;
}
