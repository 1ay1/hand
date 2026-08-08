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
#include <chrono>

#include "gvte/platform/surface.hpp"
#include "gvte/terminal.hpp"

#include "event_router.hpp"

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
    bool last_cursor_on = true;
    bool last_blink_on = true;
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

        // Route this batch of window events through the exhaustive visitor. It
        // reports whether any of them handed bytes to the child so we can
        // coalesce the echo into this frame (below).
        hand::EventRouter router{session, surf, px, running};
        surf.poll_events([&](const gvte::platform::Event &ev) { std::visit(router, ev); });
        const bool wrote_input = router.take_wrote_input();

        // Zero-latency local echo: if we just handed the child input, push it
        // out now and give the PTY a brief moment to echo, draining whatever
        // comes back so the typed glyph renders in THIS frame instead of after
        // the next vsync. Shells echo within microseconds, so the deadline is
        // essentially never hit; when the child is genuinely busy we bail fast
        // and pick the output up on the next wake — never stalling the UI.
        if (wrote_input) {
            surf.flush(); // ensure any pending display writes don't head-of-line block
            struct pollfd pf{session.pty_fd(), POLLIN, 0};
            // poll() returns the instant the echo is readable; the 3ms is only a
            // ceiling for a busy child (then we render without it and catch up
            // on the next wake). A local shell echoes in tens of microseconds,
            // so in practice this returns almost immediately.
            if (::poll(&pf, 1, 3) > 0 && (pf.revents & POLLIN)) {
                if (!session.pump_output()) need_render = true; // child gone
            }
        }

        // Render only when the terminal's damage counter advanced — no wasted
        // GPU frames while idle. The cursor blinks on a ~530ms wall-clock phase;
        // a phase flip also triggers a redraw.
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count();
        const bool cursor_on = (ms / 530) % 2 == 0;
        // Text blink (SGR 5) toggles on a slower ~750ms phase, per VT tradition.
        const bool blink_on = (ms / 750) % 2 == 0;
        // Advance inline-image animations (kitty a=f frames); bumps damage.
        session.tick_animations(static_cast<std::uint64_t>(ms));
        if (const std::uint64_t g = session.generation();
            g != last_gen || cursor_on != last_cursor_on || blink_on != last_blink_on ||
            need_render) {
            last_gen = g;
            last_cursor_on = cursor_on;
            last_blink_on = blink_on;
            need_render = false;
            glViewport(0, 0, px.w, px.h);
            session.render(px, cursor_on, blink_on);
            surf.swap();
        }

        // Block until the child has output, a window event arrives, or the
        // blink timer fires — instead of busy-spinning at 100% CPU. This is the
        // classic terminal main loop: sleep until there's real work.
        //
        // EXCEPT during a flood: if the last drain hit its budget, the child
        // still has output queued, so we skip the sleep and loop straight back
        // to drain the next chunk — having just rendered an intermediate frame
        // and processed input. That's what keeps the UI live under `yes`/`cat`.
        surf.flush();
        if (session.output_pending()) {
            continue;
        }
        struct pollfd fds[3];
        fds[0].fd = session.pty_fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = surf.event_fd();
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        fds[2].fd = surf.repeat_fd(); // key-repeat timer (Wayland)
        fds[2].events = POLLIN;
        fds[2].revents = 0;
        int nfds = 1;
        if (fds[1].fd >= 0) fds[nfds++] = fds[1];
        if (fds[2].fd >= 0) fds[nfds++] = fds[2];
        // Poll timeout: 250ms keeps cursor-blink crisp when idle. While an
        // inline-image animation is running, cap the wait at its next-frame
        // deadline so frames play at their intended rate (min 16ms).
        int timeout = 250;
        if (const std::uint64_t dl = session.next_animation_deadline(); dl != 0) {
            const std::int64_t wait = static_cast<std::int64_t>(dl) - static_cast<std::int64_t>(ms);
            timeout = static_cast<int>(std::clamp<std::int64_t>(wait, 16, 250));
        }
        (void)::poll(fds, static_cast<nfds_t>(nfds), timeout);
    }

    return 0;
}
