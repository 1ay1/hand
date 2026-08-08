// SPDX-License-Identifier: LGPL-2.0-or-later

#include "config.hpp"

#include <cstdio>
#include <cstdlib>

namespace hand {

namespace {

// RAII over the VIBE parser handle: the C API hands back a raw pointer that
// must be freed on every exit path. A unique_ptr with a custom deleter makes
// that impossible to forget.
struct VibeParserDeleter {
    void operator()(VibeParser *p) const noexcept { vibe_parser_free(p); }
};
using ParserPtr = std::unique_ptr<VibeParser, VibeParserDeleter>;

struct VibeValueDeleter {
    void operator()(VibeValue *v) const noexcept { vibe_value_free(v); }
};
using ValuePtr = std::unique_ptr<VibeValue, VibeValueDeleter>;

} // namespace

std::optional<std::string> find_config(std::span<char *> args) {
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "-c" || a == "--config") return std::string{args[i + 1]};
    }
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string{xdg} + "/hand/config.vibe";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string{home} + "/.config/hand/config.vibe";
    }
    return std::nullopt;
}

toe::Config load_config(const toe::Config &defaults, std::string_view path) {
    toe::Config cfg = defaults;
    const std::string path_s{path};

    ParserPtr parser{vibe_parser_new()};
    if (!parser) return cfg;

    ValuePtr root{vibe_parse_file(parser.get(), path_s.c_str())};
    if (!root) {
        if (const VibeError err = vibe_get_last_error(parser.get()); err.code != VIBE_OK) {
            std::fprintf(stderr, "hand: config %s: %s\n", path_s.c_str(),
                         vibe_error_code_string(err.code));
        }
        return cfg; // missing / malformed file -> defaults
    }

    if (VibeObject *font = vibe_get_object(root.get(), "font")) {
        if (VibeValue *fam = vibe_object_get(font, "family")) {
            cfg.font_family = vibe_value_string_or(fam, cfg.font_family.c_str());
        }
        if (VibeValue *sz = vibe_object_get(font, "size")) {
            if (const std::int64_t pt = vibe_value_int_or(sz, 0); pt > 0) {
                cfg.font_pixel_size = static_cast<int>(pt * 4 / 3); // pt -> px @ 96 DPI
            }
        }
    }

    if (VibeObject *colors = vibe_get_object(root.get(), "colors")) {
        const auto apply = [&](const char *key, toe::Rgb &dst) {
            if (VibeValue *v = vibe_object_get(colors, key)) {
                if (const char *s = vibe_value_string_or(v, nullptr)) {
                    if (auto c = HexColor::parse(s)) dst = c->rgb();
                }
            }
        };
        apply("foreground", cfg.default_fg);
        apply("background", cfg.default_bg);
    }

    return cfg;
}

} // namespace hand
