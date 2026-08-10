// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Theme registry: the 600+ generated built-ins PLUS user themes discovered in
// ~/.config/hand/themes/*.vibe, unified behind all_themes()/find_theme() so the
// picker and config treat them identically. Also the UI-chrome derivation.
//
// The generated built-in array is included in exactly one TU (here) to keep its
// 270 KB out of every other object file.

#include "hand/theme/themes.hpp"
#include "hand/theme/themes.gen.hpp"
#include "hand/theme/user_themes.hpp"
#include "hand/theme/palette_names.hpp"

#include "vibe.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hand {
namespace {

// --- user-theme registry ---------------------------------------------------
// Owned themes (their string_views point into these strings). Rebuilt on
// load_user_themes(). all_themes() returns a merged view: user themes FIRST
// (so they sort to the top of the picker and win find_theme by id), then the
// built-ins.
std::vector<std::unique_ptr<OwnedTheme>> g_user;
std::vector<NamedTheme> g_merged;
std::once_flag g_once;
bool g_merged_valid = false;

// Rebuild the merged [user..., builtins...] view.
void rebuild_merged() {
    g_merged.clear();
    g_merged.reserve(g_user.size() + static_cast<std::size_t>(themes::kBuiltinCount));
    for (const auto &u : g_user) g_merged.push_back(u->view);
    for (const auto &b : themes::kBuiltins) g_merged.push_back(b);
    g_merged_valid = true;
}

float luminance(toe::Rgb c) {
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
}

std::optional<toe::Rgb> parse_hex(const char *s) {
    if (!s) return std::nullopt;
    std::string_view v{s};
    if (v.size() != 7 || v.front() != '#') return std::nullopt;
    auto d = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int b[6];
    for (int i = 0; i < 6; ++i) { b[i] = d(v[1 + i]); if (b[i] < 0) return std::nullopt; }
    return toe::rgb(std::uint8_t(b[0] * 16 + b[1]), std::uint8_t(b[2] * 16 + b[3]),
                    std::uint8_t(b[4] * 16 + b[5]));
}

std::string slugify(std::string_view name) {
    std::string out;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (c >= 'A' && c <= 'Z') out += char(c - 'A' + 'a');
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "theme" : out;
}

// Parse one theme .vibe file into an OwnedTheme. Needs at least a background +
// foreground to be usable; everything else has a sensible fallback.
std::unique_ptr<OwnedTheme> parse_theme_file(const std::filesystem::path &path) {
    std::unique_ptr<VibeParser, void (*)(VibeParser *)> parser(vibe_parser_new(),
                                                               vibe_parser_free);
    if (!parser) return nullptr;
    // path::c_str() is wchar_t* on Windows; vibe's C API takes UTF-8 char*, so
    // go through string() (which narrows) rather than c_str().
    const std::string path_utf8 = path.string();
    VibeValue *root = vibe_parse_file(parser.get(), path_utf8.c_str());
    if (!root) return nullptr;
    std::unique_ptr<VibeValue, void (*)(VibeValue *)> owned(root, vibe_value_free);

    auto get = [&](const char *k) -> std::optional<toe::Rgb> {
        return parse_hex(vibe_get_string_or(root, k, nullptr));
    };
    auto bg = get("background");
    auto fg = get("foreground");
    if (!bg || !fg) return nullptr; // not a usable theme

    auto t = std::make_unique<OwnedTheme>();
    // id = "user:" + file stem, so it can never collide with a built-in id.
    t->id = "user:" + slugify(path.stem().string());
    if (const char *nm = vibe_get_string_or(root, "name", nullptr))
        t->label = nm;
    else
        t->label = path.stem().string();

    NamedTheme &nt = t->view;
    nt.bg = *bg;
    nt.fg = *fg;
    nt.cursor = get("cursor").value_or(*fg);
    nt.selection = get("selection").value_or(toe::rgb(0x40, 0x40, 0x40));
    // `dark` may be explicit; otherwise infer from the background luminance.
    nt.dark = vibe_get_bool_or(root, "dark", luminance(*bg) < 0.5f);
    // 16 ANSI slots by name (missing ones stay a neutral grey).
    for (int i = 0; i < 16; ++i) {
        auto c = get(std::string(kAnsiNames[static_cast<std::size_t>(i)]).c_str());
        nt.ansi[static_cast<std::size_t>(i)] = c.value_or(toe::rgb(0x80, 0x80, 0x80));
    }
    // accent defaults to the palette's (bright) blue.
    nt.accent = get("accent").value_or(nt.ansi[12]);

    // Point the string_views at the owned strings (stable for the theme's life).
    nt.id = t->id;
    nt.label = t->label;
    return t;
}

} // namespace

