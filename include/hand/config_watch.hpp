// SPDX-License-Identifier: LGPL-2.0-or-later
//
// ConfigWatch — a tiny filesystem watcher that makes hand's config LIVE in
// both directions: edit the VIBE file in your editor and the running terminal
// picks it up, the same instant it would from the settings pane.
//
// Cross-platform by design: an inotify backend on Linux and a kqueue backend on
// macOS/BSD, behind ONE public API (start / fd / active / note_self_write /
// drained). Both expose a single pollable fd the backend folds into its wait.
//
// Design notes (the correctness that makes this clean rather than flaky):
//
//   * We watch the config's PARENT DIRECTORY, not the file inode. Editors save
//     atomically (write temp, rename over the target); a watch on the inode dies
//     the moment that rename happens. A directory watch — inotify
//     CLOSE_WRITE/MOVED_TO filtered to the filename, or kqueue NOTE_WRITE on the
//     dir fd — survives every save style (in-place OR rename).
//
//   * The watcher is just an fd. It is folded into the SAME poll/epoll/kqueue
//     wait the terminal already blocks on (see TerminalWait::watch_config on
//     Linux, the wait_readable fd list on macOS), so the loop stays 100%
//     event-driven — zero polling, zero idle CPU, zero added latency. No thread,
//     no lock: the reload runs on the loop thread between frames, preserving the
//     engine's single-threaded invariant.
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
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define HAND_CONFIG_WATCH_WIN32 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define HAND_CONFIG_WATCH_KQUEUE 1
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#else
#define HAND_CONFIG_WATCH_INOTIFY 1
#include <sys/inotify.h>
#endif
#endif // _WIN32

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
#if HAND_CONFIG_WATCH_WIN32
        // Windows paths use either separator; split on whichever comes last.
        const auto slash = path.find_last_of("/\\");
#else
        const auto slash = path.find_last_of('/');
#endif
        dir_ = (slash == std::string::npos) ? std::string{"."} : path.substr(0, slash);
        base_ = (slash == std::string::npos) ? path : path.substr(slash + 1);
        if (dir_.empty()) dir_ = "/";

#if HAND_CONFIG_WATCH_WIN32
        // ReadDirectoryChangesW is the event-driven Windows equivalent of
        // inotify: the kernel signals our OVERLAPPED event when the directory
        // changes, so the watch folds into the same WaitForMultipleObjects the
        // PTY and window use — no polling, no watcher thread.
        {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, dir_.data(),
                                                static_cast<int>(dir_.size()), nullptr, 0);
            std::wstring wdir(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, dir_.data(), static_cast<int>(dir_.size()),
                                  wdir.data(), n);
            dir_handle_ = ::CreateFileW(
                wdir.c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (dir_handle_ == INVALID_HANDLE_VALUE) { dir_handle_ = nullptr; return false; }
        ov_.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov_.hEvent) { close_fd(); return false; }
        return post_read();
#elif HAND_CONFIG_WATCH_INOTIFY
        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return false;
        // CLOSE_WRITE covers in-place saves; MOVED_TO covers atomic-rename saves
        // (temp file renamed onto the config). CREATE catches first-time writes.
        wd_ = ::inotify_add_watch(fd_, dir_.c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd_ < 0) { close_fd(); return false; }
        return true;
