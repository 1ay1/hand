// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Unit test for FrameGate: the render-decision core. Proves the frame-skip is
// correct (identical keys skip, any change presents) and the time-derived frame
// index is monotone — with zero engine/threads.

#include "hand/gui/frame_gate.hpp"

#include <cstdio>

using hand::FrameGate;
using hand::RenderKey;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

int main() {
    FrameGate g;
    RenderKey k;
    k.generation = 10; k.scroll = 0; k.w = 800; k.h = 600; k.overlay = 0;

    // First frame always presents (gate starts invalidated).
    ck(g.should_present(k), "first frame presents");
    // Same key -> skip (pixel-identical).
    ck(!g.should_present(k), "identical key skips (no GPU work)");
    ck(!g.should_present(k), "still skips while unchanged");

    // Any pixel-affecting change forces a present, exactly once per change.
    { auto n = k; n.generation = 11; ck(g.should_present(n), "new generation presents"); k = n; }
    ck(!g.should_present(k), "then skips again");
    { auto n = k; n.scroll = 5; ck(g.should_present(n), "scroll change presents"); k = n; }
    { auto n = k; n.w = 1024; ck(g.should_present(n), "resize presents"); k = n; }
    { auto n = k; n.overlay = 2; ck(g.should_present(n), "overlay open presents"); k = n; }
    { auto n = k; n.anim_frame = 7; ck(g.should_present(n), "animation frame presents"); k = n; }
    // Interaction revision (selection drag / scroll / hover) forces a present
    // even when generation() is unchanged — the fix for laggy text selection.
    { auto n = k; n.interaction = 1; ck(g.should_present(n), "interaction bump presents"); k = n; }
    { auto n = k; n.interaction = 2; ck(g.should_present(n), "next interaction bump presents"); k = n; }
    ck(!g.should_present(k), "settled again -> skip");

    // invalidate() forces the next present even with an unchanged key.
    g.invalidate();
    ck(g.should_present(k), "invalidate forces a present");

    // Distinct keys don't collide (hash sanity over a spread of values).
    {
        FrameGate g2;
        int presented = 0;
        for (std::uint64_t gen = 0; gen < 1000; ++gen) {
            RenderKey kk; kk.generation = gen; kk.w = 800; kk.h = 600;
            if (g2.should_present(kk)) ++presented;
        }
        ck(presented == 1000, "1000 distinct generations all present (no false skips)");
    }

    // Time-derived frame index: monotone, ~60fps.
    ck(FrameGate::frame_index(0) == 0, "t=0 -> frame 0");
    ck(FrameGate::frame_index(16) == 1, "t=16ms -> frame 1");
    ck(FrameGate::frame_index(160) == 10, "t=160ms -> frame 10");
    ck(FrameGate::frame_index(1000) == 62, "t=1s -> ~62 frames (60fps)");

    std::printf(fails ? "%d FRAME GATE TEST(S) FAILED\n" : "ALL FRAME GATE TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
