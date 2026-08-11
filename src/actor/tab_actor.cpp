// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/actor/tab_actor.cpp — the per-tab blocking loop.
//
// The actor thread owns its Terminal end to end: it blocks on the child PTY fd
// OR its inbound-command wakeup fd, drains input, pumps child output into its
// own grid (under the per-tab render_lock, the only object the GUI also
// touches), and posts CHANGE-ONLY status messages to the GUI mailbox. It never
// renders — the single GL context lives on the GUI thread.

#include "hand/actor/tab_actor.hpp"

#include <chrono>
#include <poll.h>

namespace hand {

namespace {
std::int64_t now_ms_actor() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void TabActor::run() {
    for (;;) {
        if (stopping_.load()) break;

        // --- 1. drain inbound commands from the GUI --------------------------
        bool stop = false;
        inbox_.drain([&](TabCmd c) {
            std::visit(
                [&](auto &&cmd) {
                    using T = std::decay_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<T, TabInput>) {
                        std::lock_guard lk(render_lock_);
                        if (auto *s = term_.poll().running) s->send_text(cmd.bytes);
                    } else if constexpr (std::is_same_v<T, TabKey>) {
                        std::lock_guard lk(render_lock_);
                        if (auto *s = term_.poll().running) s->send_key(cmd.key);
                    } else if constexpr (std::is_same_v<T, TabStop>) {
                        stop = true;
                    }
                },
                std::move(c));
        });
        if (stop || stopping_.load()) break;

        // --- 2. pump child output into our grid ------------------------------
        // Drain the PTY to EMPTY in this wakeup (pump_output only consumes a
        // budget per call; a flooding child leaves more pending). Looping here
        // means one wakeup absorbs the whole backlog, then we block — instead of
        // returning to poll() with the fd still readable and spinning.
        int pty_fd = -1;
        bool exited = false;
        int exit_code = 0;
        {
            std::lock_guard lk(render_lock_);
            auto p = term_.poll();
            if (p.exited) { exited = true; exit_code = p.exited->code; }
            else pty_fd = p.running->pty_fd();
        }
        if (exited) {
            to_gui_.post(TabExited{id_, exit_code});
            break;
        }
        // Bounded drain: consume all readable output, but re-acquire the lock
        // PER PASS and yield between passes so the GUI thread (which also wants
        // render_lock, to render + read animation state) is never starved by a
        // flooding child. A hard pass cap bounds one wakeup's work.
        for (int passes = 0; passes < 256; ++passes) {
            bool more = false;
            {
                std::lock_guard lk(render_lock_);
                auto p = term_.poll();
                if (!p.running) break;
                p.running->pump_output();
                more = p.running->output_pending();
            }
            if (!more) break;
        }

        // --- 3. post CHANGE-ONLY status, RATE-LIMITED (latency-preserving) --
        // A chatty child (an agent, a build) bumps the grid many times per
        // millisecond; notifying the GUI on every bump would peg a core. So we
        // coalesce to at most ~1 output notification per ~8ms (120Hz) — tight
        // enough that echoed keystrokes feel instant, loose enough to cap a
        // flooding child's wakeups.
        //
        // CRUCIAL for INPUT LATENCY: when a change lands inside the window we
        // must NOT drop it (that made every echoed keystroke wait a full frame,
        // the "keys don't respond immediately" bug). Instead we mark it PENDING
        // and, in step 4, wake at the EXACT remaining time to the deadline so
        // it's posted ~immediately after the window, not a flat frame late.
        constexpr std::int64_t kCoalesceMs = 8;
        const std::int64_t now = now_ms_actor();
        const std::int64_t since = now - last_notify_ms_;
        const bool due = since >= kCoalesceMs;
        // publish_status returns true when a grid change was COALESCED (held
        // back this pass); one lock acquisition does both the post and the peek.
        const bool pending_change = publish_status(/*post_output=*/due);
        if (due) {
            last_notify_ms_ = now;
            pending_output_ = false;
        } else if (pending_change) {
            pending_output_ = true; // deferred; step 4 wakes us at the deadline
        }

        // --- 4. block until the PTY or an inbound command is ready -----------
        struct pollfd fds[2];
        int n = 0;
        if (pty_fd >= 0) {
            fds[n].fd = pty_fd;
            fds[n].events = POLLIN;
            ++n;
        }
        const int wfd = inbox_.wait_fd();
        if (wfd >= 0) {
            fds[n].fd = wfd;
            fds[n].events = POLLIN;
            ++n;
        }
        if (n == 0) break; // nothing to wait on: bail rather than spin
        // Timeout policy:
        //  * a change is PENDING (coalesced within the window) -> wake at the
        //    EXACT remaining ms to the 16ms deadline so the deferred echo is
        //    flushed with minimal latency (typically 1-15ms, not a flat 16);
        //  * a command is RUNNING -> wake every 100ms so its elapsed clock in
        //    the chrome ticks;
        //  * otherwise (idle at a prompt) -> block INDEFINITELY: zero wakeups
        //    until real PTY/inbox activity.
        int timeout = -1;
        if (pending_output_) {
            timeout = static_cast<int>(std::max<std::int64_t>(1, kCoalesceMs - since));
        } else if (last_running_) {
            timeout = 100;
        }
        ::poll(fds, static_cast<nfds_t>(n), timeout);
    }
}

// Returns true when the grid CHANGED but the change was NOT posted (because
// post_output was false — coalesced within the window). The caller uses that to
// arm a precise wake at the coalesce deadline, so no change is ever dropped and
// we avoid a second render_lock acquisition just to peek the generation.
bool TabActor::publish_status(bool post_output) {
    std::lock_guard lk(render_lock_);
    auto *s = term_.poll().running;
    if (!s) return false;

    bool pending = false;
    // Output (grid advanced) is coalesced/rate-limited by the caller: only post
    // when it's due, so a flooding child can't drown the GUI in wakeups.
    const std::uint64_t gen = s->generation();
    if (gen != last_gen_) {
        if (post_output) {
            last_gen_ = gen;
            to_gui_.post(TabOutput{id_, gen});
        } else {
            pending = true; // changed but held back this pass
        }
    }

    if (std::string t = s->window_title(); t != last_title_) {
        last_title_ = t;
        to_gui_.post(TabTitleChanged{id_, std::move(t)});
    }
    if (std::string d = s->working_dir(); d != last_cwd_) {
        last_cwd_ = d;
        to_gui_.post(TabDirChanged{id_, std::move(d)});
    }

    // OSC 133 command status: post when the running command or its completion
    // changes, so the Activity-tab chrome tracks it live.
    bool running = false;
    std::string cmd;
    bool finished = false;
    int code = 0;
    if (auto cur = s->current_command(); cur && !cur->finished) {
        running = true;
        cmd = cur->command;
    } else if (auto last = s->last_command(); last && last->finished) {
        finished = true;
        cmd = last->command;
        code = last->exit_code.value_or(0);
    }
    if (running != last_running_ || finished != last_finished_ || cmd != last_cmd_) {
        last_running_ = running;
        last_finished_ = finished;
        last_cmd_ = cmd;
        to_gui_.post(TabCommand{id_, running, cmd, finished, code});
    }
    return pending;
}

} // namespace hand
