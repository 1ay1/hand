// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Backend selection: the ONE place that knows all three backends exist. It
// picks a backend from the environment (or an explicit request) and calls that
// backend's monomorphic entry — run_wayland / run_x11 / run_offscreen — each
// defined in its own TU where App<ConcreteSurface> is instantiated. Past this
// function everything is compile-time dispatched; no surface type leaks here.

#include "hand/platform/backend.hpp"

#include <cstdlib>

namespace hand::platform {

// Defined in the per-backend translation units.
int run_wayland(const toe::Config &cfg, std::string_view title, PixelSize initial);
int run_x11(const toe::Config &cfg, std::string_view title, PixelSize initial);
int run_offscreen(const toe::Config &cfg, PixelSize initial);

int run(const toe::Config &cfg, std::string_view title, PixelSize initial, Backend backend) {
    switch (backend) {
    case Backend::wayland:  return run_wayland(cfg, title, initial);
    case Backend::x11:      return run_x11(cfg, title, initial);
    case Backend::offscreen:return run_offscreen(cfg, initial);
    case Backend::automatic:break;
    }

    // Automatic: honor TOE_HEADLESS, then Wayland, then X11, then offscreen.
    const char *wl = std::getenv("WAYLAND_DISPLAY");
    const char *x = std::getenv("DISPLAY");
    const char *headless = std::getenv("TOE_HEADLESS");

    if (headless && headless[0] != '\0') {
        return run_offscreen(cfg, initial);
    }

    if (wl && wl[0] != '\0') {
        const int rc = run_wayland(cfg, title, initial);
        // A negative rc here means Wayland startup failed; fall back to X only
        // if X is available, otherwise surface the failure.
        if (rc >= 0 || !(x && x[0] != '\0')) return rc;
    }

    if (x && x[0] != '\0') {
        return run_x11(cfg, title, initial);
    }

    // No display at all: an offscreen context so headless paths still work.
    return run_offscreen(cfg, initial);
}

} // namespace hand::platform
