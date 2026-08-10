// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Workspace::pump — advance every tab and refresh its derived status. This is
// the multi-terminal frame tick: it drains ALL tabs' child output (so a build
// in a background tab keeps running), then folds each tab's OSC 133 shell-
// integration signal into its TabModel to produce the auto-label + attention.

#include "hand/workspace.hpp"

#include <chrono>

namespace hand {

namespace {
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Build the pure TabSignal the TabModel consumes from a live Session. `since_ms`
// is when THIS running command was first observed (for live elapsed time); the
// caller owns that per-tab timestamp.
TabSignal read_signal(toe::Session &s, std::int64_t &running_since_ms) {
    TabSignal sig;
    sig.alive = true;
    sig.cwd = s.working_dir();
    sig.title = s.window_title();
    sig.generation = s.generation();

    if (auto cur = s.current_command(); cur && !cur->finished) {
        sig.running = true;
        sig.running_cmd = cur->command;
        if (running_since_ms == 0) running_since_ms = now_ms(); // first sight
        sig.running_ms = now_ms() - running_since_ms;
    } else {
        running_since_ms = 0; // nothing running; reset the stopwatch
    }

    if (auto last = s.last_command(); last && last->finished) {
        sig.have_last = true;
        sig.last_cmd = last->command;
        sig.last_exit = last->exit_code;
    }
    return sig;
}
} // namespace

Workspace::PumpResult Workspace::pump() {
    PumpResult res;
    const std::size_t focus_i = tabs_.index();

    tabs_.for_each_ordered([&](Tab &tab, bool is_focus, std::size_t i) {
        (void)i;
        // poll() performs the Running->Exited transition and drains a bit; then
        // pump_output() flushes whatever else is readable without blocking.
        auto view = tab.term.poll();
        toe::Session *s = view.running;
        if (s) {
            const std::uint64_t before = s->generation();
            s->pump_output();
            // A tab may have more queued than one drain budget; keep it flowing.
            // (Bounded: the run loop re-enters when the fd stays readable.)
            const std::uint64_t after = s->generation();
            if (is_focus && after != before) res.focus_output = true;

            TabSignal sig = read_signal(*s, tab.running_since_ms);
            const std::string prev_label = tab.model.label();
            const auto prev_status = tab.model.status();
            const auto prev_att = tab.model.attention();
            tab.model.update(sig, is_focus);
            if (tab.model.label() != prev_label || tab.model.status() != prev_status ||
                tab.model.attention() != prev_att) {
                res.chrome_dirty = true;
            }
        } else {
            // Exited: mark the tab dead once, so the chrome shows ∅ and the
            // caller can decide to auto-close it.
            TabSignal sig;
            sig.alive = false;
            const auto prev_status = tab.model.status();
            tab.model.update(sig, is_focus);
            if (tab.model.status() != prev_status) res.chrome_dirty = true;
            if (is_focus) res.focus_exited = true;
        }
    });

    (void)focus_i;
    return res;
}

} // namespace hand
