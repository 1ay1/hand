// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Workspace — the set of open tabs, as a Zipper of Tabs. This is where hand's
// type-theoretic tab model meets the engine: it binds the pure Zipper<Tab>
// container to real toe::Terminals + their TabModel status, and it preserves
// the engine's own discipline — "you can only touch a LIVE terminal, proven by
// the type system" — up into the multi-terminal layer.
//
// Design pillars:
//
//   1. Non-empty by construction. A Workspace always has >= 1 tab and always a
//      valid focus, because it IS a Zipper (see zipper.hpp). There is no
//      "no active tab" state to guard against.
//
//   2. Liveness is a capability, not a flag. Each Terminal is already a
//      Running|Exited sum type reachable only through poll(). Workspace keeps
//      that: with_live_focus(f) invokes `f(Session&, TabModel&)` ONLY when the
//      focused terminal is running, so "render/route input to a dead tab" is a
//      compile-time impossibility inherited from the engine, not a runtime if.
//
//   3. Status is derived, never stored by the user. pump() advances every tab's
//      output (so background jobs progress) and folds each tab's shell-integration
//      signal into its TabModel — the auto-label + done-attention brain.
//
// Move-only (Terminal is), header-light. The pump/status wiring that needs the
// full toe::Session API lives in workspace.cpp.

#ifndef HAND_WORKSPACE_HPP
#define HAND_WORKSPACE_HPP

#include <cstdint>
#include <optional>
#include <utility>

#include "hand/tab_model.hpp"
#include "hand/zipper.hpp"
#include "toe/terminal.hpp"

namespace hand {

// One tab: a terminal + its derived status. The Terminal owns the child/PTY/
// grid; the TabModel is pure derived state refreshed each pump().
struct Tab {
    toe::Terminal term;
    TabModel model{};
    std::uint64_t id{0};             // stable identity for the chrome (never reused)
    std::int64_t running_since_ms{0}; // when the current running cmd was first seen (0 = none)

    explicit Tab(toe::Terminal t, std::uint64_t tab_id) : term(std::move(t)), id(tab_id) {}
    Tab(Tab &&) noexcept = default;
    Tab &operator=(Tab &&) noexcept = default;
    Tab(const Tab &) = delete;
    Tab &operator=(const Tab &) = delete;
};

class Workspace {
public:
    // Born from the first terminal — mirrors Zipper's non-empty invariant.
    // next_id_ starts at 1; the first tab takes id 1, so it must be bumped
    // explicitly here (the member is declared after tabs_, so we can't rely on
    // its default in tabs_'s initializer).
    explicit Workspace(toe::Terminal first) : tabs_(Tab{std::move(first), 1}), next_id_(2) {}

    Workspace(Workspace &&) noexcept = default;
    Workspace &operator=(Workspace &&) noexcept = default;
    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;

    // --- shape --------------------------------------------------------------
    [[nodiscard]] std::size_t count() const noexcept { return tabs_.size(); }
    [[nodiscard]] std::size_t active_index() const noexcept { return tabs_.index(); }
    [[nodiscard]] Tab &focus() noexcept { return tabs_.focus(); }
    [[nodiscard]] const Tab &focus() const noexcept { return tabs_.focus(); }

    // --- mutation -----------------------------------------------------------
    // Open a terminal in a new tab next to the focus and switch to it.
    void open(toe::Terminal t) { tabs_.insert_after_focus(Tab{std::move(t), next_id_++}); }
    // Close the focused tab. Returns false when it's the LAST tab (the caller
    // then usually quits the app) — the Workspace refuses to become empty.
    [[nodiscard]] bool close_focus() { return tabs_.close_focus(); }

    // --- navigation ---------------------------------------------------------
    void focus_next() { tabs_.focus_next_cyclic(); }
    void focus_prev() { tabs_.focus_prev_cyclic(); }
    void focus_index(std::size_t i) { tabs_.focus_index(i); }

    // --- the frame tick -----------------------------------------------------
    // Advance EVERY tab's child output (so background jobs run) and refresh each
    // tab's derived status/label/attention. Call once per frame. Returns true if
    // any tab's status/label changed (the chrome should repaint) OR the focused
    // tab produced output (the grid should repaint). Implemented in workspace.cpp
    // because it needs the full Session API.
    struct PumpResult {
        bool chrome_dirty = false; // some tab's label/status/attention changed
        bool focus_output = false; // the focused tab drained new child bytes
        bool focus_exited = false; // the focused terminal transitioned to Exited
    };
    PumpResult pump();

    // Run `f(toe::Session&, TabModel&)` iff the focused terminal is LIVE. This
    // is the sole gate for render / input-routing on the active tab: a dead tab
    // is simply never passed to `f`, so illegal operations can't be expressed.
    template <class F>
    bool with_live_focus(F &&f) {
        if (auto *s = tabs_.focus().term.poll().running) {
            f(*s, tabs_.focus().model);
            return true;
        }
        return false;
    }

    // Visit every tab in display order for the chrome: f(const Tab&, bool
    // is_focus, size_t i). Pure read; no engine calls.
    template <class F>
    void for_each_tab(F &&f) const {
        tabs_.for_each_ordered(
            [&](const Tab &t, bool is_focus, std::size_t i) { f(t, is_focus, i); });
    }

private:
    Zipper<Tab> tabs_;
    std::uint64_t next_id_ = 1;
};

} // namespace hand

#endif // HAND_WORKSPACE_HPP
