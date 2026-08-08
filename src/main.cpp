// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libtoe with no GTK, no VTE, no SDL.
//
// The engine (toe) owns everything from PTY to pixels — the "terminal work".
// hand owns everything native: the window, input, clipboard and GL context,
// one modular backend per platform. This file is a thin shim: parse the config,
// then hand off to the platform dispatcher, which resolves the backend once and
// runs a fully MONOMORPHIC (no-vtable) frame loop over the concrete surface.

#include <span>

#include "hand/config/config.hpp"
#include "hand/platform/backend.hpp"

int main(int argc, char **argv) {
    const std::span<char *> args{argv, static_cast<std::size_t>(argc)};
    const toe::Config cfg = hand::load_config(args);
    return hand::platform::run(cfg, "hand", toe::PixelSize{800, 500});
}
