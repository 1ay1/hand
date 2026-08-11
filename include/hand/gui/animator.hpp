// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/animator.hpp — the ONE authority on animation timing.
//
// Animations in a terminal come from several independent sources at different
// cadences: the caret GLIDE + comet trail (~60fps until it settles), a chrome
// SPINNER for a running command (~60fps), the done-attention PULSE (~60fps), a
// steady cursor BLINK (its half-period), and the visual-BELL fade (~60fps, time-
// boxed). Previously each was handled ad-hoc and the loop's wake cadence didn't
// reliably hit 60fps mid-glide, so the caret limped.
//
// Animator unifies them. Callers register which sources are LIVE (cheap boolean
// setters, lock-free), and the loop asks two questions each pass:
//
//   next_deadline_ms()  -> how long may we sleep? 16ms if any 60fps source is
//                          live; the blink half-period if only blinking; -1
//                          (block FOREVER, zero CPU) if nothing moves.
//   frame(now_ms)       -> a monotonic ~60fps frame index derived from a real
//                          clock, so animations advance at a true rate no matter
//                          how often the loop happens to wake.
//
// Pure, header-only, lock-free (a small atomic bitset). Unit-tested. The truth
// about "is the caret still gliding" comes from the renderer (published after
// each render); everything else is model-derived, so nothing here locks.

#ifndef HAND_GUI_ANIMATOR_HPP
#define HAND_GUI_ANIMATOR_HPP

#include <atomic>
#include <cstdint>

namespace hand {

// The independent animation sources. Fast sources want 60fps; Blink is slow.
enum class Anim : std::uint32_t {
    CaretGlide = 1u << 0, // caret easing to its cell (+ comet trail) [fast]
    Spinner    = 1u << 1, // a tab has a running command               [fast]
    Pulse      = 1u << 2, // a done-attention pulse                    [fast]
    Bell       = 1u << 3, // visual-bell fade                          [fast]
    Blink      = 1u << 4, // steady cursor blink                       [slow]
    Autoscroll = 1u << 5, // drag-selection past the viewport edge     [fast]
};

class Animator {
public:
    // Register/clear a source as live. Lock-free; safe to call from the render
    // path (CaretGlide/Bell are published by the renderer after each frame).
    void set(Anim a, bool live) noexcept {
        const std::uint32_t bit = static_cast<std::uint32_t>(a);
        if (live) live_.fetch_or(bit, std::memory_order_relaxed);
        else      live_.fetch_and(~bit, std::memory_order_relaxed);
    }
    [[nodiscard]] bool any_fast() const noexcept {
        constexpr std::uint32_t kFast =
            static_cast<std::uint32_t>(Anim::CaretGlide) | static_cast<std::uint32_t>(Anim::Spinner) |
            static_cast<std::uint32_t>(Anim::Pulse) | static_cast<std::uint32_t>(Anim::Bell) |
            static_cast<std::uint32_t>(Anim::Autoscroll);
        return (live_.load(std::memory_order_relaxed) & kFast) != 0;
    }
    [[nodiscard]] bool blinking() const noexcept {
        return (live_.load(std::memory_order_relaxed) &
                static_cast<std::uint32_t>(Anim::Blink)) != 0;
    }
    // Any animation at all live (fast or blink)?
    [[nodiscard]] bool active() const noexcept { return live_.load(std::memory_order_relaxed) != 0; }

    // The blink half-period (ms); 0 = blink disabled. Set from config/focus.
    void set_blink_ms(int ms) noexcept { blink_ms_ = ms; }

    // How long the loop may sleep. -1 => block forever (nothing animates).
    [[nodiscard]] int next_deadline_ms() const noexcept {
        if (any_fast()) return 16;                 // ~60fps
        if (blinking() && blink_ms_ > 0) return blink_ms_;
        return -1;                                  // idle: block forever
    }

    // Monotone ~60fps frame index from an elapsed-ms clock. Deriving the frame
    // from TIME (not a per-loop counter) makes every animation advance at a real
    // rate independent of the loop's wake frequency.
    [[nodiscard]] static std::uint32_t frame(std::int64_t elapsed_ms) noexcept {
        return static_cast<std::uint32_t>(elapsed_ms / 16);
    }

private:
    std::atomic<std::uint32_t> live_{0};
    int blink_ms_ = 530;
};

} // namespace hand

#endif // HAND_GUI_ANIMATOR_HPP
