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

#include <algorithm>
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
        // Genuinely 60fps sources: a caret glide, a spinning "running" glyph, a
        // visual-bell fade, drag autoscroll. NOT Pulse — the done-attention
        // pulse is a SLOW blink (see next_deadline_ms), so it must not peg the
        // loop at 60fps while a background tab's attention stays latched.
        constexpr std::uint32_t kFast =
            static_cast<std::uint32_t>(Anim::CaretGlide) | static_cast<std::uint32_t>(Anim::Spinner) |
            static_cast<std::uint32_t>(Anim::Bell) | static_cast<std::uint32_t>(Anim::Autoscroll);
        return (live_.load(std::memory_order_relaxed) & kFast) != 0;
    }
    [[nodiscard]] bool blinking() const noexcept {
        // The steady cursor blink AND the done-attention pulse are both slow
        // blinks driven off the frame clock — neither needs 60fps.
        constexpr std::uint32_t kSlow =
            static_cast<std::uint32_t>(Anim::Blink) | static_cast<std::uint32_t>(Anim::Pulse);
        return (live_.load(std::memory_order_relaxed) & kSlow) != 0;
    }
    // Any animation at all live (fast or blink)?
    [[nodiscard]] bool active() const noexcept { return live_.load(std::memory_order_relaxed) != 0; }

    // The blink half-period (ms); 0 = blink disabled. Set from config/focus.
    void set_blink_ms(int ms) noexcept { blink_ms_ = ms; }

    // How long the loop may sleep. -1 => block forever (nothing animates).
    [[nodiscard]] int next_deadline_ms() const noexcept {
        if (any_fast()) return 16;                 // ~60fps
        // A slow blink (cursor blink or done-attention pulse). The pulse toggles
        // every 8 frames (~128ms); waking at ~64ms is comfortably fast enough
        // and costs a fraction of the 60fps path. The cursor blink uses its
        // configured half-period.
        if (blinking()) {
            const bool pulse = (live_.load(std::memory_order_relaxed) &
                                static_cast<std::uint32_t>(Anim::Pulse)) != 0;
            const int blink = (blink_ms_ > 0) ? blink_ms_ : 1 << 30;
            const int pulse_ms = pulse ? 64 : 1 << 30;
            const int d = std::min(blink, pulse_ms);
            return d < (1 << 30) ? d : -1;
        }
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