// The user THEME directory. Built-in themes are compiled in (themes.gen.hpp is
// a constexpr table), so this is purely for optional user-authored *.vibe
// overrides and is allowed not to exist.
//
// This MUST resolve the same way find_config() does in config/config.cpp
// (XDG_CONFIG_HOME, then HOME) — the settings panel saves themes here and
// reads the config from there, so a divergence would write one place and read
// another. On Windows, MSYS2/Git-Bash style environments set HOME; a bare cmd
// session falls back to USERPROFILE below.
std::string user_themes_dir() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/hand/themes";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/hand/themes";
#if defined(_WIN32)
    if (const char *up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "/.config/hand/themes";
#endif
    return {};
}

void load_user_themes() {
    g_user.clear();
    const std::string dir = user_themes_dir();
    if (!dir.empty()) {
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            const auto &p = e.path();
            if (p.extension() != ".vibe") continue;
            if (auto t = parse_theme_file(p)) g_user.push_back(std::move(t));
        }
        // Stable alphabetical order in the picker.
        std::sort(g_user.begin(), g_user.end(),
                  [](const auto &a, const auto &b) { return a->label < b->label; });
    }
    rebuild_merged();
}

std::string save_user_theme(std::string_view display_name, const ThemeColors &colors) {
    const std::string dir = user_themes_dir();
    if (dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string stem = slugify(display_name);
    const std::string path = dir + "/" + stem + ".vibe";

    std::unique_ptr<VibeValue, void (*)(VibeValue *)> root(vibe_value_new_object(),
                                                           vibe_value_free);
    if (!root) return {};
    VibeObject *o = vibe_value_object(root.get());
    auto hex = [](toe::Rgb c) {
        char b[8];
        std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
        return std::string(b);
    };
    vibe_object_set_string(o, "name", std::string(display_name).c_str());
    vibe_object_set_bool(o, "dark", colors.dark);
    vibe_object_set_string(o, "background", hex(colors.bg).c_str());
    vibe_object_set_string(o, "foreground", hex(colors.fg).c_str());
    vibe_object_set_string(o, "cursor", hex(colors.cursor).c_str());
    vibe_object_set_string(o, "selection", hex(colors.selection).c_str());
    vibe_object_set_string(o, "accent", hex(colors.accent).c_str());
    for (int i = 0; i < 16; ++i)
        vibe_object_set_string(o, std::string(kAnsiNames[static_cast<std::size_t>(i)]).c_str(),
                               hex(colors.ansi[static_cast<std::size_t>(i)]).c_str());

    if (!vibe_emit_file(root.get(), path.c_str())) return {};
    load_user_themes(); // so the new theme shows up immediately in the picker
    return path;
}

std::span<const NamedTheme> all_themes() noexcept {
    std::call_once(g_once, [] { load_user_themes(); });
    if (!g_merged_valid) rebuild_merged();
    return {g_merged.data(), g_merged.size()};
}

const NamedTheme *find_theme(std::string_view id) noexcept {
    std::call_once(g_once, [] { load_user_themes(); });
    for (const auto &u : g_user)
        if (u->view.id == id) return &u->view;
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
    g.panel_bg = mix(t.bg, t.fg, t.dark ? 0.06f : 0.05f);
    g.fg = t.fg;
    g.dim = mix(t.fg, t.bg, 0.45f);
    g.accent = t.accent;
    g.accent_fg = t.bg;
    g.border = mix(t.bg, t.fg, t.dark ? 0.18f : 0.22f);
    g.focus_bg = mix(t.bg, t.accent, t.dark ? 0.22f : 0.16f);
    g.ok = t.ansi[2];
    g.warn = t.ansi[3];
    return g;
}

} // namespace hand
