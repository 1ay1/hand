// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/frame_gate.hpp — the render-decision core, factored out of the loop.
//
// The tabbed loop must answer two questions cheaply, every wakeup:
//   1. "does the visible image differ from what's on screen?" -> should we do a
//      GPU frame at all (present), or skip it (zero GPU cost on a static screen)?
//   2. "how long may we sleep?" -> the wait deadline: forever when nothing moves,
//      else the animation cadence.
//
// FrameGate answers (1) with a RenderKey: a single value folding everything that
// affects the pixels. If this frame's key equals the last presented one, the
// image is identical and the caller skips the whole clear/render/swap. It also
// derives the animation FRAME INDEX straight from a monotonic clock, so a
// spinner/pulse advances smoothly without round-tripping a Tick message through
// the mailbox (which was the earlier busy-loop hazard).
//
// Pure + header-only + no engine/threads: unit-testable (tests/frame_gate_test).

#ifndef HAND_GUI_FRAME_GATE_HPP
#define HAND_GUI_FRAME_GATE_HPP

#include <cstdint>

namespace hand {

// Everything that affects the on-screen pixels, folded into one comparable key.
// Two frames with equal keys are pixel-identical.
struct RenderKey {
    std::uint64_t generation = 0; // terminal content epoch
    std::int64_t scroll = 0;      // scroll offset
    std::int32_t w = 0, h = 0;    // drawable size
    std::uint32_t overlay = 0;    // 0 none / 1 help / 2 search / 3 settings
    std::uint32_t anim_frame = 0; // animation frame index (only when animating)

    [[nodiscard]] std::uint64_t fold() const noexcept {
        std::uint64_t k = 1469598103934665603ull; // FNV-1a offset
        auto mix = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ull; };
        mix(generation);
        mix(static_cast<std::uint64_t>(scroll + 1));
        mix((static_cast<std::uint64_t>(static_cast<std::uint32_t>(w)) << 32) |
            static_cast<std::uint32_t>(h));
        mix(overlay);
        mix(anim_frame);
        return k;
    }
};

// Cadence classes: the wait deadline when idle / animating fast / blinking.
enum class Cadence : int {
    Idle = -1,      // block forever
    Fast = 16,      // ~60fps: spinner / pulse / caret glide
};

class FrameGate {
public:
    // Should the caller present this frame? True unless the key is unchanged
    // since the last presented frame. Updates the stored key when it returns
    // true. (When it returns false, the caller does NO GPU work.)
    [[nodiscard]] bool should_present(const RenderKey &k) noexcept {
        const std::uint64_t f = k.fold();
        if (f == last_) return false;
        last_ = f;
        return true;
    }

    // Force the next present (e.g. on resize / focus change / first frame).
    void invalidate() noexcept { last_ = ~0ull; }

    // Monotonic ~60fps frame index from an elapsed-ms clock. Deriving the frame
    // from TIME (not a counter bumped per loop) means the spinner advances at a
    // real rate regardless of how often the loop wakes.
    [[nodiscard]] static std::uint32_t frame_index(std::int64_t elapsed_ms) noexcept {
        return static_cast<std::uint32_t>(elapsed_ms / 16);
    }

private:
    std::uint64_t last_ = ~0ull; // last presented key (~0 = force first frame)
};

} // namespace hand

#endif // HAND_GUI_FRAME_GATE_HPP
