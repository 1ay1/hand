// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libtoe with no GTK, no VTE, no SDL.
//
// toe owns the contract of everything; hand just implements it. toe declares
// what a frontend must provide — the `toe::App` contract: a `surface()` (the
// window) and a `config()` (the build recipe) — and owns everything downstream:
// PTY, VT, screen, renderer, and the frame loop (`toe::run`). hand supplies one
// native backend per platform (Wayland/X11/offscreen), each a model of
// `toe::Surface`, wrapped with the parsed config into a `hand::TerminalApp` that
// satisfies `toe::App`. This file is a thin shim: parse the config, then hand
// off to the platform dispatcher, which resolves the backend once, opens its
// concrete surface, and tail-calls `toe::run(app)` — a fully MONOMORPHIC
// (no-vtable) loop instantiated on that concrete App.

#include <span>

#include "hand/config/config.hpp"
#include "hand/platform/backend.hpp"

int main(int argc, char **argv) {
    const std::span<char *> args{argv, static_cast<std::size_t>(argc)};
    const toe::Config cfg = hand::load_config(args);
    return hand::platform::run(cfg, "hand", toe::PixelSize{800, 500});
}
