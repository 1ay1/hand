// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libgvte with no GTK, no VTE, no SDL: the window comes from
// gvte::platform, the terminal from gvte::Terminal. Configuration is a .vibe
// file (the VIBE config format).

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

#include <epoxy/gl.h>
#include <poll.h>

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
    std::uint64_t last_gen = 0;
    bool need_render = true; // draw the first frame
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

        // Honor OSC 52 clipboard-set requests from the running app.
        if (auto clip = session.take_clipboard_request()) {
            surf.set_clipboard(*clip);
        }

        surf.poll_events([&](const gvte::platform::Event &ev) {
            std::visit(
                [&](auto &&e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, gvte::platform::CloseRequested>) {
                        running = false;
                    } else if constexpr (std::is_same_v<T, gvte::platform::Resized>) {
                        px = e.size;
                        // TEA: window resize is a Msg; update() resizes the grid
                        // and returns a ResizePty Cmd that run() applies.
                        session.run(session.update(gvte::Resized{px}));
                    } else if constexpr (std::is_same_v<T, gvte::platform::KeyPressed>) {
                        const auto *sk = std::get_if<gvte::SpecialKey>(&e.key.key);
                        // Shift+PageUp/Down scroll the scrollback (primary screen only).
                        if (sk && e.key.mods.shift &&
                            (*sk == gvte::SpecialKey::PageUp ||
                             *sk == gvte::SpecialKey::PageDown)) {
                            if (!session.on_alt_screen()) {
                                const int page = session.grid_size().rows - 1;
                                session.scroll(*sk == gvte::SpecialKey::PageUp ? page : -page);
                            } else {
                                session.run(session.update(gvte::Key{e.key})); // let the app page
                            }
                            return;
                        }
                        // Ctrl+Shift+V pastes the clipboard into the child,
                        // bracketed when the app requested it (CSI ?2004).
                        const auto *txt = std::get_if<gvte::TextInput>(&e.key.key);
                        if (txt && e.key.mods.ctrl && e.key.mods.shift &&
                            (txt->utf8 == "v" || txt->utf8 == "V")) {
                            std::string clip = surf.get_clipboard();
                            if (!clip.empty()) {
                                if (session.bracketed_paste()) {
                                    session.send_text("\x1b[200~");
                                    session.send_text(clip);
                                    session.send_text("\x1b[201~");
                                } else {
                                    session.send_text(clip);
                                }
                            }
                            return;
                        }
                        // Ctrl+Shift+C copies the current selection to the clipboard.
                        if (txt && e.key.mods.ctrl && e.key.mods.shift &&
                            (txt->utf8 == "c" || txt->utf8 == "C")) {
                            if (session.has_selection()) {
                                std::string sel = session.selected_text();
                                if (!sel.empty()) surf.set_clipboard(sel);
                            }
                            return;
                        }
                        // Everything else is child input: hand it to the TEA
                        // pipeline — update() encodes it, run() writes it.
                        session.run(session.update(gvte::Key{e.key}));
                    } else if constexpr (std::is_same_v<T, gvte::platform::TextEntered>) {
                        session.run(session.update(gvte::Paste{std::string{e.utf8}}));
                    } else if constexpr (std::is_same_v<T, gvte::platform::MouseDown>) {
                        const int col = e.x / std::max(1, session.cell_width());
                        const int vrow = e.y / std::max(1, session.cell_height());
                        // When the app tracks the mouse (and Shift isn't held to
                        // override), report the event to it instead of selecting.
                        if (session.wants_mouse() && !e.mods.shift) {
                            int btn = (e.button == gvte::platform::MouseButton::right)  ? 2
                                      : (e.button == gvte::platform::MouseButton::middle) ? 1
                                                                                          : 0;
                            session.report_mouse(gvte::Session::MouseEvent::press, btn, col, vrow,
                                                 e.mods.shift, e.mods.alt, e.mods.ctrl);
                        } else if (e.button == gvte::platform::MouseButton::left) {
                            if (e.click_count >= 3) {
                                session.select_line(vrow, col);
                            } else if (e.click_count == 2) {
                                session.select_word(vrow, col);
                            } else {
                                session.select_begin(vrow, col, 0);
                            }
                        } else if (e.button == gvte::platform::MouseButton::middle) {
                            std::string clip = surf.get_clipboard();
                            if (!clip.empty()) session.send_text(clip);
                        }
                    } else if constexpr (std::is_same_v<T, gvte::platform::MouseMove>) {
                        const int col = e.x / std::max(1, session.cell_width());
                        const int vrow = e.y / std::max(1, session.cell_height());
                        if (session.wants_mouse() &&
                            (session.wants_mouse_motion() ||
                             (e.button_down && session.wants_mouse_drag()))) {
                            const int btn = e.button_down ? 0 : 3;
                            session.report_mouse(gvte::Session::MouseEvent::motion, btn, col, vrow,
                                                 false, false, false);
                        } else if (e.button_down) {
                            session.select_extend(vrow, col);
                        }
                    } else if constexpr (std::is_same_v<T, gvte::platform::MouseUp>) {
                        const int col = e.x / std::max(1, session.cell_width());
                        const int vrow = e.y / std::max(1, session.cell_height());
                        if (session.wants_mouse() && !e.mods.shift) {
                            int btn = (e.button == gvte::platform::MouseButton::right)  ? 2
                                      : (e.button == gvte::platform::MouseButton::middle) ? 1
                                                                                          : 0;
                            session.report_mouse(gvte::Session::MouseEvent::release, btn, col, vrow,
                                                 e.mods.shift, e.mods.alt, e.mods.ctrl);
                        } else if (e.button == gvte::platform::MouseButton::left &&
                                   session.has_selection()) {
                            std::string sel = session.selected_text();
                            if (!sel.empty()) surf.set_clipboard(sel);
                        }
                    } else if constexpr (std::is_same_v<T, gvte::platform::MouseWheel>) {
                        if (session.wants_mouse()) {
                            // Wheel buttons in the xterm protocol: 64 up, 65 down.
                            const int col = 0, vrow = 0;
                            session.report_mouse(gvte::Session::MouseEvent::press,
                                                 e.dy > 0 ? 64 : 65, col, vrow, false, false, false);
                        } else if (!session.on_alt_screen()) {
                            session.scroll(e.dy * 3); // 3 lines per wheel step
                        }
                    }
                },
                ev);
        });

        // Render only when the terminal's damage counter advanced — no wasted
        // GPU frames while idle.
        if (const std::uint64_t g = session.generation(); g != last_gen || need_render) {
            last_gen = g;
            need_render = false;
            glViewport(0, 0, px.w, px.h);
            session.render(px);
            surf.swap();
        }

        // Block until the child has output, a window event arrives, or the
        // blink timer fires — instead of busy-spinning at 100% CPU. This is the
        // classic terminal main loop: sleep until there's real work.
        surf.flush();
        struct pollfd fds[2];
        fds[0].fd = session.pty_fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = surf.event_fd();
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        const int nfds = fds[1].fd >= 0 ? 2 : 1;
        // 500ms cap keeps cursor-blink and resize responsive even when idle.
        (void)::poll(fds, static_cast<nfds_t>(nfds), 500);
    }

    return 0;
}