#else // kqueue (macOS / BSD)
        // Two vnode watches, because a directory vnode and a file vnode fire on
        // DIFFERENT events on macOS:
        //   * The PARENT DIR fd fires NOTE_WRITE when a file is created, renamed
        //     onto, or deleted inside it — this catches atomic-rename saves
        //     (the temp-then-rename dance every serious editor does), which
        //     replace the inode and would kill a file-only watch.
        //   * The FILE fd fires NOTE_WRITE/NOTE_EXTEND on an in-place append or
        //     truncate-rewrite — which does NOT touch the directory vnode.
        // Watching only the dir misses in-place saves; watching only the file
        // misses rename saves. We register both on one kqueue; the kqueue()
        // handle is the single pollable fd the backend folds into its wait.
        fd_ = ::kqueue();
        if (fd_ < 0) return false;
        ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
        dir_fd_ = ::open(dir_.c_str(), O_RDONLY | O_CLOEXEC);
        if (dir_fd_ < 0) { close_fd(); return false; }
        struct kevent kev[2];
        int nk = 0;
        EV_SET(&kev[nk++], dir_fd_, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);
        // The file may not exist yet (first run) — that's fine; the dir watch
        // catches its creation, after which drained() re-arms the file watch.
        file_fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (file_fd_ >= 0) {
            EV_SET(&kev[nk++], file_fd_, EVFILT_VNODE, EV_ADD | EV_CLEAR,
                   NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME | NOTE_DELETE | NOTE_ATTRIB, 0, nullptr);
        }
        path_ = path;
        if (::kevent(fd_, kev, nk, nullptr, 0, nullptr) < 0) { close_fd(); return false; }
        return true;
#endif
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
#if HAND_CONFIG_WATCH_WIN32
    // Windows has no fd to fold into a wait; the backend waits on this EVENT
    // instead (signalled by the kernel when the directory changes).
    [[nodiscard]] void *event_handle() const noexcept { return ov_.hEvent; }
    [[nodiscard]] bool active() const noexcept { return dir_handle_ != nullptr; }
#else
    [[nodiscard]] bool active() const noexcept { return fd_ >= 0; }
#endif

    // Record that WE just wrote the config, so the resulting inotify event is
    // ignored (we already have those values live). Keep the window short.
    void note_self_write() noexcept { self_write_ms_ = now_ms(); }

    // Drain all pending inotify events. Returns true iff a real, non-self,
    // relevant change to our config file was seen — the caller then reloads.
    // Always drains fully so the fd goes quiet (level-triggered epoll).
    [[nodiscard]] bool drained() {
#if HAND_CONFIG_WATCH_WIN32
        if (!dir_handle_) return false;
        bool hit = false;
        // Harvest the completed overlapped read (non-blocking), then re-arm.
        DWORD got = 0;
        while (::GetOverlappedResult(dir_handle_, &ov_, &got, FALSE)) {
            // got == 0 means the kernel's buffer overflowed and it could not
            // report which files changed. Treat that as a hit: the caller just
            // re-reads the config, and a spurious reload is a cheap no-op.
            if (got == 0) {
                hit = true;
            } else {
                DWORD off = 0;
                for (;;) {
                    auto *fni = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buf_ + off);
                    const int wlen = static_cast<int>(fni->FileNameLength / sizeof(wchar_t));
                    const int n = ::WideCharToMultiByte(CP_UTF8, 0, fni->FileName, wlen, nullptr,
                                                        0, nullptr, nullptr);
                    std::string name(static_cast<std::size_t>(n), '\0');
                    ::WideCharToMultiByte(CP_UTF8, 0, fni->FileName, wlen, name.data(), n,
                                          nullptr, nullptr);
                    if (name == base_) hit = true;
                    if (fni->NextEntryOffset == 0) break;
                    off += fni->NextEntryOffset;
                }
            }
            ::ResetEvent(ov_.hEvent);
            if (!post_read()) break;
            // Loop again in case more changes completed synchronously.
            if (::WaitForSingleObject(ov_.hEvent, 0) != WAIT_OBJECT_0) break;
        }
        if (!hit) return false;
