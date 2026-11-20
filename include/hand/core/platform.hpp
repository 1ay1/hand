#pragma once
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
// interesting ones: ConPTY hands back anonymous pipes, which are NOT
// selectable, so readiness is modelled with an overlapped peek instead (see
// src/core/platform_win.cpp).

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
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
#  ifndef NOGDI
#    define NOGDI 1
#  endif
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

// ── 3. how much memory we used ──────────────────────────────────────────
/// Peak resident set size in bytes, 0 when the OS will not say.
[[nodiscard]] std::uint64_t peak_rss();

// ── 4/5. readiness ──────────────────────────────────────────────────────
//
// A Waker is a self-pipe: one end you signal, one end a waiter watches. The
// reader thread blocks on {waker, pty} and returns the moment either has
// something, which is what makes ctrl-C latency a scheduling question and
// not a polling-interval question.
class Waker {
  public:
    Waker();
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    /// Make a pending wait() return promptly. Safe from any thread, and
    /// idempotent: many signals may collapse into one wakeup.
    void signal() const noexcept;
    /// Drain the pending signals so the next wait() blocks again.
    void drain() const noexcept;

    /// The raw handle a waiter watches; an fd on POSIX.
    [[nodiscard]] std::intptr_t handle() const noexcept { return read_; }

  private:
    std::intptr_t read_ = -1;
    std::intptr_t write_ = -1;
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
