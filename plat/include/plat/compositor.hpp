// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/compositor.hpp — the clean seam where plat (window + GPU context) meets a
// content renderer (hand's toe Sessions), WITHOUT plat knowing what a terminal
// is.
//
// plat owns the window, the GL context, and the frame lifecycle. It does NOT
// own a renderer — the content is drawn by a callback the host supplies. The
// callback receives a RenderPass: a capability token proving "a GL context is
// current on this thread and here is the framebuffer to draw into" — exactly
// toe's RenderContext idea, expressed platform-side so plat stays terminal-
// agnostic. The host bridges it to toe with one line:
//
//     comp.frame([&](const plat::RenderPass& pass){
//         auto rc = toe::gfx::RenderContext::adopt_current(
//                       toe::gfx::Framebuffer{pass.framebuffer});
//         active_tab.session().render(rc, px);        // toe draws the grid
//         chrome.render_overlay(rc, ...);             // chrome on top
//     });
//
// So the two layers meet at a single typed boundary (RenderPass ↔ RenderContext,
// both capability tokens), and nothing is extracted or rewritten.

#ifndef PLAT_COMPOSITOR_HPP
#define PLAT_COMPOSITOR_HPP

#include <cstdint>

#include "plat/gpu.hpp"

namespace plat {

// The per-frame capability token handed to the content callback. Its existence
// is the PROOF that: the platform's GL context is current on this thread right
// now, and `framebuffer` is the destination the platform wants drawn this
// frame (0 = the window's default; nonzero = an offscreen FBO the platform
// composites). Move-only, non-escaping — it is valid only for the duration of
// the callback plat invokes it in.
class RenderPass {
public:
    RenderPass(std::uint32_t fb, Size size) noexcept : framebuffer(fb), size_(size) {}
    RenderPass(const RenderPass &) = delete;
    RenderPass &operator=(const RenderPass &) = delete;
    RenderPass(RenderPass &&) = delete; // cannot escape the frame callback
    RenderPass &operator=(RenderPass &&) = delete;

    // The destination framebuffer for this frame (feeds toe's Framebuffer{...}).
    const std::uint32_t framebuffer;

    // Drawable size in pixels.
    [[nodiscard]] Size size() const noexcept { return size_; }

private:
    Size size_{};
};

// A Compositor is anything that can run a content callback inside a live,
// context-current frame and present it. Backends (the real windows) model this;
// the callback body is the host's toe/chrome rendering.
//
// `Draw` is the host's callback type: invocable with `const RenderPass&`.
template <class C, class Draw>
concept Compositor = requires(C c, Draw draw, Color clear) {
    // Clear to `clear`, make the context current, invoke draw(pass), present.
    { c.frame(clear, draw) } -> std::same_as<void>;
};

} // namespace plat

#endif // PLAT_COMPOSITOR_HPP