#elif HAND_CONFIG_WATCH_INOTIFY
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
#else // kqueue: drain all queued vnode events non-blocking.
        if (fd_ < 0) return false;
        bool hit = false;
        // The dir vnode doesn't tell us WHICH file changed, so any write to the
        // watched directory is a candidate. We coalesce every pending event and
        // report one hit; the caller re-reads the config file, so a spurious
        // wake from a sibling file costs only a cheap no-op reload.
        struct timespec zero{0, 0};
        struct kevent ev;
        bool dir_changed = false;
        for (;;) {
            const int n = ::kevent(fd_, nullptr, 0, &ev, 1, &zero);
            if (n <= 0) break; // 0 = nothing left, <0 = error
            if (ev.filter != EVFILT_VNODE) continue;
            hit = true;
            if (static_cast<int>(ev.ident) == dir_fd_) dir_changed = true;
            // Our watched dir or file was renamed/deleted — the vnode watch is
            // now stale; flag a re-arm on the current path so we keep working.
            if (ev.fflags & (NOTE_RENAME | NOTE_DELETE)) rearm_ = true;
        }
        // An atomic-rename save swaps the config inode: the OLD file_fd_ now
        // points at the deleted inode and never fires again. Any directory
        // change is our cue to re-open the file and re-register its vnode.
        if (dir_changed || rearm_) { rearm_ = false; rearm_watch(); }
#endif
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
#if HAND_CONFIG_WATCH_WIN32
        if (dir_handle_) {
            ::CancelIoEx(dir_handle_, &ov_);
            DWORD got = 0;
            ::GetOverlappedResult(dir_handle_, &ov_, &got, TRUE);
            ::CloseHandle(dir_handle_);
            dir_handle_ = nullptr;
        }
        if (ov_.hEvent) { ::CloseHandle(ov_.hEvent); ov_.hEvent = nullptr; }
        return;
#else
#if HAND_CONFIG_WATCH_KQUEUE
        if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
        if (dir_fd_ >= 0) { ::close(dir_fd_); dir_fd_ = -1; }
#endif
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        wd_ = -1;
#endif
    }

#if HAND_CONFIG_WATCH_WIN32
    // (Re-)arm the asynchronous directory read. Returns false if the watch died.
    bool post_read() {
        if (!dir_handle_) return false;
        DWORD got = 0;
        const BOOL ok = ::ReadDirectoryChangesW(
            dir_handle_, buf_, sizeof buf_, FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
            &got, &ov_, nullptr);
        return ok || ::GetLastError() == ERROR_IO_PENDING;
    }
#endif

#if HAND_CONFIG_WATCH_KQUEUE
    // Re-open the config file fd and re-register its vnode filter. Called after
    // an atomic-rename save swaps the inode (the old file_fd_ went stale) or the
    // file was first created. The dir watch itself stays valid across this.
    void rearm_watch() {
        if (fd_ < 0 || path_.empty()) return;
        if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
        file_fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (file_fd_ < 0) return; // file gone for now; dir watch catches re-create
        struct kevent kev;
        EV_SET(&kev, file_fd_, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME | NOTE_DELETE | NOTE_ATTRIB, 0, nullptr);
        (void)::kevent(fd_, &kev, 1, nullptr, 0, nullptr);
    }
#endif

    int fd_ = -1;
    int wd_ = -1;
#if HAND_CONFIG_WATCH_WIN32
    HANDLE dir_handle_ = nullptr; // the watched directory, opened OVERLAPPED
    OVERLAPPED ov_{};             // in-flight ReadDirectoryChangesW state
    // DWORD-aligned: FILE_NOTIFY_INFORMATION is read out of this in place.
    alignas(DWORD) char buf_[4096]{};
#endif
#if HAND_CONFIG_WATCH_KQUEUE
    int dir_fd_ = -1;    // parent-directory fd (catches create/rename saves)
    int file_fd_ = -1;   // config-file fd (catches in-place writes)
    bool rearm_ = false; // set when a watched vnode was renamed/deleted
    std::string path_;   // full config path, for re-opening after rename
#endif
    std::string dir_, base_;
    std::uint64_t self_write_ms_ = 0;
};

} // namespace hand

#endif // HAND_CONFIG_WATCH_HPP
