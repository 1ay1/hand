#pragma once
// hand::proc — the OS services the SSH manager needs, behind one small seam.
//
//   run(argv, in, ms)     a child process, stdout/stderr captured, killed at
//                         the timeout (sftp listings, transfers)
//   tcp_listeners()       every listening TCP port and who owns it (live
//                         tunnel status: is ssh's -L 5432 really up?)
//   putty_sessions(root)  PuTTY's saved sessions (registry on Windows,
//                         ~/.putty/sessions elsewhere), raw key=value
//   pick_files(title)     the OS file picker (Windows); empty if cancelled
//   downloads_dir()       where downloads go
//   reveal(path)          show a file in the OS file manager
//
// POSIX in proc_posix.cpp, Windows in proc_win.cpp; nothing above this
// header includes an OS header.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hand::proc {

struct Result {
    bool        started = false;   ///< the program could be run at all
    bool        timed_out = false;
    int         code = -1;
    std::string out, err;
};
/// Run argv[0] (looked up on PATH) with argv[1..], feeding `in` to stdin.
[[nodiscard]] Result run(const std::vector<std::string>& argv, std::string_view in = {}, int timeout_ms = 30000);

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

/// A child that stays up and is spoken to in requests: write bytes to its
/// stdin, read its answer off stdout. What a remote shell kept open over
/// ssh is (hand::remote): one spawn and one login, then every request costs
/// a round trip instead of a new process (measured: 3 ms against 40-65 ms).
///
/// stdin is a socket that can never raise SIGPIPE (a write to a dead child
/// fails instead of killing hand); stderr is drained as it comes, into err().
/// Not shared between threads: one user at a time (ChannelPool hands them out).
class Channel {
  public:
    /// argv started with its stdio piped; null where the platform has no
    /// channels (Windows: callers fall back to run()) or it cannot start.
    [[nodiscard]] static std::unique_ptr<Channel> open(const std::vector<std::string>& argv);
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    /// Closes stdin, ends the child, reaps it.
    ~Channel();

    /// All of `s` to the child's stdin by `by`; false once it is gone.
    [[nodiscard]] bool write(std::string_view s, Deadline by);
    /// One line of stdout (without its newline) by `by`.
    [[nodiscard]] bool read_line(std::string& line, Deadline by);
    /// Exactly `n` bytes of stdout, appended to `out`, by `by`.
    [[nodiscard]] bool read_exact(std::size_t n, std::string& out, Deadline by);
    /// Everything up to `marker` appended to `out` (the marker is consumed, not
    /// appended), by `by`.
    [[nodiscard]] bool read_until(std::string_view marker, std::string& out, Deadline by);
    /// What the child wrote to stderr so far.
    [[nodiscard]] const std::string& err() const noexcept { return err_; }
    /// A read or write failed or timed out: the channel is out of step, never reuse it.
    [[nodiscard]] bool broken() const noexcept { return broken_; }
    /// When it was last used (the pool closes channels idle too long).
    Deadline used{};
    /// What its user learned about the far end when it opened it (hand::remote:
    /// the host decodes base64).
    bool b64 = false;

  private:
    Channel() = default;
    bool fill(Deadline by);   ///< read more of stdout into buf_ (and drain stderr)
    int in_ = -1, out_ = -1, errfd_ = -1, pid_ = -1;
    std::string buf_, err_;
    bool broken_ = false;
};

/// Idle channels by key (the argv that opened them), for reuse. One per
/// process; thread-safe (the lock is held only to take or give, never across
/// a request, so a slow host never holds up another).
class ChannelPool {
  public:
    /// An idle channel for `key`, or null (open one). Channels idle longer
    /// than `max_idle` are closed instead of handed out.
    [[nodiscard]] std::unique_ptr<Channel> take(const std::string& key, std::chrono::seconds max_idle = std::chrono::seconds(300));
    /// Back for reuse (a broken one is just closed); at most `keep` idle per key.
    void give(const std::string& key, std::unique_ptr<Channel> ch, std::size_t keep = 4);
    /// Close every idle channel whose key starts with `prefix` ("" = all).
    void drop(const std::string& prefix);
    /// Idle channels held now (for tests).
    [[nodiscard]] std::size_t idle(const std::string& key);
};
/// The process's one pool.
[[nodiscard]] ChannelPool& channels();

struct Listener {
    int         port = 0;
    int         pid = 0;
    std::string process;           ///< short name ("ssh"), "" if unknown
};
/// Listening TCP sockets (IPv4 and IPv6). `ok` false: not knowable here.
[[nodiscard]] std::vector<Listener> tcp_listeners(bool* ok = nullptr);

struct RawSession {
    std::string name;              ///< decoded ("My server")
    std::map<std::string, std::string> values;   ///< DWORDs as decimal
};
/// PuTTY's sessions under `root` ("" = PuTTY's own location).
[[nodiscard]] std::vector<RawSession> putty_sessions(const std::string& root = {});

[[nodiscard]] std::vector<std::string> pick_files(const std::string& title);
[[nodiscard]] std::string downloads_dir();
void reveal(const std::string& path);
/// Open a local file in its default app.
void open_path(const std::string& path);
/// Open a link with the system's opener (`open` on macOS, xdg-open on the
/// free desktops, the shell's association on Windows). Detached: never waits
/// for the browser it starts.
void open_url(const std::string& url);
/// This machine's name, as a shell would report it in OSC 7.
[[nodiscard]] std::string host_name();
/// A fresh temp file path (not created).
[[nodiscard]] std::string temp_path(std::string_view stem);

}  // namespace hand::proc
