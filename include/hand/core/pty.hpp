#pragma once
// hand::Pty — a shell on a pseudo-terminal.
//
// Two implementations, one interface: forkpty on POSIX (src/core/pty.cpp) and
// ConPTY on Windows (src/core/pty_win.cpp). The app only ever sees an opaque
// `fd` and a `pid`, so nothing above this header is platform-aware.
//
// Owns the master fd and the child pid. Reading happens on a background
// stream (see main.cpp), writing from the UI thread; both are plain fd I/O.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
// No <sys/types.h> on Windows. hand only ever stores a pid and compares it,
// so a plain int is the honest type; on POSIX it stays the real pid_t.
namespace hand {
using pty_pid_t = int;
}
#  define HAND_PID_T ::hand::pty_pid_t
#else
#  include <sys/types.h>
#  define HAND_PID_T ::pid_t
#endif

namespace hand {

/// The process id type: pid_t on POSIX, int on Windows.
using pid_t = HAND_PID_T;

class Pty {
  public:
    Pty() = default;
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    ~Pty();

    /// Start `shell` as an interactive shell on a new pty of cols x rows,
    /// with `env` set and `args` after argv[0].
    /// POSIX: empty shell means $SHELL, else /bin/sh.
    /// Windows: empty means %HAND_SHELL%, else pwsh, powershell, %COMSPEC%.
    bool spawn(int cols, int rows, std::string shell = {},
               const std::vector<std::pair<std::string, std::string>>& env = {},
               const std::vector<std::string>& args = {}, const std::string& cwd = {});
    /// The shell spawn() would run for `shell` (resolves $SHELL).
    [[nodiscard]] static std::string resolve_shell(std::string shell);

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    /// Write all of `bytes` (retries on EINTR/EAGAIN, bounded).
    void write(std::string_view bytes) const;
    /// Tell the child the new grid size (TIOCSWINSZ -> SIGWINCH;
    /// ResizePseudoConsole on Windows, where px_w/px_h are ignored).
    void resize(int cols, int rows, int px_w = 0, int px_h = 0) const;

  private:
    int   fd_ = -1;
    pid_t pid_ = -1;
};

/// Block until `fd` has data or `timeout_ms` passes; read what is there.
/// Returns false on EOF/error (the child is gone).
bool pty_read(int fd, std::string& out, int timeout_ms);

/// The OS handle to wait on for "this pty has output": the fd itself on
/// POSIX, the ConPTY output pipe HANDLE on Windows (where `fd` is only a
/// table index). Feed it to plat::wait_readable; -1 when the pty is closed.
[[nodiscard]] std::intptr_t pty_handle(int fd);

/// Reap the child; its exit code, or -1 if still running.
int pty_reap(pid_t pid);

/// The foreground process group on the pty (the program you are talking to), -1 if none.
/// Windows has no process groups on a pty: the newest child of the shell is
/// reported instead, which answers the same question in practice.
[[nodiscard]] pid_t pty_foreground(int fd);
/// A process's short name ("ssh", "nvim"); "" when it is gone.
[[nodiscard]] std::string proc_name(pid_t pid);
/// A process's argv; empty when it is gone or not ours to read.
[[nodiscard]] std::vector<std::string> proc_argv(pid_t pid);

/// Run `argv` fully detached (double fork, setsid, stdio on /dev/null;
/// DETACHED_PROCESS on Windows): never blocks, never leaves a zombie.
/// For opening links, notifications.
void spawn_detached(const std::vector<std::string>& argv);

}  // namespace hand
