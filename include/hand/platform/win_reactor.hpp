// SPDX-License-Identifier: LGPL-2.0-or-later
//
// win_reactor — hand's readiness core on Windows: the ONE place the loop blocks.
// The exact counterpart of reactor.hpp (epoll) on Linux, and the reason the
// Windows port is genuinely fast rather than merely functional.
//
// ─── the problem ───────────────────────────────────────────────────────────
// A terminal must block on a heterogeneous set: child PTY output, window
// messages, and a key-repeat timer. On Linux all three are fds, so epoll takes
// them uniformly. On Windows they are three DIFFERENT kinds of object, and the
// naive solutions are all bad:
//
//   • a reader THREAD per pipe + a queue     — a thread, a lock, a copy, and
//                                              scheduler latency per keystroke;
//   • PeekNamedPipe in a polling loop        — burns a core, adds up to the
//                                              poll interval of latency;
//   • GetMessage() only                      — blocks on the message queue and
//                                              cannot see the PTY at all.
//
// ─── what we do instead ────────────────────────────────────────────────────
// MsgWaitForMultipleObjectsEx is the one primitive that waits on kernel handles
// AND the window message queue simultaneously, with a timeout. Because the PTY
// read is OVERLAPPED (see toe/pty/win_io.hpp), its readiness IS a kernel event
// object — so all three sources reduce to "handles + messages", which is
// precisely this call's shape:
//
//     [pty event][repeat timer] + QS_ALLINPUT  ->  MsgWaitForMultipleObjectsEx
//
// One syscall, no threads, no polling, no locks. The wait is woken by the
// kernel the instant the child writes, a key repeats, or a message posts —
// giving the same latency profile as the epoll path on Linux.
//
// MWMO_INPUTAVAILABLE is essential: without it, input ALREADY sitting in the
// queue when we call does not wake the wait (the "new input only" trap), and
// the terminal would appear to hang until the next keypress.

#ifndef HAND_PLATFORM_WIN_REACTOR_HPP
#define HAND_PLATFORM_WIN_REACTOR_HPP

#if defined(_WIN32)

#include <cstdint>

#include "toe/app.hpp"      // toe::Readiness, toe::WaitDeadline
#include "toe/pty/win_io.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hand::platform {

// The readiness wait, as a value type owning only borrowed handles.
class WinReactor {
public:
    // `repeat_timer` is an optional waitable timer for key auto-repeat (null if
    // the backend has no repeat armed). Handles are borrowed, never closed here.
    void set_repeat_timer(HANDLE t) noexcept { repeat_ = t; }

    // The config-watch event (ReadDirectoryChangesW completion). Folding it in
    // here is what makes live config reload event-driven on Windows, with the
    // same "one wait for everything" property the epoll path has on Linux.
    void set_config_event(void *e) noexcept { config_ = static_cast<HANDLE>(e); }

    // Did the last wait() wake because the config watch fired? The caller
    // drains the watch and reloads.
    [[nodiscard]] bool config_ready() const noexcept { return config_hit_; }

    // Block until the PTY is readable, a window message arrives, the repeat
    // timer fires, or the deadline elapses. Reports which of {pty, window} woke
    // us — a spurious wake (both false) is fine; the caller re-probes.
    [[nodiscard]] toe::Readiness wait(int pty_fd, toe::WaitDeadline d) noexcept {
        HANDLE handles[3];
        DWORD n = 0;
        config_hit_ = false;

        // The PTY's readiness event: signalled by the kernel when the posted
        // overlapped read completes. Index 0 when present.
        const DWORD pty_index = n;
        if (HANDLE ev = static_cast<HANDLE>(toe::win::readable_event(pty_fd)); ev) {
            handles[n++] = ev;
        }
        const DWORD repeat_index = n;
        if (repeat_) handles[n++] = repeat_;
        const DWORD config_index = n;
        if (config_) handles[n++] = config_;

        const DWORD timeout = d.blocks_forever()
                                  ? INFINITE
                                  : static_cast<DWORD>((d.ns + 999'999) / 1'000'000);

        // QS_ALLINPUT: any window message. MWMO_INPUTAVAILABLE: also wake for
        // input already queued before this call (see the header note).
        const DWORD r = ::MsgWaitForMultipleObjectsEx(n, handles, timeout, QS_ALLINPUT,
                                                      MWMO_INPUTAVAILABLE);

        toe::Readiness out{};
        if (r == WAIT_TIMEOUT || r == WAIT_FAILED) return out;

        const DWORD hit = r - WAIT_OBJECT_0;
        if (hit == n) {
            out.window = true; // the message-queue slot sits just past the handles
            return out;
        }
        if (toe::win::readable_event(pty_fd) && hit == pty_index) {
            out.pty = true;
        } else if (config_ && hit == config_index) {
            config_hit_ = true;
        } else if (repeat_ && hit == repeat_index) {
            // The repeat timer fired. Not a PTY/window event: the caller's own
            // repeat logic runs off the same turn, so report a spurious wake.
        }
        return out;
    }

private:
    HANDLE repeat_ = nullptr;
    HANDLE config_ = nullptr;
    bool config_hit_ = false;
};

} // namespace hand::platform

#endif // _WIN32
#endif // HAND_PLATFORM_WIN_REACTOR_HPP
