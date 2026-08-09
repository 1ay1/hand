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
    const toe::Config cfg = hand::load_config(args);
    const std::vector<std::string> child = child_argv_from(argc, argv);
    return hand::run(cfg, {.title = "hand", .size = {800, 500}}, hand::Backend::automatic, child);
}
