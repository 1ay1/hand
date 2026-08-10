// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/sokol_gpu.hpp — a concrete Gpu backed by sokol_gfx (GL / D3D11 / Metal).
//
// This is the FIRST real implementation of the plat::Gpu concept: it drives
// actual hardware through sokol, while presenting the type-theoretic surface
// (owning Texture handles, the linear Frame). The QuadInstance layout matches
// the shader below; a null texture id draws solid/rounded rects (bg, cursor,
// selection), a valid one samples the atlas (glyphs).
//
// The GL context + sg_setup() are the HOST's responsibility (SokolGpu assumes a
// context is current when constructed and a pass target is set per frame) — the
// same capability-token discipline as toe's RenderContext. SokolGpu::attach()
// takes the pass action / swapchain the host hands it each frame.
//
// Impl lives in sokol_gpu.cpp (the one TU that includes sokol_gfx.h).

#ifndef PLAT_SOKOL_GPU_HPP
#define PLAT_SOKOL_GPU_HPP

#include <cstdint>
#include <span>

#include "plat/frame.hpp"
#include "plat/gpu.hpp"

namespace plat {

// The pass target the host provides each frame: either the default swapchain
// (0) or an offscreen FBO (headless tests). Colour-only; clear colour comes
// from begin_frame().
struct PassTarget {
    std::uint32_t fbo = 0; // GL framebuffer name (0 = default/window)
    Size size{};
};

class SokolGpu {
public:
    // Construct with a context already current + sg_setup() done by the host.
    // Lazily builds the pipeline/buffers on first frame.
    SokolGpu() = default;
    ~SokolGpu();
    SokolGpu(const SokolGpu &) = delete;
    SokolGpu &operator=(const SokolGpu &) = delete;
    SokolGpu(SokolGpu &&) noexcept;
    SokolGpu &operator=(SokolGpu &&) noexcept;

    // The host sets the frame's target (window swapchain or offscreen FBO)
    // before begin_frame(). size() reports it.
    void set_target(PassTarget t) noexcept { target_ = t; }
    [[nodiscard]] Size size() const noexcept { return target_.size; }

    // --- Gpu concept: textures -------------------------------------------
    Texture create_texture(Size s, std::span<const std::uint8_t> rgba);
    void update_texture(ResourceId id, Size s, std::span<const std::uint8_t> rgba);

    // --- Gpu concept: frames ---------------------------------------------
    // FrameOps the linear Frame drives. Holds a back-pointer to the device.
    struct Ops {
        SokolGpu *dev = nullptr;
        void draw(std::span<const QuadInstance> instances, ResourceId tex);
        void finish();
    };
    // Begin a pass on the current target, clearing to `clear`. Returns the
    // linear Frame; draw through it, and its dtor ends the pass (the host
    // commits/presents around the loop, or Ops::finish commits here).
    Frame<Ops> begin_frame(Color clear);

private:
    void ensure_pipeline();

    PassTarget target_{};
    std::uint32_t pip_ = 0;       // sg_pipeline id
    std::uint32_t shd_ = 0;       // sg_shader id
    std::uint32_t quad_vbuf_ = 0; // unit-quad vertices (per-vertex)
    std::uint32_t inst_vbuf_ = 0; // streaming instances (per-instance)
    std::uint32_t smp_ = 0;       // sampler
    std::uint32_t white_ = 0;     // 1x1 white image (for null-texture draws)
    std::uint32_t white_view_ = 0;
    bool pass_open_ = false;
};

static_assert(FrameOps<SokolGpu::Ops>, "SokolGpu::Ops must model FrameOps");
static_assert(Gpu<SokolGpu>, "SokolGpu must model the Gpu concept");

} // namespace plat

#endif // PLAT_SOKOL_GPU_HPP
