// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Unit tests for Zipper<T> as PURE ALGEBRA (T = a move-only int wrapper, no
// terminals). Proves the two structural invariants — always non-empty, focus
// always valid — hold under insertion, deletion, navigation, and index jumps.

#include "hand/zipper.hpp"

#include <cstdio>
#include <string>
#include <vector>

using hand::Zipper;

static int fails = 0;
static void ck(bool ok, const char *name) {
    if (!ok) { std::printf("FAIL %s\n", name); ++fails; }
}

// A move-only element to prove the zipper never copies its T (like Terminal).
struct MoveOnly {
    int v;
    explicit MoveOnly(int x) : v(x) {}
    MoveOnly(const MoveOnly &) = delete;
    MoveOnly &operator=(const MoveOnly &) = delete;
    MoveOnly(MoveOnly &&) noexcept = default;
    MoveOnly &operator=(MoveOnly &&) noexcept = default;
};

// Snapshot the display order into a vector<int>.
static std::vector<int> order(const Zipper<MoveOnly> &z) {
    std::vector<int> out;
    z.for_each_ordered([&](const MoveOnly &m, bool, std::size_t) { out.push_back(m.v); });
    return out;
}

int main() {
    // Birth: a zipper is always non-empty, focused on its single element.
    Zipper<MoveOnly> z{MoveOnly{0}};
    ck(z.size() == 1, "born with one element");
    ck(z.focus().v == 0, "focus is the element");
    ck(z.index() == 0, "focus index 0");

    // insert_after_focus moves focus onto the new element, order preserved.
    z.insert_after_focus(MoveOnly{1});
    z.insert_after_focus(MoveOnly{2}); // focus on 2, between... order: 0,1,2
    ck((order(z) == std::vector<int>{0, 1, 2}), "insert keeps display order");
    ck(z.focus().v == 2, "focus follows the newest insert");
    ck(z.index() == 2 && z.size() == 3, "index/size after inserts");

    // Navigation is pure rotation; edges are sticky (no wrap for left/right).
    ck(z.focus_left() && z.focus().v == 1, "focus_left moves to 1");
    ck(z.focus_left() && z.focus().v == 0, "focus_left moves to 0");
    ck(!z.focus_left(), "focus_left at the left edge is a no-op");
    ck(z.index() == 0, "at left edge index 0");
    ck((order(z) == std::vector<int>{0, 1, 2}), "order is invariant under navigation");

    // focus_index jumps in display order (clamped).
    z.focus_index(2);
    ck(z.focus().v == 2, "focus_index(2) -> element 2");
    z.focus_index(99);
    ck(z.focus().v == 2, "focus_index clamps past the end");

    // Cyclic navigation wraps.
    z.focus_last();
    z.focus_next_cyclic();
    ck(z.focus().v == 0, "next_cyclic wraps from last to first");
    z.focus_prev_cyclic();
    ck(z.focus().v == 2, "prev_cyclic wraps from first to last");

    // close_focus: focus falls to the RIGHT neighbour when present.
    z.focus_index(1); // focus on 1 (order 0,1,2)
    ck(z.close_focus(), "close middle succeeds");
    ck((order(z) == std::vector<int>{0, 2}), "middle removed");
    ck(z.focus().v == 2, "focus fell to the right neighbour");

    // close_focus at the end: focus falls LEFT.
    ck(z.close_focus(), "close last-in-order succeeds");
    ck((order(z) == std::vector<int>{0}), "only 0 remains");
    ck(z.focus().v == 0, "focus fell to the left neighbour");

    // The sole element cannot be closed — invariant 1 is structural.
    ck(!z.close_focus(), "closing the last element is refused");
    ck(z.size() == 1 && z.focus().v == 0, "zipper stays non-empty");

    // Stress: build up, then close everything but one, checking invariants hold
    // at every step.
    Zipper<MoveOnly> big{MoveOnly{100}};
    for (int i = 101; i < 110; ++i) big.insert_after_focus(MoveOnly{i});
    ck(big.size() == 10, "grew to 10");
    while (big.close_focus()) {
        ck(big.size() >= 1, "never empty mid-teardown");
        ck(big.index() < big.size(), "focus index always in range");
    }
    ck(big.size() == 1, "teardown stops at one element");

    std::printf(fails ? "%d ZIPPER TEST(S) FAILED\n" : "ALL ZIPPER TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
