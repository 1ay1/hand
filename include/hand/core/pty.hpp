#pragma once
#include <mayag/core/target.hpp>
// hand::Pty — a shell on a pseudo-terminal.
//
// Two implementations, one interface: forkpty on POSIX (src/core/pty.cpp) and
// ConPTY on Windows (src/core/pty_win.cpp). The app only ever sees an opaque
// `fd` and a `pid`, so nothing above this header is platform-aware.
//
// Owns the master fd and the child pid. Reading happens on a background
// stream (see main.cpp), writing from the UI thread; both are plain fd I/O.

#include <mayag/core/sync.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if MAYAG_OS_WINDOWS
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

/// What to start on a new pty.
struct PtySpec {
    int cols = 80, rows = 24;
    /// POSIX: empty means $SHELL, else /bin/sh.
    /// Windows: empty means %HAND_SHELL%, else pwsh, powershell, %COMSPEC%.
    std::string shell;
    std::vector<std::pair<std::string, std::string>> env;   ///< set in the child
    std::vector<std::string> args;                          ///< after argv[0]
    std::string cwd;                                        ///< "" = inherit
};

/// A program on a pseudo-terminal, started when constructed.
///
/// Shared by the tab (writes, resizes) and its reader thread (reads, reaps)
/// as a mayag::CoOwned<Pty>, which needs mayag::Sync -- and jaal PROVES it
/// here, field by field, rather than taking anyone's word: the fd and pid are
/// `const` (fixed at construction, so no thread can write them), and the one
/// thing that changes, "the child was reaped", is a Cell. Every member
/// function is a syscall the kernel makes thread-safe. (static_assert below.)
class Pty {
    struct Started { int fd = -1; pid_t pid = -1; };
    explicit Pty(Started s) noexcept : master(s.fd), child(s.pid) {}
    [[nodiscard]] static Started start(const PtySpec& spec);

  public:
    explicit Pty(const PtySpec& spec) : Pty(start(spec)) {}
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    ~Pty();

    /// The shell a spec runs for `shell` (resolves $SHELL).
    [[nodiscard]] static std::string resolve_shell(std::string shell);

    /// Did the program start? (A failed spawn is a Pty with no fd.)
    [[nodiscard]] bool ok() const noexcept { return master >= 0; }
    [[nodiscard]] int fd() const noexcept { return master; }
    [[nodiscard]] pid_t pid() const noexcept { return child; }
    /// The child's exit code if it has exited (and reap it), else -1. Once it
    /// returns >= 0 the destructor leaves the pid alone: the OS may already
    /// have given that number to another process.
    int reap();

    /// Write all of `bytes` (retries on EINTR/EAGAIN, bounded).
    void write(std::string_view bytes) const;
    /// Tell the child the new grid size (TIOCSWINSZ -> SIGWINCH;
    /// ResizePseudoConsole on Windows, where px_w/px_h are ignored).
    void resize(int cols, int rows, int px_w = 0, int px_h = 0) const;

    // Public on purpose: these ARE the state, and they are what jaal checks.
    const int         master = -1;   ///< the pty's master fd (a table slot on Windows)
    const pid_t       child = -1;
    mayag::Cell<bool> reaped;        ///< the reader reaped the child
};
static_assert(mayag::Sync<Pty>, "Pty must be safe to share between the tab and its reader");

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

/// In a fork child, before exec: every signal disposition default, nothing
/// blocked (ignored signals survive exec; a child must not inherit hand's).
/// POSIX only; async-signal-safe.
void reset_signals() noexcept;

/// Run `argv` fully detached (double fork, setsid, stdio on /dev/null;
/// DETACHED_PROCESS on Windows): never blocks, never leaves a zombie.
/// For opening links, notifications.
void spawn_detached(const std::vector<std::string>& argv);

}  // namespace hand
