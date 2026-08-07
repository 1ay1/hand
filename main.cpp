// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libgvte with no GTK, no VTE, no SDL: the window comes from
// gvte::platform, the terminal from gvte::Terminal. Configuration is a .vibe
// file (the VIBE config format).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

#include <epoxy/gl.h>

#include "gvte/platform/surface.hpp"
#include "gvte/terminal.hpp"

#include "vibe.h"

namespace {

// Parse "#rrggbb" into an Rgb.
std::optional<gvte::Rgb> parse_color(const char *s) {
    if (!s) return std::nullopt;
    const std::string str{s};
    if (str.size() == 7 && str[0] == '#') {
        auto hex = [&](int i) {
            return std::stoi(str.substr(static_cast<size_t>(i), 2), nullptr, 16);
        };
        return gvte::rgb(static_cast<uint8_t>(hex(1)), static_cast<uint8_t>(hex(3)),
                         static_cast<uint8_t>(hex(5)));
    }
    return std::nullopt;
}

// Locate the config file: -c/--config, then $XDG_CONFIG_HOME/hand/config.vibe,
// then ~/.config/hand/config.vibe.
std::string find_config(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[static_cast<size_t>(i)];
        if (a == "-c" || a == "--config") return argv[static_cast<size_t>(i + 1)];
    }
    const char *base = std::getenv("XDG_CONFIG_HOME");
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const std::string cfg_dir = base ? std::string{base} : (home + "/.config");
    if (cfg_dir.empty()) return "";
    return cfg_dir + "/hand/config.vibe";
}

// Read the .vibe config into a gvte::Config. Unknown or missing keys keep the
// defaults; a parse error is reported but non-fatal (we fall back to defaults).
//
// Expected shape:
//
//     font {
//         family "Monospace"
//         size   11              # points; scaled to px at 96 DPI
//     }
//     colors {
//         foreground "#dcdccc"
//         background "#171720"
//     }
gvte::Config load_config(int argc, char **argv) {
    gvte::Config cfg;
    const std::string path = find_config(argc, argv);
    if (path.empty()) return cfg;

    VibeParser *parser = vibe_parser_new();
    if (!parser) return cfg;
    VibeValue *root = vibe_parse_file(parser, path.c_str());
    if (!root) {
        VibeError err = vibe_get_last_error(parser);
        if (err.code != VIBE_OK) {
            std::fprintf(stderr, "hand: config %s: %s\n", path.c_str(),
                         vibe_error_code_string(err.code));
        }
        vibe_parser_free(parser);
        return cfg;
    }

    if (VibeObject *font = vibe_get_object(root, "font")) {
        if (VibeValue *fam = vibe_object_get(font, "family")) {
            cfg.font_family = vibe_value_string_or(fam, cfg.font_family.c_str());
        }
        if (VibeValue *sz = vibe_object_get(font, "size")) {
            const int64_t pt = vibe_value_int_or(sz, 0);
            if (pt > 0) cfg.font_pixel_size = static_cast<int>(pt * 4 / 3); // pt -> px @96dpi
        }
    }
    if (VibeObject *colors = vibe_get_object(root, "colors")) {
        if (VibeValue *fg = vibe_object_get(colors, "foreground")) {
            if (auto c = parse_color(vibe_value_string_or(fg, nullptr))) cfg.default_fg = *c;
        }
        if (VibeValue *bg = vibe_object_get(colors, "background")) {
            if (auto c = parse_color(vibe_value_string_or(bg, nullptr))) cfg.default_bg = *c;
        }
    }

    vibe_value_free(root);
    vibe_parser_free(parser);
    return cfg;
}

} // namespace

int main(int argc, char **argv) {
    gvte::Config cfg = load_config(argc, argv);

    auto surface = gvte::platform::open_surface("hand", gvte::PixelSize{800, 500});
    if (!surface) {
        std::fprintf(stderr, "hand: %s\n", surface.error().message.c_str());
        return 1;
    }
    gvte::platform::Surface &surf = **surface;
    gvte::PixelSize px = surf.pixel_size();

    auto term = gvte::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "hand: %s\n", term.error().message.c_str());
        return 1;
    }

    bool running = true;
    std::string last_title;
    while (running && !surf.should_close()) {
        gvte::Terminal::Poll p = term->poll();
        if (p.exited) {
            return p.exited->code;
        }
        gvte::Session &session = *p.running;

        // Reflect the terminal's OSC 0/2 title onto the window when it changes.
        if (std::string t = session.window_title(); t != last_title) {
            surf.set_title(t);
            last_title = std::move(t);
        }

        surf.poll_events([&](const gvte::platform::Event &ev) {
            std::visit(
                [&](auto &&e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, gvte::platform::CloseRequested>) {
                        running = false;
                    } else if constexpr (std::is_same_v<T, gvte::platform::Resized>) {
                        px = e.size;
                        session.resize(px);
                    } else if constexpr (std::is_same_v<T, gvte::platform::KeyPressed>) {
                        session.send_key(e.key);
                    } else if constexpr (std::is_same_v<T, gvte::platform::TextEntered>) {
                        gvte::KeyEvent k;
                        k.key = gvte::TextInput{std::string{e.utf8}};
                        session.send_key(k);
                    }
                },
                ev);
        });

        glViewport(0, 0, px.w, px.h);
        session.render(px);
        surf.swap();
    }

    return 0;
}
