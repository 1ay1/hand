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

#include "hand/app.hpp"
#include "hand/config/config.hpp"

int main(int argc, char **argv) {
    const std::span<char *> args{argv, static_cast<std::size_t>(argc)};
    const toe::Config cfg = hand::load_config(args);
    return hand::run(cfg, {.title = "hand", .size = {800, 500}});
}
