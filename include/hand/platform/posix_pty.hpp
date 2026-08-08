// SPDX-License-Identifier: LGPL-2.0-or-later
//
// posix_pty — process creation, the HOST's job.
//
// toe never forks: its engine adopts an already-open PTY master fd (toe::AdoptFd)
// and drives it. Creating the child is host policy — argv resolution, the TERM
// value, a pre-exec hook — and the OS mechanism (forkpty, via the glibc <pty.h>
// or the BSD/macOS <util.h> spelling) is a native detail. Both belong HERE, in
// hand, next to the window and clipboard code, NOT in the portable engine.
//
// spawn() forkpty()s the shell and returns a toe::AdoptFd { master_fd, child }
// the caller drops straight into toe::Config::source. This is the one place in
// the whole terminal that calls forkpty; a Windows port would add a sibling
// conpty.cpp producing the same AdoptFd, with zero change to toe.

#ifndef HAND_PLATFORM_POSIX_PTY_HPP
#define HAND_PLATFORM_POSIX_PTY_HPP

#include <functional>
#include <string>
#include <vector>

#include "toe/core/types.hpp" // toe::Result / toe::fail
#include "toe/pty/pty_source.hpp" // toe::AdoptFd

namespace hand {

// How the host spawns the child terminal. Mirrors the policy toe used to bake in
// (argv/$SHELL, TERM, pre_exec) — now owned by the frontend.
struct SpawnCommand {
    // The child argv. Empty -> resolved to $SHELL, then /bin/sh.
    std::vector<std::string> argv{};

    // The TERM value advertised to the child (not hard-coded in the engine).
    std::string term = "xterm-256color";

    // Runs in the CHILD after fork(), before exec(): setenv/chdir/setsid/
    // drop-privs. Must be async-signal-safe. Optional.
    std::function<void()> pre_exec{};
};

// forkpty() the command and hand back an adopt-able master fd + child pid.
// The returned AdoptFd owns the fd (owns_fd = true) so toe closes it on
// teardown. The initial winsize is a placeholder; toe resizes the pty to the
// real grid the moment the Terminal is created.
[[nodiscard]] toe::Result<toe::AdoptFd> spawn_pty(const SpawnCommand &cmd);

} // namespace hand

#endif // HAND_PLATFORM_POSIX_PTY_HPP
