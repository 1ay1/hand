// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::App::open — the ONE place that knows all three backends exist. It picks
// a backend from the environment (or an explicit request), opens it, and wraps
// the live one in a hand::App. Every concrete backend type (Wayland/X11/
// offscreen) stays in its own TU behind the AppBackend interface; nothing but a
// unique_ptr<AppBackend> crosses this boundary, so no wl_*/xcb_*/EGL type leaks.
//
// main writes `toe::run<hand::App>(cfg, win)`; toe calls App::open; App::open
// selects. hand owns "which window"; toe owns "run the terminal".

#include "hand/app.hpp"

#include <cstdlib>

namespace hand {

toe::Result<App> App::open(const toe::WindowConfig &win, Backend force) {
    // Honor an explicit backend request first.
    switch (force) {
    case Backend::wayland:
        if (auto b = open_wayland(win)) return App{std::move(b)};
        return toe::fail("wayland backend unavailable");
    case Backend::x11:
        if (auto b = open_x11(win)) return App{std::move(b)};
        return toe::fail("x11 backend unavailable");
    case Backend::offscreen:
        if (auto b = open_offscreen(win)) return App{std::move(b)};
        return toe::fail("offscreen backend unavailable");
    case Backend::automatic:
        break;
    }

    // Automatic: honor TOE_HEADLESS, then Wayland, then X11, then offscreen.
    const char *wl = std::getenv("WAYLAND_DISPLAY");
    const char *x = std::getenv("DISPLAY");
    const char *headless = std::getenv("TOE_HEADLESS");

    if (headless && headless[0] != '\0') {
        if (auto b = open_offscreen(win)) return App{std::move(b)};
        return toe::fail("offscreen backend unavailable");
    }

    if (wl && wl[0] != '\0') {
        if (auto b = open_wayland(win)) return App{std::move(b)};
        // Wayland present but failed to open: fall back to X only if available.
    }

    if (x && x[0] != '\0') {
        if (auto b = open_x11(win)) return App{std::move(b)};
    }

    // No display (or every display backend failed): an offscreen context so
    // headless paths still work.
    if (auto b = open_offscreen(win)) return App{std::move(b)};
    return toe::fail("no usable window backend (wayland/x11/offscreen all failed)");
}

} // namespace hand
