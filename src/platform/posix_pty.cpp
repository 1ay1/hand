// SPDX-License-Identifier: LGPL-2.0-or-later
//
// forkpty() the child shell — the host's process-creation mechanism. See
// posix_pty.hpp for why this lives in hand and not in the toe engine.

#include "hand/platform/posix_pty.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

// forkpty lives in a different header on glibc vs BSD/macOS. This OS spelling is
// a HOST detail — the engine never sees it.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#include <util.h> // forkpty (BSD / macOS)
#else
#include <pty.h> // forkpty (glibc / util-linux)
#endif

#include <sys/ioctl.h>

namespace hand {

namespace {

// Make the master fd non-blocking so toe's reactor-driven reads never stall.
[[nodiscard]] toe::Result<void> set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return toe::fail(std::string{"fcntl(F_GETFL) failed: "} + std::strerror(errno));
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return toe::fail(std::string{"fcntl(F_SETFL) failed: "} + std::strerror(errno));
    return {};
}

} // namespace

toe::Result<toe::AdoptFd> spawn_pty(const SpawnCommand &cmd) {
    // Resolve argv: explicit -> $SHELL -> /bin/sh.
    std::vector<std::string> args = cmd.argv;
    if (args.empty()) {
        const char *shell = ::getenv("SHELL");
        args.emplace_back(shell && *shell ? shell : "/bin/sh");
    }
    std::vector<char *> cargv;
    cargv.reserve(args.size() + 1);
    for (auto &a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    // A placeholder winsize; toe resizes to the real grid at Terminal::create.
    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;

    int master = -1;
    const ::pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return toe::fail(std::string{"forkpty failed: "} + std::strerror(errno));

    if (pid == 0) {
        // --- child --- TERM is host-chosen, not hard-coded in the engine.
        ::setenv("TERM", cmd.term.empty() ? "xterm-256color" : cmd.term.c_str(), 1);
        // Host hook: setenv/chdir/setsid/drop-privs, before exec.
        if (cmd.pre_exec) cmd.pre_exec();
        ::execvp(cargv[0], cargv.data());
        // exec failed. perror is not async-signal-safe after fork; write a
        // fixed diagnostic via the raw syscall, then leave with 127.
        const char msg[] = "hand: exec failed\n";
        const ssize_t ignored = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
        ::_exit(127);
    }

    // --- parent ---
    if (auto nb = set_nonblocking(master); !nb) {
        ::close(master);
        ::kill(pid, SIGHUP);
        return toe::fail(nb.error().message);
    }

    // Hand toe an adopt-able fd: it owns the master and observes the child pid.
    return toe::AdoptFd{.master_fd = master, .child = pid, .owns_fd = true};
}

} // namespace hand
