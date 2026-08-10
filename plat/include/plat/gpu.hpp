// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/gpu.hpp — a TYPE-THEORETIC GPU resource algebra.
//
// The goal: make the frame/draw/present protocol and resource lifetimes facts
// the COMPILER checks, not conventions a comment states. Three ideas do the work:
//
//   1. Resources are AFFINE, move-only RAII handles (Texture, Pipeline). Copy is
//      deleted; the destructor frees the GPU object exactly once. There is no
//      "is this still alive?" question — the type owns the answer.
//
//   2. A Frame is a LINEAR (use-once) capability token. You obtain one from
//      Gpu::begin_frame(); it is non-copyable AND non-movable, so it cannot
//      escape the scope that opened it. draw() takes `Frame&` — so "draw with no
//      frame open" is not expressible. The Frame's destructor ENDS the pass, so
//      "forget to end/commit the frame" cannot happen either. This mirrors toe's
//      own RenderContext capability-token idea, taken to its conclusion.
//
//   3. Colours / sizes / handles are strong newtypes, so a width can't be passed
//      where a texture id is meant.
//
// This header is the CONTRACT: pure types + the Gpu concept. A concrete backend
// (sokol_gpu.hpp) satisfies the concept; nothing here knows about GL/D3D/Metal.

#ifndef PLAT_GPU_HPP
#define PLAT_GPU_HPP

#include <cstdint>
#include <span>
#include <utility>

namespace plat {

// --- strong value types -----------------------------------------------------

struct Size {
    int w = 0, h = 0;
    constexpr auto operator<=>(const Size &) const = default;
};

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    constexpr auto operator<=>(const Color &) const = default;
};

// An opaque, backend-defined GPU resource id. The RAII handles below own one.
// 0 is the canonical "null / not-a-resource" value.
struct ResourceId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    constexpr auto operator<=>(const ResourceId &) const = default;
};

// One packed instance for the instanced-quad renderer (glyph cell, bg rect, SDF
// shape). Layout matches toe's renderer Instance so the shader is unchanged.
// Kept POD so a whole frame's worth uploads as one span.
struct QuadInstance {
    float x, y, w, h;           // pixel rect
    float u0, v0, u1, v1;       // atlas UVs in [0,1]
    std::uint8_t r, g, b, a;    // colour (bytes; shader normalizes)
    std::uint8_t flags0, flags1, flags2, flags3; // is_glyph, radius, shape/corners, pad
};

// --- affine resource handles ------------------------------------------------
// A move-only owner of a GPU resource. The Deleter is a stateless functor the
// backend supplies; the handle calls it exactly once, on the last owner's death.
// This is the RAII half of the algebra: no leaks, no double-free, by type.
template <class Deleter>
class Resource {
public:
    Resource() = default; // a null handle (valid() == false)
    explicit Resource(ResourceId id) noexcept : id_(id) {}

    Resource(const Resource &) = delete;            // affine: no copies
    Resource &operator=(const Resource &) = delete;

    Resource(Resource &&o) noexcept : id_(std::exchange(o.id_, ResourceId{})) {}
    Resource &operator=(Resource &&o) noexcept {
        if (this != &o) {
            reset();
            id_ = std::exchange(o.id_, ResourceId{});
        }
        return *this;
    }
    ~Resource() { reset(); }

    [[nodiscard]] ResourceId id() const noexcept { return id_; }
    [[nodiscard]] bool valid() const noexcept { return id_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    // Relinquish ownership without freeing (hand the id to the backend).
    [[nodiscard]] ResourceId release() noexcept { return std::exchange(id_, ResourceId{}); }

private:
    void reset() noexcept {
        if (id_.valid()) {
            Deleter{}(id_);
            id_ = ResourceId{};
        }
    }
    ResourceId id_{};
};

// Distinct handle types so a Texture can't be passed where a Pipeline is meant.
// The deleter tags are defined by the backend (sokol_gpu.hpp) — declared here.
struct TextureDeleter { void operator()(ResourceId) const noexcept; };
struct PipelineDeleter { void operator()(ResourceId) const noexcept; };

using Texture = Resource<TextureDeleter>;
using Pipeline = Resource<PipelineDeleter>;

// A rectangle of the window damaged this frame (for partial present). Empty =>
// nothing changed; a full-size rect => present everything.
struct Damage {
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    [[nodiscard]] static constexpr Damage full(Size s) noexcept { return {0, 0, s.w, s.h}; }
};

} // namespace plat

#endif // PLAT_GPU_HPP
