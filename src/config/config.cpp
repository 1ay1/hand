// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Config load/save: the VIBE file <-> HandConfig round-trip.
//
// Loading is fault-tolerant by design: every field falls back to its default,
// so a typo or a missing key never stops the terminal from opening. Saving
// serializes the full schema back to canonical VIBE so the settings panel can
// persist edits.

#include "hand/config/config.hpp"
#include "hand/theme/palette_names.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace hand {

namespace {

struct VibeParserDeleter {
    void operator()(VibeParser *p) const noexcept { vibe_parser_free(p); }
};
using ParserPtr = std::unique_ptr<VibeParser, VibeParserDeleter>;

struct VibeValueDeleter {
    void operator()(VibeValue *v) const noexcept { vibe_value_free(v); }
};
using ValuePtr = std::unique_ptr<VibeValue, VibeValueDeleter>;

// Parse a file into a VibeValue tree, or nullptr on any error (with a stderr
// diagnostic for a genuine parse failure, silence for a missing file).
[[nodiscard]] ValuePtr parse_file(std::string_view path) {
    ParserPtr parser{vibe_parser_new()};
    if (!parser) return nullptr;
    std::string p(path);
    VibeValue *root = vibe_parse_file(parser.get(), p.c_str());
    if (!root) {
        VibeError e = vibe_get_last_error(parser.get());
        if (e.has_error && e.message && std::filesystem::exists(p)) {
            std::fprintf(stderr, "hand: config %.*s: %s\n", int(path.size()), path.data(), e.message);
        }
        return nullptr;
    }
    return ValuePtr{root};
}

// Read a "#rrggbb" hex color at a dotted path; keep `dst` on absence/invalid.
void load_color(VibeValue *root, const char *path, toe::Rgb &dst) {
    if (const char *s = vibe_get_string_or(root, path, nullptr)) {
        if (auto c = HexColor::parse(s)) dst = c->rgb();
    }
}

[[nodiscard]] std::string hex_of(toe::Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

CursorShape shape_from(const char *s, CursorShape dflt) {
    if (!s) return dflt;
    std::string v(s);
    if (v == "block") return CursorShape::Block;
    if (v == "bar" || v == "beam") return CursorShape::Bar;
    if (v == "underline" || v == "under") return CursorShape::Underline;
    return dflt;
}
const char *shape_str(CursorShape s) {
    switch (s) {
    case CursorShape::Bar: return "bar";
    case CursorShape::Underline: return "underline";
    default: return "block";
    }
}

} // namespace

std::optional<std::string> find_config(std::span<char *> args) {
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "-c" || a == "--config") return std::string{args[i + 1]};
    }
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string{xdg} + "/hand/config.vibe";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::string{home} + "/.config/hand/config.vibe";
    return std::nullopt;
}

