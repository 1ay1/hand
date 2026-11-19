#pragma once
// hand::Pty — a shell on a pseudo-terminal. POSIX only (forkpty).
//
// Owns the master fd and the child pid. Reading happens on a background
// stream (see main.cpp), writing from the UI thread; both are plain fd I/O.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/types.h>

namespace hand {

class Pty {
  public:
    Pty() = default;
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    ~Pty();

    /// Start `shell` (empty: $SHELL, else /bin/sh) as an interactive shell on
    /// a new pty of cols x rows, with `env` set and `args` after argv[0].
    bool spawn(int cols, int rows, std::string shell = {},
               const std::vector<std::pair<std::string, std::string>>& env = {},
               const std::vector<std::string>& args = {}, const std::string& cwd = {});
    /// The shell spawn() would run for `shell` (resolves $SHELL).
    [[nodiscard]] static std::string resolve_shell(std::string shell);

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    /// Write all of `bytes` (retries on EINTR/EAGAIN, bounded).
    void write(std::string_view bytes) const;
    /// Tell the child the new grid size (TIOCSWINSZ -> SIGWINCH).
    void resize(int cols, int rows, int px_w = 0, int px_h = 0) const;

  private:
    int   fd_ = -1;
    pid_t pid_ = -1;
};

/// Block until `fd` has data or `timeout_ms` passes; read what is there.
/// Returns false on EOF/error (the child is gone).
bool pty_read(int fd, std::string& out, int timeout_ms);

/// Reap the child; its exit code, or -1 if still running.
int pty_reap(pid_t pid);

/// Run `argv` fully detached (double fork, setsid, stdio on /dev/null):
/// never blocks, never leaves a zombie. For opening links, notifications.
void spawn_detached(const std::vector<std::string>& argv);

}  // namespace hand
