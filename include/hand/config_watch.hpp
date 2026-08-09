// SPDX-License-Identifier: LGPL-2.0-or-later
//
// ConfigWatch — a tiny inotify-backed watcher that makes hand's config LIVE in
// both directions: edit the VIBE file in your editor and the running terminal
// picks it up, the same instant it would from the settings pane.
//
// Design notes (the correctness that makes this clean rather than flaky):
//
//   * We watch the config's PARENT DIRECTORY, not the file inode. Editors save
//     atomically (write temp, rename over the target); a watch on the inode dies
//     the moment that rename happens. A directory watch for CLOSE_WRITE/MOVED_TO
//     filtered to the filename survives every save style (in-place OR rename).
//
//   * The watcher is just an fd. It is folded into the SAME epoll wait the
//     terminal already blocks on (see TerminalWait::watch_config), so the loop
//     stays 100% event-driven — zero polling, zero idle CPU, zero added latency.
//     No thread, no lock: the reload runs on the loop thread between frames,
//     preserving the engine's single-threaded invariant.
//
//   * Self-write suppression: when hand itself saves the file (settings pane),
//     it calls note_self_write(); events within a short window after are
//     ignored so we don't reload the file we just wrote and stomp a live edit.
//
//   * Debounce: an editor save emits several events; drained() coalesces them so
//     the caller reloads once per settle.

#ifndef HAND_CONFIG_WATCH_HPP
#define HAND_CONFIG_WATCH_HPP

#include <chrono>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace hand {

class ConfigWatch {
public:
    ConfigWatch() = default;
    ~ConfigWatch() { close_fd(); }
    ConfigWatch(const ConfigWatch &) = delete;
    ConfigWatch &operator=(const ConfigWatch &) = delete;

    // Begin watching `path`'s directory for changes to its basename. Safe to
    // call with an empty path (no watch). Returns true if the watch is live.
    bool start(const std::string &path) {
        close_fd();
        if (path.empty()) return false;
        const auto slash = path.find_last_of('/');
        dir_ = (slash == std::string::npos) ? std::string{"."} : path.substr(0, slash);
        base_ = (slash == std::string::npos) ? path : path.substr(slash + 1);
        if (dir_.empty()) dir_ = "/";

        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return false;
        // CLOSE_WRITE covers in-place saves; MOVED_TO covers atomic-rename saves
        // (temp file renamed onto the config). CREATE catches first-time writes.
        wd_ = ::inotify_add_watch(fd_, dir_.c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd_ < 0) { close_fd(); return false; }
        return true;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool active() const noexcept { return fd_ >= 0; }

    // Record that WE just wrote the config, so the resulting inotify event is
    // ignored (we already have those values live). Keep the window short.
    void note_self_write() noexcept { self_write_ms_ = now_ms(); }

    // Drain all pending inotify events. Returns true iff a real, non-self,
    // relevant change to our config file was seen — the caller then reloads.
    // Always drains fully so the fd goes quiet (level-triggered epoll).
    [[nodiscard]] bool drained() {
        if (fd_ < 0) return false;
        bool hit = false;
        alignas(inotify_event) char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n <= 0) break; // EAGAIN or error: nothing (more) to read
            ssize_t off = 0;
            while (off < n) {
                auto *ev = reinterpret_cast<inotify_event *>(buf + off);
                if (ev->len > 0 && base_ == ev->name) hit = true;
                off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            }
        }
        if (!hit) return false;
        // Drop the change if it's the echo of our own recent save.
        if (self_write_ms_ != 0 && now_ms() - self_write_ms_ < kSelfWriteWindowMs) return false;
        return true;
    }

private:
    static constexpr std::uint64_t kSelfWriteWindowMs = 600;

    static std::uint64_t now_ms() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    void close_fd() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        wd_ = -1;
    }

    int fd_ = -1;
    int wd_ = -1;
    std::string dir_, base_;
    std::uint64_t self_write_ms_ = 0;
};

} // namespace hand

#endif // HAND_CONFIG_WATCH_HPP