HandConfig load_hand_config(std::string_view path) {
    HandConfig cfg{}; // defaults
    ValuePtr root = parse_file(path);
    if (!root) return cfg; // missing/broken file -> all defaults

    VibeValue *r = root.get();
    const auto S = [&](const char *p, std::string &dst) {
        if (const char *s = vibe_get_string_or(r, p, nullptr); s) dst = s;
    };
    const auto I = [&](const char *p, int &dst) {
        dst = static_cast<int>(vibe_get_int_or(r, p, dst));
    };
    const auto B = [&](const char *p, bool &dst) { dst = vibe_get_bool_or(r, p, dst); };

    // theme: the BASE colour layer. Resolve it first so explicit `colors { }`
    // keys below override the theme's palette rather than the other way round.
    if (const char *tn = vibe_get_string_or(r, "theme", nullptr))
        cfg.apply_theme(tn);
    else
        cfg.apply_theme(cfg.theme_name); // seed defaults from the default theme

    // window
    I("window.width", cfg.window.width);
    I("window.height", cfg.window.height);
    S("window.title", cfg.window.title);
    I("window.padding", cfg.window.padding);
    cfg.window.opacity = static_cast<float>(vibe_get_float_or(r, "window.opacity", cfg.window.opacity));
    cfg.window.overlay_panel_opacity = static_cast<float>(
        vibe_get_float_or(r, "window.overlay_panel_opacity", cfg.window.overlay_panel_opacity));
    cfg.window.overlay_scrim_opacity = static_cast<float>(
        vibe_get_float_or(r, "window.overlay_scrim_opacity", cfg.window.overlay_scrim_opacity));
    B("window.decorations", cfg.window.decorations);

    // font (accept both font.size and legacy top-level size)
    S("font.family", cfg.font.family);
    S("font.file", cfg.font.file);
    S("font.fallback", cfg.font.fallback);
    S("font.file_bold", cfg.font.file_bold);
    S("font.file_italic", cfg.font.file_italic);
    S("font.file_bold_italic", cfg.font.file_bold_italic);
    I("font.size", cfg.font.size);
    B("font.ligatures", cfg.font.ligatures);

    // colors
    load_color(r, "colors.foreground", cfg.colors.foreground);
    load_color(r, "colors.background", cfg.colors.background);
    load_color(r, "colors.cursor", cfg.colors.cursor);
    load_color(r, "colors.selection", cfg.colors.selection_bg);
    // The 16-colour ANSI palette can be authored two ways (both optional; the
    // theme's palette is the base). NAMED keys are the friendly form:
    //   colors { red "#.." green "#.." bright_blue "#.." ... }
    // and an indexed `palette [ ... ]` array still works for bulk paste. Named
    // keys win over the array where both are set.
    if (VibeObject *colors = vibe_get_object(r, "colors")) {
        // Start from the theme's palette (already seeded) so a partial set of
        // named overrides only touches those slots.
        std::vector<toe::Rgb> pal = cfg.colors.palette;
        bool touched = false;
        // 1) indexed array (bulk).
        if (VibeValue *pv = vibe_object_get(colors, "palette")) {
            if (VibeArray *arr = vibe_value_array(pv)) {
                const std::size_t n = vibe_array_size(arr);
                if (n > 0) {
                    if (pal.size() < 16) pal.resize(16, toe::rgb(0, 0, 0));
                    for (std::size_t i = 0; i < n && i < 16; ++i)
                        if (const char *s = vibe_value_string_or(vibe_array_get(arr, i), nullptr))
                            if (auto c = HexColor::parse(s)) { pal[i] = c->rgb(); touched = true; }
                }
            }
        }
        // 2) named keys (red, green, bright_blue, ...). The obvious way to hand-
        //    author a theme without memorising which index is which colour.
        for (int i = 0; i < 16; ++i) {
            const std::string key = "colors." + std::string(kAnsiNames[static_cast<std::size_t>(i)]);
            if (const char *s = vibe_get_string_or(r, key.c_str(), nullptr)) {
                if (auto c = HexColor::parse(s)) {
                    if (pal.size() < 16) pal.resize(16, toe::rgb(0, 0, 0));
                    pal[static_cast<std::size_t>(i)] = c->rgb();
                    touched = true;
                }
            }
        }
        if (touched) cfg.colors.palette = std::move(pal);
    }

    // cursor
    cfg.cursor.shape = shape_from(vibe_get_string_or(r, "cursor.shape", nullptr), cfg.cursor.shape);
    B("cursor.blink", cfg.cursor.blink);
    I("cursor.blink_ms", cfg.cursor.blink_ms);
    B("cursor.animate", cfg.cursor.animate);
    I("cursor.animate_ms", cfg.cursor.animate_ms);
    B("cursor.animate_trail", cfg.cursor.animate_trail);

    // scroll
    I("scroll.scrollback", cfg.scroll.scrollback_lines);
    I("scroll.wheel_lines", cfg.scroll.wheel_lines);
    B("scroll.on_output", cfg.scroll.scroll_on_output);
    B("scroll.on_keystroke", cfg.scroll.scroll_on_keystroke);

    // behavior
    S("behavior.shell", cfg.behavior.shell);
    S("behavior.term", cfg.behavior.term);
    B("behavior.audible_bell", cfg.behavior.audible_bell);
    B("behavior.visual_bell", cfg.behavior.visual_bell);
    B("behavior.copy_on_select", cfg.behavior.copy_on_select);
    B("behavior.confirm_close", cfg.behavior.confirm_close);

    return cfg;
}

