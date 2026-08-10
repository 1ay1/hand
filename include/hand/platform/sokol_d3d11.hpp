// SPDX-License-Identifier: LGPL-2.0-or-later
//
// sokol_d3d11 — the Windows counterpart of sokol_gl.hpp: the glue that turns a
// live D3D11 device + DXGI swapchain into a sokol_gfx frame.
//
// On Linux the host binds by making a GL context current, so sokol needs no
// objects from it. D3D11 has no ambient "current context": sokol must be handed
// the device, the device context, and the render/depth VIEWS for the frame. So
// this owns slightly more than its GL sibling, but the host contract is the
// same three calls:
//
//   1. setup(device, context)  — once, after the device exists.
//   2. begin_frame()           — build the swapchain descriptor, begin a pass
//                                (clearing to the terminal background), leave
//                                it open.
//   3. end_frame()             — end the pass and commit; the host Presents.
//
// sokol asks for the views through CALLBACKS rather than storing them, because
// a resize destroys and recreates them; the callbacks always return the current
// ones, so a resize needs no sokol-side teardown.

#ifndef HAND_PLATFORM_SOKOL_D3D11_HPP
#define HAND_PLATFORM_SOKOL_D3D11_HPP

#if defined(_WIN32)

#include <cstdint>

#include "toe/core/types.hpp" // toe::PixelSize

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

// The consumer TU includes the sokol HEADER (no IMPL — that lives in toe).
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_log.h"

namespace hand::platform::sokold3d {

// The views for the CURRENT frame. The host updates these on resize; sokol
// reads them through the callbacks below at pass-begin time.
struct Views {
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
};

inline Views &views() noexcept {
    static Views v;
    return v;
}

inline void setup(ID3D11Device *device, ID3D11DeviceContext *ctx) {
    sg_desc desc{};
    desc.environment.defaults.color_format = SG_PIXELFORMAT_BGRA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    desc.environment.defaults.sample_count = 1;
    desc.environment.d3d11.device = device;
    desc.environment.d3d11.device_context = ctx;
    desc.logger.func = slog_func;
    sg_setup(&desc);
}

inline sg_swapchain swapchain_for(toe::PixelSize px) {
    sg_swapchain sc{};
    sc.width = px.w;
    sc.height = px.h;
    sc.sample_count = 1;
    sc.color_format = SG_PIXELFORMAT_BGRA8;
    sc.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    sc.d3d11.render_view = views().rtv;
    sc.d3d11.resolve_view = nullptr; // no MSAA: render view is the resolve target
    sc.d3d11.depth_stencil_view = views().dsv;
    return sc;
}

// Begin the swapchain pass, clearing to (r,g,b) at alpha `a` (window opacity).
// Leaves the pass OPEN so the terminal renderer can draw into it.
inline void begin_frame(toe::PixelSize px, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                        float a = 1.0f) {
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {r / 255.0f, g / 255.0f, b / 255.0f, a};
    pass.swapchain = swapchain_for(px);
    sg_begin_pass(&pass);
}

inline void end_frame() {
    sg_end_pass();
    sg_commit();
}

} // namespace hand::platform::sokold3d

#endif // _WIN32
#endif // HAND_PLATFORM_SOKOL_D3D11_HPP
