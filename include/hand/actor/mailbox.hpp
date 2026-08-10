// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/actor/mailbox.hpp — the ONE cross-thread contact surface.
//
// The entire threading design rests on a single discipline: threads share NO
// mutable state; they communicate ONLY by moving immutable message VALUES
// through typed mailboxes. If the only thing that crosses a thread boundary is
// an owned value posted to a queue, there is no data race to guard — the
// architecture forbids it, not a mutex sprinkled at call sites. This is the
// Elm/actor model made concrete for C++ threads.
//
// Mailbox<Msg> is a move-only MPSC (many senders, one receiver) queue:
//   * post(Msg)      — any thread enqueues an owned message (moved in).
//   * drain(f)       — the OWNER thread pops all pending messages, calling f.
//   * wait_fd()      — an eventfd/pipe read end the owner can fold into its
//                      poll() set, so it blocks on "window OR a message" in one
//                      syscall (no busy-wait, no separate condvar timing).
//
// Msg must be move-constructible. Nothing here is templated on the transport;
// the fd makes it composable with epoll/poll alongside the window + PTY fds.

#ifndef HAND_ACTOR_MAILBOX_HPP
#define HAND_ACTOR_MAILBOX_HPP

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#elif !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hand {

template <class Msg>
class Mailbox {
public:
    Mailbox() { open_wakeup(); }
    ~Mailbox() { close_wakeup(); }
    Mailbox(const Mailbox &) = delete;
    Mailbox &operator=(const Mailbox &) = delete;
    Mailbox(Mailbox &&) = delete; // pinned: its fd is folded into a poll set
    Mailbox &operator=(Mailbox &&) = delete;

    // Enqueue an owned message from ANY thread and wake the owner's poll().
    void post(Msg m) {
        {
            std::lock_guard lk(mu_);
            queue_.push_back(std::move(m));
        }
        signal_wakeup();
    }

    // Pop every pending message on the OWNER thread, moving each into `f`.
    // Drains the wakeup fd so the next wait blocks until a NEW post arrives.
    template <class F>
    void drain(F &&f) {
        std::vector<Msg> local;
        {
            std::lock_guard lk(mu_);
            local.swap(queue_);
        }
        clear_wakeup();
        for (auto &m : local) f(std::move(m));
    }

    // The read end of the wakeup fd (eventfd on Linux, pipe elsewhere). Fold it
    // into the owner's poll()/epoll set. -1 if unavailable (falls back to a
    // timed drain).
    [[nodiscard]] int wait_fd() const noexcept { return rfd_; }

    // True if any message is pending (cheap peek; the owner still drains()).
    [[nodiscard]] bool pending() const {
        std::lock_guard lk(mu_);
        return !queue_.empty();
    }

private:
    mutable std::mutex mu_;
    std::vector<Msg> queue_;
    int rfd_ = -1; // read end (poll on this)
    int wfd_ = -1; // write end (signal via this); == rfd_ for eventfd

#if defined(__linux__)
    void open_wakeup() {
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        rfd_ = wfd_ = fd;
    }
    void signal_wakeup() {
        if (wfd_ >= 0) {
            std::uint64_t one = 1;
            [[maybe_unused]] ssize_t n = ::write(wfd_, &one, sizeof one);
        }
    }
    void clear_wakeup() {
        if (rfd_ >= 0) {
            std::uint64_t buf;
            while (::read(rfd_, &buf, sizeof buf) > 0) {
            }
        }
    }
    void close_wakeup() {
        if (rfd_ >= 0) ::close(rfd_);
        rfd_ = wfd_ = -1;
    }
#elif !defined(_WIN32)
    void open_wakeup() {
        int fds[2];
        if (::pipe(fds) == 0) {
            ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
            ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
            rfd_ = fds[0];
            wfd_ = fds[1];
        }
    }
    void signal_wakeup() {
        if (wfd_ >= 0) {
            char c = 1;
            [[maybe_unused]] ssize_t n = ::write(wfd_, &c, 1);
        }
    }
    void clear_wakeup() {
        if (rfd_ >= 0) {
            char buf[256];
            while (::read(rfd_, buf, sizeof buf) > 0) {
            }
        }
    }
    void close_wakeup() {
        if (rfd_ >= 0) ::close(rfd_);
        if (wfd_ >= 0) ::close(wfd_);
        rfd_ = wfd_ = -1;
    }
#else
    // Windows: no fd; the owner polls pending()/drain() on its message pump.
    void open_wakeup() {}
    void signal_wakeup() {}
    void clear_wakeup() {}
    void close_wakeup() {}
#endif
};

} // namespace hand

#endif // HAND_ACTOR_MAILBOX_HPP
