// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Zipper<T> — a NON-EMPTY list with a distinguished focus, a.k.a. a list
// zipper. This is the algebraically-correct structure for a set of tabs: it
// makes the two invariants that a `vector<Tab> + size_t active` gets WRONG
// structurally true, checked by the type system rather than at runtime:
//
//     invariant 1  |Zipper| >= 1        — there is ALWAYS a focused element,
//                                          because the focus is a VALUE, not an
//                                          index. An empty zipper is unrepresentable.
//     invariant 2  focus is always valid — you cannot have an out-of-range focus,
//                                          because the focus IS an element.
//
// The shape is the classic three-part cursor (as in XMonad's window Stack):
//
//        left            focus         right
//     [ t0  t1  t2 ]  →   t3   ←   [ t4  t5 ]
//        (reversed)                  (in order)
//
// `left` is stored REVERSED so that "move focus left" is an O(1) push/pop at the
// back of both sides — no shifting, no reindexing. Every navigation is a pure
// rotation; close() is total and can never dangle a focus. The linear ORDER for
// display is  reverse(left) ++ [focus] ++ right, exposed via for_each_ordered.
//
// Move-only in T (Terminal is move-only), header-only, and utterly free of any
// terminal/GL/windowing dependency, so it is unit-tested as pure algebra
// (tests/zipper_test.cpp).

#ifndef HAND_ZIPPER_HPP
#define HAND_ZIPPER_HPP

#include <cstddef>
#include <utility>
#include <vector>

namespace hand {

template <class T>
class Zipper {
public:
    // The ONLY constructor: a zipper is born focused on its single element.
    // There is deliberately no default constructor — "no tabs" cannot exist.
    explicit Zipper(T focus) : focus_(std::move(focus)) {}

    Zipper(const Zipper &) = delete; // move-only, like its T (Terminal)
    Zipper &operator=(const Zipper &) = delete;
    Zipper(Zipper &&) noexcept = default;
    Zipper &operator=(Zipper &&) noexcept = default;

    // --- focus access (total: there is always a focus) ---------------------
    [[nodiscard]] T &focus() noexcept { return focus_; }
    [[nodiscard]] const T &focus() const noexcept { return focus_; }

    // --- cardinality + position (for "3/7" readouts and tab numbering) ------
    [[nodiscard]] std::size_t size() const noexcept { return left_.size() + 1 + right_.size(); }
    [[nodiscard]] std::size_t index() const noexcept { return left_.size(); } // 0-based focus pos

    // --- insertion ----------------------------------------------------------
    // Open a new element immediately AFTER the focus and move focus onto it —
    // the "open a tab next to this one and switch to it" gesture.
    void insert_after_focus(T v) {
        // Old focus slides into the left stack; the new value becomes focus.
        left_.push_back(std::move(focus_));
        focus_ = std::move(v);
    }

    // --- deletion (total; preserves invariant 1) ----------------------------
    // Remove the focused element. Focus falls to the RIGHT neighbour if one
    // exists, else the LEFT neighbour. Returns false and is a NO-OP when this is
    // the last element — the caller decides what "close the last tab" means
    // (usually: quit the app), because the zipper itself refuses to become empty.
    [[nodiscard]] bool close_focus() {
        if (!right_.empty()) {
            focus_ = std::move(right_.front());
            right_.erase(right_.begin());
            return true;
        }
        if (!left_.empty()) {
            focus_ = std::move(left_.back());
            left_.pop_back();
            return true;
        }
        return false; // sole element: refuse to empty the zipper
    }

    // --- navigation (pure rotations; O(1)) ----------------------------------
    // Move focus one step left/right. No wrap (edges are sticky); returns
    // whether focus actually moved.
    bool focus_left() {
        if (left_.empty()) return false;
        right_.insert(right_.begin(), std::move(focus_));
        focus_ = std::move(left_.back());
        left_.pop_back();
        return true;
    }
    bool focus_right() {
        if (right_.empty()) return false;
        left_.push_back(std::move(focus_));
        focus_ = std::move(right_.front());
        right_.erase(right_.begin());
        return true;
    }
    // Cyclic variants for Ctrl+Tab style cycling.
    void focus_next_cyclic() {
        if (!focus_right()) focus_first();
    }
    void focus_prev_cyclic() {
        if (!focus_left()) focus_last();
    }
    // Jump focus to the i-th element in DISPLAY order (clamped; total).
    void focus_index(std::size_t i) {
        const std::size_t n = size();
        if (n == 0) return;
        if (i >= n) i = n - 1;
        while (index() > i) focus_left();
        while (index() < i) focus_right();
    }
    void focus_first() { while (focus_left()) {} }
    void focus_last() { while (focus_right()) {} }

    // --- ordered traversal --------------------------------------------------
    // Visit every element in DISPLAY order (left-to-right), passing the element
    // and a flag for whether it is the focus. `f` is (T&, bool is_focus, size_t i).
    // `left_` is stored farthest..nearest (nearest at the back), so display
    // order is left_ FORWARD, then focus, then right_.
    template <class F>
    void for_each_ordered(F &&f) {
        std::size_t i = 0;
        for (auto &l : left_) f(l, false, i++);
        f(focus_, true, i++);
        for (auto &r : right_) f(r, false, i++);
    }
    template <class F>
    void for_each_ordered(F &&f) const {
        std::size_t i = 0;
        for (const auto &l : left_) f(l, false, i++);
        f(focus_, true, i++);
        for (const auto &r : right_) f(r, false, i++);
    }

private:
    std::vector<T> left_;  // elements LEFT of focus, stored REVERSED (nearest at back)
    T focus_;              // the always-present focused element
    std::vector<T> right_; // elements RIGHT of focus, in order (nearest at front)
};

} // namespace hand

#endif // HAND_ZIPPER_HPP
