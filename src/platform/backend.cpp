// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::run — the ONE runtime decision, then a fully-monomorphic loop.
//
// Linux ships all backends in one binary. This function makes the single
// Wayland-vs-X11-vs-offscreen choice from the environment, then enters that
// backend's own `toe::run<...>` instantiation (via run_wayland / run_x11 /
// run_offscreen, each in its backend TU). That loop inlines to the backend's
// direct calls — no vtable, no per-op branch. The choice costs one `switch` per
// process; every frame op after it is statically known.
//
// If the preferred display backend fails to open (rc < 0), fall through to the
// next, so a headless box still lands on offscreen.

#include "hand/app.hpp"

#include <cstdlib>

namespace hand {

namespace {

// The backend the environment asks for (before trying to open anything).
[[nodiscard]] Backend choose(Backend force) {
    if (force != Backend::automatic) return force;
    if (const char *h = std::getenv("TOE_HEADLESS"); h && *h) return Backend::offscreen;
    if (const char *wl = std::getenv("WAYLAND_DISPLAY"); wl && *wl) return Backend::wayland;
    if (const char *x = std::getenv("DISPLAY"); x && *x) return Backend::x11;
    return Backend::offscreen;
}

} // namespace

int run(const toe::Config &cfg, const toe::WindowConfig &win, Backend force) {
    switch (choose(force)) {
    case Backend::wayland: {
        const int rc = run_wayland(cfg, win);
        if (rc >= 0) return rc; // else fall back
        [[fallthrough]];
    }
    case Backend::x11: {
        const int rc = run_x11(cfg, win);
        if (rc >= 0) return rc;
        [[fallthrough]];
    }
    case Backend::offscreen:
    default:
        return run_offscreen(cfg, win);
    }
}

} // namespace hand
