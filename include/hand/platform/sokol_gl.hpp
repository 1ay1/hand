// SPDX-License-Identifier: LGPL-2.0-or-later
//
// sokol_gl — the tiny bit of glue that turns "an EGL/GL context is current on
// this thread" into a live sokol_gfx frame, shared by every Linux GL backend
// (Wayland, X11, offscreen).
//
// toe's renderer is 100% sokol_gfx now: it issues only sg_* calls into whatever
// pass the host has begun. On Apple the host is Metal (cocoa.mm); on Linux it's
// sokol's GLCORE backend, which calls the real libGL entry points directly. The
// host's contract is exactly three things:
//
//   1. setup()        — once, after the GL context is first made current.
//   2. begin_frame()  — build the default GL swapchain, begin a pass (clearing
//                       to the terminal's background), leaving it open.
//   3. end_frame()    — end the pass and commit; the caller then swaps buffers.
//
// Because sokol GLCORE draws into the currently-bound GL framebuffer (the
// swapchain's gl.framebuffer = 0 = the default FBO of the current draw
// surface), the host does NOT hand sokol a device/texture the way Metal does —
// making the context current is the whole binding step.

#ifndef HAND_PLATFORM_SOKOL_GL_HPP
#define HAND_PLATFORM_SOKOL_GL_HPP

#include <cstdint>

#include "toe/core/types.hpp" // toe::PixelSize

// The consumer TUs (Wayland/X11/offscreen) include the sokol HEADER (no IMPL;
// SOKOL_IMPL lives in toe's sokol_impl.c). The backend define must match that
// TU so the struct layouts/enums agree — Linux is the GLCORE backend.
//
// sokol only pulls <GL/gl.h> inside its IMPL section, so in header mode we
// include it ourselves (with GL_GLEXT_PROTOTYPES) — this gives the host TUs the
// real gl* prototypes (glFlush, glGenFramebuffers, ...) resolved against libGL.
#define SOKOL_GLCORE
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_log.h"

namespace hand::platform::sokolgl {

// Call once, with a GL context already current. Idempotent via the caller's
// own guard flag (sokol asserts if set up twice).
inline void setup() {
    sg_desc desc{};
    desc.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE; // colour-only
    desc.environment.defaults.sample_count = 1;
    desc.logger.func = slog_func; // surface sokol validation messages on stderr
    sg_setup(&desc);
}

// Build a swapchain that targets an explicit GL framebuffer object (0 = the
// default FBO of the current draw surface). Used by the windowed backends (fbo
// 0) and the headless render tests (their own texture FBO).
inline sg_swapchain swapchain_for(toe::PixelSize px, std::uint32_t fbo) {
    sg_swapchain sc{};
    sc.width = px.w;
    sc.height = px.h;
    sc.sample_count = 1;
    sc.color_format = SG_PIXELFORMAT_RGBA8;
    sc.depth_format = SG_PIXELFORMAT_NONE;
    sc.gl.framebuffer = fbo;
    return sc;
}

inline sg_swapchain default_swapchain(toe::PixelSize px) { return swapchain_for(px, 0); }

// Begin a pass into an explicit FBO, clearing to (r,g,b). Leaves it OPEN.
inline void begin_frame_fbo(std::uint32_t fbo, toe::PixelSize px, std::uint8_t r,
                            std::uint8_t g, std::uint8_t b) {
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    pass.swapchain = swapchain_for(px, fbo);
    sg_begin_pass(&pass);
}

// Begin the swapchain pass, clearing to (r,g,b). Leaves the pass OPEN so the
// terminal renderer can draw into it; the host calls end_frame() after.
inline void begin_frame(toe::PixelSize px, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    begin_frame_fbo(0, px, r, g, b);
}

inline void end_frame() {
    sg_end_pass();
    sg_commit();
}

} // namespace hand::platform::sokolgl

#endif // HAND_PLATFORM_SOKOL_GL_HPP
