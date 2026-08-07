// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libgvte with no GTK, no VTE, no SDL: the window comes from
// gvte::platform, the terminal from gvte::Terminal. Reads an INI config for
// font + colors (its own, falling back to termite's for compatibility).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include <epoxy/gl.h>

#include "gvte/platform/surface.hpp"
#include "gvte/terminal.hpp"

namespace {

// --- a tiny INI reader (section.key -> value) ------------------------------
using Ini = std::unordered_map<std::string, std::string>;

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

Ini parse_ini(const std::string &path) {
    Ini ini;
    std::ifstream in(path);
    if (!in) return ini;
    std::string line, section;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = s.substr(1, s.size() - 2);
            continue;
        }
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(s.substr(0, eq));
        const std::string val = trim(s.substr(eq + 1));
        ini[section + "." + key] = val;
    }
    return ini;
}

std::optional<std::string> get(const Ini &ini, const std::string &k) {
    if (auto it = ini.find(k); it != ini.end()) return it->second;
    return std::nullopt;
}

// Parse "#rrggbb" (the GTK/termite color form) into an Rgb.
std::optional<gvte::Rgb> parse_color(const std::string &s) {
    if (s.size() == 7 && s[0] == '#') {
        auto hex = [&](int i) { return std::stoi(s.substr(static_cast<size_t>(i), 2), nullptr, 16); };
        return gvte::rgb(static_cast<uint8_t>(hex(1)), static_cast<uint8_t>(hex(3)),
                         static_cast<uint8_t>(hex(5)));
    }
    return std::nullopt;
}

// Termite fonts look like "Monospace 9" / "DejaVu Sans Mono 11". Split the
// trailing integer point-size off the family. gvte wants a pixel size, so we
// scale points to pixels at a nominal 96 DPI (pt * 96/72 = pt * 4/3).
void apply_font(const std::string &spec, gvte::Config &cfg) {
    std::istringstream iss(spec);
    std::string token, family;
    int last_size = 0;
    while (iss >> token) {
        char *end = nullptr;
        const long n = std::strtol(token.c_str(), &end, 10);
        if (end && *end == '\0' && n > 0) {
            last_size = static_cast<int>(n);
        } else {
            if (!family.empty()) family += ' ';
            family += token;
        }
    }
    if (!family.empty()) cfg.font_family = family;
    if (last_size > 0) cfg.font_pixel_size = last_size * 4 / 3;
}

// Locate the config file: -c/--config, then $XDG_CONFIG_HOME/hand/config, then
// ~/.config/hand/config, falling back to termite's paths for compatibility.
std::string find_config(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[static_cast<size_t>(i)];
        if (a == "-c" || a == "--config") return argv[static_cast<size_t>(i + 1)];
    }
    const char *base = std::getenv("XDG_CONFIG_HOME");
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const std::string cfg_dir = base ? std::string{base} : (home + "/.config");
    if (cfg_dir.empty()) return "";

    for (const char *app : {"hand", "termite"}) {
        std::string p = cfg_dir + "/" + app + "/config";
        if (std::ifstream{p}) return p;
    }
    return "";
}

gvte::Config load_config(int argc, char **argv) {
    gvte::Config cfg;
    const std::string path = find_config(argc, argv);
    if (path.empty()) return cfg;
    const Ini ini = parse_ini(path);

    if (auto f = get(ini, "options.font")) apply_font(*f, cfg);
    if (auto c = get(ini, "colors.foreground")) {
        if (auto col = parse_color(*c)) cfg.default_fg = *col;
    }
    if (auto c = get(ini, "colors.background")) {
        if (auto col = parse_color(*c)) cfg.default_bg = *col;
    }
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
    while (running && !surf.should_close()) {
        gvte::Terminal::Poll p = term->poll();
        if (p.exited) {
            return p.exited->code;
        }
        gvte::Session &session = *p.running;

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
