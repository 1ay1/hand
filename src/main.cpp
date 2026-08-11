// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libtoe with no GTK, no VTE, no SDL.
//
// toe owns the contract of everything; hand just implements it. There is ONE
// contract — `toe::App` — the window: how to open it, present it, feed it input,
// know when it closes. hand provides three models (Wayland/X11/offscreen), each
// a distinct thin handle so `toe::run<App>` is fully MONOMORPHIC per backend —
// no vtable, no per-op branch. `hand::run` makes the single Wayland-vs-X11
// choice from the environment ONCE at startup, then enters that backend's
// specialised loop. Everything downstream — PTY, VT, screen, renderer, the
// frame loop — is toe's.

#include <span>
#include <string>
#include <vector>

#include "hand/app.hpp"
#include "hand/config/config.hpp"
#include "hand/gui/host_config.hpp"
#include "hand/settings_panel.hpp"

// Parse the child command from argv:
//   hand                 -> spawn $SHELL (default)
//   hand -e CMD ARGS...  -> run CMD ARGS... as the child (everything after -e)
//   hand CMD ARGS...     -> run CMD ARGS... (first non-option token onward)
// The config flag (-c/--config PATH) is consumed by load_config and skipped.
static std::vector<std::string> child_argv_from(int argc, char **argv) {
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-e" || a == "--exec") {
            for (int j = i + 1; j < argc; ++j) out.emplace_back(argv[j]);
            return out;
        }
        if (a == "-c" || a == "--config") {
            ++i; // skip the config path argument
            continue;
        }
        if (!a.empty() && a.front() == '-') continue; // unknown option: ignore
        // First bare token: treat it and the rest as the command to run.
        for (int j = i; j < argc; ++j) out.emplace_back(argv[j]);
        return out;
    }
    return out;
}

int main(int argc, char **argv) {
    const std::span<char *> args{argv, static_cast<std::size_t>(argc)};
    const hand::HandConfig hc = hand::load_hand_config(args);
    const std::string cfg_path = hand::find_config(args).value_or(std::string{});
    // Hand the loaded config + its path to the settings panel, so it seeds its
    // form and knows where to persist edits.
    hand::set_settings_source(hc, cfg_path);

    const toe::Config cfg = hc.to_toe();
    // Host-side (hand-only) runtime knobs the GUI loop reads (autoscroll, font
    // zoom, pointer shapes, flyout) — filled once, read process-wide.
    {
        auto &host = hand::host_config();
        host.autoscroll_min = hc.scroll.autoscroll_min;
        host.autoscroll_max = hc.scroll.autoscroll_max;
        host.font_zoom_step = hc.scroll.font_zoom_step;
        host.pointer_shapes = hc.scroll.pointer_shapes;
        host.flyout = hc.chrome.flyout;
        host.flyout_rows = hc.chrome.flyout_rows;
        host.flyout_width = hc.chrome.flyout_width;
        host.flyout_accent = (static_cast<std::uint32_t>(hc.chrome.flyout_accent.r) << 16) |
                             (static_cast<std::uint32_t>(hc.chrome.flyout_accent.g) << 8) |
                             static_cast<std::uint32_t>(hc.chrome.flyout_accent.b);
        auto pack = [](toe::Rgb c) {
            return (static_cast<std::uint32_t>(c.r) << 16) |
                   (static_cast<std::uint32_t>(c.g) << 8) | static_cast<std::uint32_t>(c.b);
        };
        host.flyout_bg = pack(hc.chrome.flyout_bg);
        host.flyout_border = pack(hc.chrome.flyout_border);
    }
    const std::vector<std::string> child = child_argv_from(argc, argv);
    // Tabs come from config (default on) OR the HAND_TABS env var. Tabbed mode
    // draws its OWN chrome, so it turns off the server titlebar.
    const char *tabs_env = std::getenv("HAND_TABS");
    const bool tabs = hc.behavior.tabs || (tabs_env && *tabs_env);
    const bool decorations = hc.window.decorations && !tabs;
    return hand::run(cfg,
                     {.title = hc.window.title,
                      .size = {hc.window.width, hc.window.height},
                      .decorations = decorations},
                     hand::Backend::automatic, child, tabs);
}
