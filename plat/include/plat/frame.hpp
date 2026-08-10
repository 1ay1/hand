// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/frame.hpp — the linear FRAME token and the Gpu capability concept.
//
// A Frame is the proof that "a render pass is open on the current context right
// now". It is:
//   * non-copyable  — a capability is not duplicable,
//   * non-movable   — so it CANNOT escape the scope that opened it (no dangling
//                     "draw into last frame" bugs; it dies where it was born),
//   * RAII-closing  — its destructor ends the pass + commits, so "forgot to end
//                     the frame" is unrepresentable.
//
// The only way to get one is Gpu::begin_frame(); the only way to draw is to
// pass the live Frame& to draw(). Together these make the frame protocol
//     begin → draw* → (scope end: auto end+present)
// a THEOREM the type checker proves, not a rule the programmer remembers.

#ifndef PLAT_FRAME_HPP
#define PLAT_FRAME_HPP

#include <concepts>
#include <span>

#include "plat/gpu.hpp"

namespace plat {

// The backend implements FrameOps; Frame is a thin RAII wrapper that ends the
// pass on destruction. FrameOps is a stateless-ish handle the backend hands to
// the Frame (it holds a back-pointer to the device + the acquired drawable).
// Kept as a concept so any backend can supply its own frame mechanics.
template <class T>
concept FrameOps = requires(T ops, std::span<const QuadInstance> instances, ResourceId tex) {
    { ops.draw(instances, tex) } -> std::same_as<void>; // issue one instanced batch
    { ops.finish() } -> std::same_as<void>;             // end pass + commit
};

// Frame<Ops> — the linear token. Constructed by the Gpu (which begins the pass),
// consumed at scope end (which finishes it). Not movable: it lives and dies in
// one scope, so it can never be stored, returned, or aliased.
template <FrameOps Ops>
class Frame {
public:
    explicit Frame(Ops ops) noexcept : ops_(ops) {}

    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
    Frame(Frame &&) = delete;      // LINEAR: cannot escape its opening scope
    Frame &operator=(Frame &&) = delete;

    ~Frame() {
        if (!finished_) ops_.finish();
    }

    // Draw one instanced batch of quads sampling `tex` (a null tex = solid /
    // bg-only). The ONLY drawing entry point, and it requires a live Frame.
    void draw(std::span<const QuadInstance> instances, ResourceId tex = {}) {
        ops_.draw(instances, tex);
    }

    // Optional explicit finish (otherwise the destructor does it). Idempotent.
    void finish() {
        if (!finished_) {
            ops_.finish();
            finished_ = true;
        }
    }

private:
    Ops ops_;
    bool finished_ = false;
};

// --- the Gpu capability concept ---------------------------------------------
// A type models Gpu iff it can size itself, manage textures, and open frames.
// begin_frame() returns a Frame<Ops> BY VALUE into the caller's scope — the
// linear token. Because Frame is non-movable, callers must bind it to a named
// local (guaranteed copy elision from the prvalue return makes this legal and
// zero-cost) and use it in place:
//
//     { auto f = gpu.begin_frame(clear);  f.draw(cells, atlas);  }  // auto-present
//
template <class G>
concept Gpu = requires(G g, const G cg, Color clear, Size sz, ResourceId tex,
                       std::span<const std::uint8_t> pixels) {
    { cg.size() } -> std::same_as<Size>;

    // Upload an RGBA8 texture of `sz`; returns an owning Texture handle.
    { g.create_texture(sz, pixels) } -> std::same_as<Texture>;
    // Replace a sub-region of an existing texture (font-atlas growth).
    { g.update_texture(tex, sz, pixels) } -> std::same_as<void>;

    // Begin a frame that clears to `clear`. The returned object models FrameOps
    // via Frame<...>; we check it exposes begin_frame at all (the concrete
    // Frame type is backend-specific, so we don't name it here).
    { g.begin_frame(clear) };
};

} // namespace plat

#endif // PLAT_FRAME_HPP