bool save_hand_config(const HandConfig &cfg, std::string_view path) {
    ValuePtr root{vibe_value_new_object()};
    if (!root) return false;
    VibeObject *r = vibe_value_object(root.get());
    if (!r) return false;

    const auto obj = [&](const char *key) -> VibeObject * {
        VibeValue *o = vibe_value_new_object();
        vibe_object_set(r, key, o);
        return vibe_value_object(o);
    };

    vibe_object_set_string(r, "theme", cfg.theme_name.c_str());

    VibeObject *win = obj("window");
    vibe_object_set_int(win, "width", cfg.window.width);
    vibe_object_set_int(win, "height", cfg.window.height);
    vibe_object_set_string(win, "title", cfg.window.title.c_str());
    vibe_object_set_int(win, "padding", cfg.window.padding);
    vibe_object_set_float(win, "opacity", cfg.window.opacity);
    vibe_object_set_float(win, "overlay_panel_opacity", cfg.window.overlay_panel_opacity);
    vibe_object_set_float(win, "overlay_scrim_opacity", cfg.window.overlay_scrim_opacity);
    vibe_object_set_bool(win, "decorations", cfg.window.decorations);

    VibeObject *font = obj("font");
    vibe_object_set_string(font, "family", cfg.font.family.c_str());
    if (!cfg.font.file.empty()) vibe_object_set_string(font, "file", cfg.font.file.c_str());
    if (!cfg.font.fallback.empty()) vibe_object_set_string(font, "fallback", cfg.font.fallback.c_str());
    if (!cfg.font.file_bold.empty()) vibe_object_set_string(font, "file_bold", cfg.font.file_bold.c_str());
    if (!cfg.font.file_italic.empty()) vibe_object_set_string(font, "file_italic", cfg.font.file_italic.c_str());
    if (!cfg.font.file_bold_italic.empty())
        vibe_object_set_string(font, "file_bold_italic", cfg.font.file_bold_italic.c_str());
    vibe_object_set_int(font, "size", cfg.font.size);
    vibe_object_set_bool(font, "ligatures", cfg.font.ligatures);

    VibeObject *col = obj("colors");
    vibe_object_set_string(col, "foreground", hex_of(cfg.colors.foreground).c_str());
    vibe_object_set_string(col, "background", hex_of(cfg.colors.background).c_str());
    vibe_object_set_string(col, "cursor", hex_of(cfg.colors.cursor).c_str());
    vibe_object_set_string(col, "selection", hex_of(cfg.colors.selection_bg).c_str());
    // Write the 16 palette slots by NAME (red, green, bright_blue, ...) so the
    // saved file is human-readable and easy to hand-edit — not a cryptic array.
    if (!cfg.colors.palette.empty()) {
        for (std::size_t i = 0; i < cfg.colors.palette.size() && i < 16; ++i)
            vibe_object_set_string(col, std::string(kAnsiNames[i]).c_str(),
                                   hex_of(cfg.colors.palette[i]).c_str());
    }

    VibeObject *cur = obj("cursor");
    vibe_object_set_string(cur, "shape", shape_str(cfg.cursor.shape));
    vibe_object_set_bool(cur, "blink", cfg.cursor.blink);
    vibe_object_set_int(cur, "blink_ms", cfg.cursor.blink_ms);
    vibe_object_set_bool(cur, "animate", cfg.cursor.animate);
    vibe_object_set_int(cur, "animate_ms", cfg.cursor.animate_ms);
    vibe_object_set_bool(cur, "animate_trail", cfg.cursor.animate_trail);

    VibeObject *scr = obj("scroll");
    vibe_object_set_int(scr, "scrollback", cfg.scroll.scrollback_lines);
    vibe_object_set_int(scr, "wheel_lines", cfg.scroll.wheel_lines);
    vibe_object_set_bool(scr, "on_output", cfg.scroll.scroll_on_output);
    vibe_object_set_bool(scr, "on_keystroke", cfg.scroll.scroll_on_keystroke);

    VibeObject *beh = obj("behavior");
    if (!cfg.behavior.shell.empty()) vibe_object_set_string(beh, "shell", cfg.behavior.shell.c_str());
    vibe_object_set_string(beh, "term", cfg.behavior.term.c_str());
    vibe_object_set_bool(beh, "audible_bell", cfg.behavior.audible_bell);
    vibe_object_set_bool(beh, "visual_bell", cfg.behavior.visual_bell);
    vibe_object_set_bool(beh, "copy_on_select", cfg.behavior.copy_on_select);
    vibe_object_set_bool(beh, "confirm_close", cfg.behavior.confirm_close);

    // Ensure parent dir exists, then emit.
    std::string p(path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    return vibe_emit_file(root.get(), p.c_str());
}

// --- legacy toe::Config shim ------------------------------------------------
toe::Config load_config(const toe::Config &defaults, std::string_view path) {
    HandConfig hc = load_hand_config(path);
    toe::Config c = hc.to_toe();
    // Preserve any non-config fields the caller set on `defaults` (e.g. source).
    c.source = defaults.source;
    return c;
}

} // namespace hand
