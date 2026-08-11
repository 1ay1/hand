// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Unit test for Animator: the single animation-timing authority. Proves the
// deadline aggregation (fast sources -> 16ms, blink-only -> half-period, idle ->
// block forever) and the monotone time-derived frame index.

#include "hand/gui/animator.hpp"

#include <cstdio>
#include <initializer_list>

using hand::Anim;
using hand::Animator;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

int main() {
    Animator a;
    a.set_blink_ms(530);

    // Nothing live -> block forever, not active.
    ck(!a.active(), "starts inactive");
    ck(a.next_deadline_ms() == -1, "idle: block forever");

    // A fast source -> 16ms.
    a.set(Anim::CaretGlide, true);
    ck(a.active() && a.any_fast(), "caret glide is a fast animation");
    ck(a.next_deadline_ms() == 16, "fast source -> 60fps deadline");

    // Clearing it drops back to idle.
    a.set(Anim::CaretGlide, false);
    ck(a.next_deadline_ms() == -1, "cleared -> idle again");

    // Blink alone -> half-period.
    a.set(Anim::Blink, true);
    ck(a.blinking() && !a.any_fast(), "blink is slow, not fast");
    ck(a.next_deadline_ms() == 530, "blink-only -> half-period deadline");

    // A fast source dominates the blink.
    a.set(Anim::Spinner, true);
    ck(a.next_deadline_ms() == 16, "fast source dominates blink");
    a.set(Anim::Spinner, false);
    ck(a.next_deadline_ms() == 530, "spinner gone -> back to blink cadence");

    // Blink disabled (ms=0) + nothing fast -> block forever.
    a.set_blink_ms(0);
    ck(a.next_deadline_ms() == -1, "blink disabled + idle -> forever");
    a.set(Anim::Blink, false);

    // Each fast source independently forces 60fps.
    for (Anim s : {Anim::CaretGlide, Anim::Spinner, Anim::Pulse, Anim::Bell}) {
        a.set(s, true);
        ck(a.next_deadline_ms() == 16, "each fast source -> 16ms");
        a.set(s, false);
    }
    ck(a.next_deadline_ms() == -1, "all cleared -> idle");

    // Time-derived frame index: monotone, ~60fps.
    ck(Animator::frame(0) == 0, "t=0 -> frame 0");
    ck(Animator::frame(16) == 1, "t=16ms -> frame 1");
    ck(Animator::frame(1000) == 62, "t=1s -> ~62 frames");
    ck(Animator::frame(100) <= Animator::frame(200), "monotone in time");

    std::printf(fails ? "%d ANIMATOR TEST(S) FAILED\n" : "ALL ANIMATOR TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
