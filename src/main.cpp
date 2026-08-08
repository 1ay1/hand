// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libtoe with no GTK, no VTE, no SDL.
//
// toe owns the contract of everything; hand just implements it. There is ONE
// contract — `toe::App` — the window: how to open it (App::open), how to present
// it, how to feed it input, when it closes. hand implements exactly that in
// `hand::App`, which opens the right native backend (Wayland/X11/offscreen) for
// the machine and forwards the contract to it. Everything downstream — PTY, VT,
// screen, renderer, the frame loop — is toe's, behind `toe::run`.
//
// So main is the whole story in one line: name the window type, hand it to the
// engine. toe opens it and runs the terminal in it.

#include <span>

#include "hand/app.hpp"
#include "hand/config/config.hpp"

#include "toe/run.hpp"

int main(int argc, char **argv) {
    const std::span<char *> args{argv, static_cast<std::size_t>(argc)};
    const toe::Config cfg = hand::load_config(args);
    return toe::run<hand::App>(cfg, {.title = "hand", .size = {800, 500}});
}
