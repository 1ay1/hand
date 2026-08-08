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
#include "hand/platform/posix_pty.hpp"
#if defined(__APPLE__)
#include "hand/platform/fonts.hpp"
#endif

#include <cstdio>
#include <cstdlib>

namespace hand {

namespace {

// The backend the environment asks for (before trying to open anything).
[[nodiscard]] Backend choose(Backend force) {
    if (force != Backend::automatic) return force;
    if (const char *h = std::getenv("TOE_HEADLESS"); h && *h) return Backend::offscreen;
#if defined(__APPLE__)
    // macOS has exactly one native window system.
    return Backend::cocoa;
#else
    if (const char *wl = std::getenv("WAYLAND_DISPLAY"); wl && *wl) return Backend::wayland;
    if (const char *x = std::getenv("DISPLAY"); x && *x) return Backend::x11;
    return Backend::offscreen;
#endif
}

} // namespace

int run(const toe::Config &cfg_in, const toe::WindowConfig &win, Backend force) {
    // Process creation is the HOST's job: forkpty the child here and hand toe an
    // adopt-able master fd. The engine never forks (see posix_pty.hpp).
    toe::Config cfg = cfg_in;
    auto fd = spawn_pty(SpawnCommand{});
    if (!fd) {
        std::fprintf(stderr, "hand: %s\n", fd.error().message.c_str());
        return 1;
    }
    cfg.source = *fd;

#if defined(__APPLE__)
    // Font is host policy on macOS. Match Terminal.app's defaults: SF Mono at
    // ~11pt, which on a 2x Retina display is ~22 device pixels. resolve_font_file
    // already prefers SF Mono, so we only nudge the size — and only when the
    // user hasn't set their own (still at toe's cross-platform default of 18).
    if (cfg.font_pixel_size == 18) cfg.font_pixel_size = 22;
    // Font discovery is host policy: resolve a concrete macOS face and hand it
    // to toe via font_file, so the engine needs no macOS font-path branch.
    if (cfg.font_file.empty()) {
        std::string f = resolve_font_file(cfg.font_family);
        if (!f.empty()) cfg.font_file = std::move(f);
    }
#endif

    switch (choose(force)) {
#if defined(__APPLE__)
    case Backend::cocoa:
    default:
        return run_cocoa(cfg, win);
#else
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
#endif
    }
}

} // namespace hand
