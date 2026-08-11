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
                    } else if constexpr (std::is_same_v<T, TabResize>) {
                        std::lock_guard lk(render_lock_);
                        if (auto *s = term_.poll().running)
                            s->resize(toe::PixelSize{cmd.px_w, cmd.px_h});
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
            if (p.exited) {
                exited = true;
                exit_code = p.exited->code;
            } else {
                pty_fd = p.running->pty_fd();
                // Bounded drain: consume all readable output, but cap the number
                // of budgeted passes so a truly endless stream can't starve the
                // command loop (it just resumes next wakeup).
                for (int passes = 0; passes < 64; ++passes) {
                    p.running->pump_output();
                    if (!p.running->output_pending()) break;
                }
            }
        }
        if (exited) {
            to_gui_.post(TabExited{id_, exit_code});
            break;
        }

        // --- 3. post CHANGE-ONLY status, RATE-LIMITED ------------------------
        // A chatty child (an agent, a build) bumps the grid many times per
        // millisecond; notifying the GUI on every bump would peg a core. Coalesce
        // to at most ~1 output notification per frame (16ms) — the GUI can't
        // show more than that anyway. Title/dir/command changes are posted
        // immediately (they're rare).
        const std::int64_t now = now_ms_actor();
        const bool due = (now - last_notify_ms_) >= 16;
        publish_status(due);
        if (due) last_notify_ms_ = now;

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
        // While a coalesced output notification is still pending (fd stays
        // readable under a flood), a short timeout paces us to ~60fps; otherwise
        // block up to 100ms so the running-command elapsed clock still ticks.
        const int timeout = due ? 100 : 16;
        ::poll(fds, static_cast<nfds_t>(n), timeout);
    }
}

void TabActor::publish_status(bool post_output) {
    std::lock_guard lk(render_lock_);
    auto *s = term_.poll().running;
    if (!s) return;

    // Output (grid advanced) is coalesced/rate-limited by the caller: only post
    // when it's due, so a flooding child can't drown the GUI in wakeups.
    if (post_output) {
        const std::uint64_t gen = s->generation();
        if (gen != last_gen_) {
            last_gen_ = gen;
            to_gui_.post(TabOutput{id_, gen});
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
}

} // namespace hand
