#pragma once
#include <mayag/core/target.hpp>
// hand::plat — the thin OS seam.
//
// hand is a POSIX program at heart: a pty is an fd, you poll it, you read it.
// Windows has all of those ideas but none of the same spellings, so rather
// than scatter `#ifdef _WIN32` through the app this header provides the five
// POSIX primitives hand actually leans on, and implements them natively:
//
//   1. fnmatch()          — ssh_config host globs
//   2. getuid()           — a per-user runtime directory
//   3. max RSS            — the benchmark's peak-memory line
//   4. a wakeable channel  — the reader thread's "stop now" pipe
//   5. wait-for-readable   — poll() over {wake, pty}
//
// On POSIX these are one-line passthroughs to the real thing, so the native
// platform pays nothing for the portability. On Windows (4) and (5) are the
// interesting ones: ConPTY's output is read through an overlapped named
// pipe, so readiness is a real kernel wait on its read's event (see
// src/core/platform_win.cpp and pty_win.cpp).

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#if MAYAG_OS_WINDOWS
// Keep the Win32 headers from redefining the language. <windows.h> brings in
// `min`/`max` macros that break std::min, a legacy `near`/`far` memory-model
// keyword pair that collides with ordinary variable names, and ~2 MB of
// headers nobody needs. Every translation unit that touches Win32 in hand
// goes through here, so the quarantine is complete.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
// NOGDI is deliberately not defined: mayag's Win32 window backend blits
// through GDI (SetDIBitsToDevice), and it would lose those declarations.
#endif

namespace hand::plat {

// ── 1. shell-style glob ─────────────────────────────────────────────────
/// fnmatch(pat, s, 0): `*`, `?` and `[...]`, no leading-dot or slash rules.
/// hand only ever matches ssh_config Host patterns with it.
[[nodiscard]] bool glob_match(const std::string& pat, const std::string& s);

/// Expand a filesystem glob to the paths that exist, sorted. The `Include`
/// directive in ssh_config is the only caller. Wildcards are honoured in the
/// last component only, which is all ssh itself documents.
[[nodiscard]] std::vector<std::string> glob_paths(const std::string& pattern);

// ── 2. who we are ───────────────────────────────────────────────────────
/// A stable per-user token for naming runtime files. The uid on POSIX; a
/// hash of the username+SID on Windows, where there is no small integer.
[[nodiscard]] std::string user_token();

/// This process's id. getpid() / GetCurrentProcessId().
[[nodiscard]] int self_pid();

/// The absolute path of the running executable. Used to re-exec ourselves as
/// a test fixture (`--cat-v`), so the tests need no external programs.
[[nodiscard]] std::string self_exe();

/// `name` found on PATH (`:`-separated, executable; on Windows `;` and
/// `name.exe`), or "" when it is not there. An absolute or relative path is
/// returned as is if it exists. The one PATH lookup hand does itself.
[[nodiscard]] std::string find_program(std::string_view name);

/// The argv prefix that runs a command LINE (arguments, pipes, quotes) the
/// way this OS's terminals run `-e CMD`: {"/bin/sh", "-c"} on POSIX,
/// {"cmd.exe", "/d", "/s", "/c"} on Windows (which has no /bin/sh). The
/// command line itself is appended as the last argument.
[[nodiscard]] std::vector<std::string> command_runner();

/// One OS read from stdin (no stdio buffering): bytes read, 0 at EOF, <0 on error.
[[nodiscard]] long read_stdin(char* buf, std::size_t n);
/// Write all of `n` bytes to stdout, unbuffered. False when the reader is gone.
bool write_stdout(const char* buf, std::size_t n);

// ── 3. how much memory we used ──────────────────────────────────────────
/// Peak resident set size in bytes, 0 when the OS will not say.
[[nodiscard]] std::uint64_t peak_rss();

/// The user's home, with '/' separators: what `~` means to hand AND to ssh.
/// $HOME on POSIX. %USERPROFILE% on Windows, ALWAYS: Microsoft's OpenSSH
/// reads %USERPROFILE%\.ssh and ignores $HOME (MSYS2 / Git Bash point it at
/// AppData\Roaming, a ~/.ssh ssh never reads). The one definition -- env,
/// ssh config, tilde expansion and the shared-connection directory all ask
/// here. "" when unknown.
[[nodiscard]] std::string home_dir();

/// Local broken-down time, thread-safe (localtime_r / localtime_s).
[[nodiscard]] std::tm local_time(std::time_t t);

/// Does this OS's pty repaint the whole viewport from its own buffer after a
/// resize? Windows ConPTY does (it re-sends every visible row at the new
/// size); a POSIX pty never does -- the app redraws what it owns. A capability
/// toe needs: history it brought down into view would be painted over.
[[nodiscard]] constexpr bool pty_repaints_on_resize() noexcept {
#if MAYAG_OS_WINDOWS
    return true;
#else
    return false;
#endif
}

/// Can this OS's OpenSSH share a connection (ControlMaster over a Unix
/// socket)? POSIX yes; Microsoft's OpenSSH for Windows no -- every
/// connection with the options died with "getsockname failed: Not a socket".
/// A capability, asked by the one place that decides (remote::control_dir).
[[nodiscard]] constexpr bool ssh_can_share() noexcept {
#if MAYAG_OS_WINDOWS
    return false;
#else
    return true;
#endif
}

// ── 4/5. readiness ──────────────────────────────────────────────────────
//
// A Waker is a self-pipe: one end you signal, one end a waiter watches. The
// reader thread blocks on {waker, pty} and returns the moment either has
// something, which is what makes ctrl-C latency a scheduling question and
// not a polling-interval question.
class Waker {
    struct Ends { std::intptr_t read = -1, write = -1; };
    [[nodiscard]] static Ends open();   ///< a pipe (POSIX) / an event (Windows)
    explicit Waker(Ends e) noexcept : read_end(e.read), write_end(e.write) {}

  public:
    Waker() : Waker(open()) {}
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    /// Make a pending wait() return promptly. Safe from any thread, and
    /// idempotent: many signals may collapse into one wakeup.
    void signal() const noexcept;
    /// Drain the pending signals so the next wait() blocks again.
    void drain() const noexcept;

    /// The raw handle a waiter watches; an fd on POSIX.
    [[nodiscard]] std::intptr_t handle() const noexcept { return read_end; }

    // Public and const: fixed when the waker is made, so threads may share
    // it (jaal proves Waker Sync from these two fields).
    const std::intptr_t read_end = -1;
    const std::intptr_t write_end = -1;   ///< == read_end on Windows (one event)
};

/// Which of {waker, pty} became ready within timeout_ms.
struct Ready {
    bool woke = false;   ///< the waker was signalled
    bool data = false;   ///< the pty has bytes (or hung up)
    bool error = false;  ///< the wait itself failed
};

/// Wait until the waker is signalled, `pty` is readable, or the timeout
/// expires. Pass pty = -1 to watch only the waker (back-pressure stalls).
[[nodiscard]] Ready wait_readable(const Waker& w, std::intptr_t pty, int timeout_ms);

}  // namespace hand::plat

namespace hand {
/// Convenience aliases: these read better at call sites.
using plat::self_exe;
using plat::self_pid;
}  // namespace hand
