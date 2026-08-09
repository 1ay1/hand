// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Reactor — hand's readiness core: the ONE place the single-threaded loop blocks.
// A type-safe, allocation-free, syscall-minimal wrapper over Linux epoll.
//
// This lives in HAND, not toe. toe is the portable engine (it targets macOS and
// Windows hosts too — see toe's README) and must not bake in a Linux-only
// syscall like epoll. Event-loop / OS readiness multiplexing is the HOST's job;
// hand is the native Linux frontend, so the fast Linux reactor lives here and
// hand drives toe with it.
//
// ─── why epoll, and why level-triggered ────────────────────────────────────
// A terminal waits on a tiny, fixed set of fds (pty · window · key-repeat) and
// is latency-bound: it blocks thousands of times a second, so the per-wait cost
// IS the hot path. poll(2) (the old PollSet) re-marshals the whole pollfd[] into
// the kernel every call — O(n) copy, and the kernel rescans every fd. epoll puts
// the interest set in the kernel ONCE (at registration) and each wait returns
// only the fds that are ready — O(ready), marshalling nothing.
//
// We register LEVEL-triggered. The man page and the I/O-model literature single
// out "interactive terminal / stdin" as the level-triggered case: child output
// arrives incrementally and we deliberately drain at a byte budget (not to
// EAGAIN), so we want epoll to keep reporting readiness until we've actually
// consumed it. Edge-triggered would silently strand un-drained bytes.
//
// Timeouts use epoll_pwait2() (Linux ≥ 5.11) for NANOSECOND precision, so the
// cursor-blink / inline-image cadence is exact instead of rounded to whole
// milliseconds; it degrades to epoll_wait() on older kernels.
//
// ─── why this is type-safe ─────────────────────────────────────────────────
// The interest set is fixed IN THE TYPE. `Reactor<Pty, Win, Timer>` names its
// three sources by phantom tag; each `arm<Tag>(fd)` registers one, and wait()
// returns a `Ready` set you query as `r.get<Pty>()`. Querying a tag the Reactor
// was not parameterised on is a COMPILE error — you cannot ask about a source
// you never declared, and there are no raw fd integers to confuse. Readiness is
// a std::bitset indexed by the tag's compile-time position, so get<Tag>() is a
// single bit test, no scan.

#ifndef HAND_REACTOR_HPP
#define HAND_REACTOR_HPP

#include <array>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <tuple>
#include <type_traits>
#include <utility>
#include <functional>

#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "toe/app.hpp" // toe::Readiness, toe::WaitDeadline

namespace hand {

// A wait deadline as a first-class value: block forever, or up to a duration
// with nanosecond resolution (honoured via epoll_pwait2 when available).
class Deadline {
public:
    static constexpr Deadline forever() noexcept { return Deadline{-1}; }
    static Deadline nanos(std::int64_t ns) noexcept { return Deadline{ns < 0 ? 0 : ns}; }
    static Deadline from(std::chrono::nanoseconds d) noexcept { return nanos(d.count()); }
    static Deadline millis(int ms) noexcept {
        return ms < 0 ? forever() : nanos(static_cast<std::int64_t>(ms) * 1'000'000);
    }
    [[nodiscard]] bool blocks_forever() const noexcept { return ns_ < 0; }
    [[nodiscard]] std::int64_t ns() const noexcept { return ns_; }

private:
    explicit constexpr Deadline(std::int64_t ns) noexcept : ns_(ns) {}
    std::int64_t ns_{-1};
};

namespace detail {
// index_of<T, Tags...> — the compile-time position of tag T in the pack, or a
// hard error (via the static_assert in the accessor) if T isn't a member.
template <typename T, typename... Ts>
struct index_of;
template <typename T, typename... Rest>
struct index_of<T, T, Rest...> : std::integral_constant<std::size_t, 0> {};
template <typename T, typename U, typename... Rest>
struct index_of<T, U, Rest...>
    : std::integral_constant<std::size_t, 1 + index_of<T, Rest...>::value> {};

template <typename T, typename... Ts>
inline constexpr bool contains = (std::is_same_v<T, Ts> || ...);
} // namespace detail

// The result of one wait(): which of the Reactor's declared sources are ready.
// Query by tag; the tag must be one the Reactor was parameterised on, checked at
// compile time. A single bit test — no fd scan.
template <typename... Tags>
class Ready {
public:
    template <typename Tag>
    [[nodiscard]] bool get() const noexcept {
        static_assert(detail::contains<Tag, Tags...>,
                      "Ready::get<Tag>: this Tag is not one of the Reactor's sources");
        return bits_.test(detail::index_of<Tag, Tags...>::value);
    }
    [[nodiscard]] bool any() const noexcept { return bits_.any(); }

    template <typename Tag>
    void set() noexcept {
        bits_.set(detail::index_of<Tag, Tags...>::value);
    }

private:
    std::bitset<sizeof...(Tags)> bits_{};
};

// The Reactor. `Tags...` names its fixed interest set. Non-copyable (owns an
// epoll fd). Register each source once with arm<Tag>(fd); a source may be left
// unarmed (fd < 0) — e.g. a backend with no key-repeat timer — and simply never
// reports ready.
template <typename... Tags>
class Reactor {
    static constexpr std::size_t N = sizeof...(Tags);

public:
    Reactor() : epfd_(::epoll_create1(EPOLL_CLOEXEC)) {}
    ~Reactor() {
        if (epfd_ >= 0) ::close(epfd_);
    }
    Reactor(const Reactor &) = delete;
    Reactor &operator=(const Reactor &) = delete;
    Reactor(Reactor &&o) noexcept : epfd_(o.epfd_), fds_(o.fds_) { o.epfd_ = -1; }

    [[nodiscard]] bool valid() const noexcept { return epfd_ >= 0; }

    // Register `fd` for readability under the compile-time source Tag. A negative
    // fd is accepted and left unarmed (the source is absent on this backend), so
    // callers need not special-case optional sources. Level-triggered: the
    // kernel keeps reporting readiness until the fd is drained.
    template <typename Tag>
    Reactor &arm(int fd) noexcept {
        static_assert(detail::contains<Tag, Tags...>,
                      "Reactor::arm<Tag>: this Tag is not one of the Reactor's sources");
        constexpr std::size_t i = detail::index_of<Tag, Tags...>::value;
        fds_[i] = fd;
        if (fd >= 0 && epfd_ >= 0) {
            ::epoll_event ev{};
            ev.events = EPOLLIN; // level-triggered (no EPOLLET) — see header note
            ev.data.u32 = static_cast<std::uint32_t>(i);
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
        }
        return *this;
    }

    // Block until any armed source is readable or the deadline elapses. Returns
    // the ready set (empty on timeout). One syscall, marshals nothing.
    [[nodiscard]] Ready<Tags...> wait(Deadline d) noexcept {
        Ready<Tags...> r;
        std::array<::epoll_event, N == 0 ? 1 : N> evs{};
        const int n = wait_syscall(evs.data(), static_cast<int>(evs.size()), d);
        for (int k = 0; k < n; ++k) mark(r, evs[static_cast<std::size_t>(k)].data.u32);
        return r;
    }

private:
    // Fold over the tag pack to set the bit for the ready source index `idx`.
    void mark(Ready<Tags...> &r, std::uint32_t idx) noexcept {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((idx == Is ? (mark_at<Is>(r), void()) : void()), ...);
        }(std::make_index_sequence<N>{});
    }
    template <std::size_t I>
    void mark_at(Ready<Tags...> &r) noexcept {
        r.template set<std::tuple_element_t<I, std::tuple<Tags...>>>();
    }

    int wait_syscall(::epoll_event *evs, int maxev, Deadline d) noexcept {
#if defined(__NR_epoll_pwait2)
        // Nanosecond-precise timeout. `nullptr` timespec == block forever.
        ::timespec ts{};
        ::timespec *tp = nullptr;
        if (!d.blocks_forever()) {
            ts.tv_sec = static_cast<std::time_t>(d.ns() / 1'000'000'000);
            ts.tv_nsec = static_cast<long>(d.ns() % 1'000'000'000);
            tp = &ts;
        }
        const long rc2 = ::syscall(__NR_epoll_pwait2, epfd_, evs, maxev, tp, nullptr, 0);
        if (rc2 >= 0 || errno != ENOSYS) return static_cast<int>(rc2 < 0 ? 0 : rc2);
        // Fall through to epoll_wait on kernels without epoll_pwait2.
#endif
        const int ms = d.blocks_forever()
                           ? -1
                           : static_cast<int>((d.ns() + 999'999) / 1'000'000); // round up
        const int rc = ::epoll_wait(epfd_, evs, maxev, ms);
        return rc < 0 ? 0 : rc;
    }

    int epfd_{-1};
    std::array<int, N> fds_{}; // the armed fds, by tag index (informational)
};

// --- the terminal's readiness wait, as toe::App expects it -----------------
// The sources a terminal blocks on, as compile-time tags.
struct PtySource {};
struct WindowSource {};
struct RepeatSource {};
// The config-file watcher (inotify). Like RepeatSource it is a purely HOST-
// internal wakeup: toe never learns it exists. When it fires we run the host's
// reload hook and return a spurious (all-false) Readiness — which the loop is
// documented to tolerate — so the engine's contract is untouched.
struct ConfigSource {};

// A Reactor pre-typed for a terminal's fixed interest set. A backend holds one,
// arms its window + repeat fds once at open, and re-arms the PTY per session.
using TermReactor = Reactor<PtySource, WindowSource, RepeatSource, ConfigSource>;

// Stateful helper a backend embeds to implement toe::App::wait_readable. It owns
// the epoll reactor and arms each source EXACTLY ONCE, the first time it is seen
// (window + repeat at construction, the PTY on the first wait once toe supplies
// its fd). Registration is O(1) and happens once per fd for the session's life;
// every subsequent wait is a single epoll_pwait2 that marshals nothing.
class TerminalWait {
public:
    TerminalWait() = default;

    // Set the window (compositor/X connection) and key-repeat timer fds. Call
    // once the backend actually has them (after connecting). Either may be -1.
    void set_fds(int window_fd, int repeat_fd) noexcept {
        window_fd_ = window_fd;
        repeat_fd_ = repeat_fd;
    }

    // Fold a config-watcher fd into the wait, with the hook to run when it wakes
    // (drain the inotify events + hot-reload). The reactor stays oblivious to
    // what the fd means — it just calls the hook. Pass -1 fd to disable.
    void watch_config(int fd, std::function<void()> on_change) noexcept {
        config_fd_ = fd;
        on_config_ = std::move(on_change);
    }

    // toe::App::wait_readable. Arms each source exactly once, the first time it
    // is seen with a valid fd, then blocks on a single epoll_pwait2.
    [[nodiscard]] toe::Readiness wait(int pty_fd, toe::WaitDeadline d) noexcept {
        if (pty_fd != armed_pty_) { r_.arm<PtySource>(pty_fd); armed_pty_ = pty_fd; }
        if (window_fd_ != armed_win_) { r_.arm<WindowSource>(window_fd_); armed_win_ = window_fd_; }
        if (repeat_fd_ != armed_rep_) { r_.arm<RepeatSource>(repeat_fd_); armed_rep_ = repeat_fd_; }
        if (config_fd_ != armed_cfg_) { r_.arm<ConfigSource>(config_fd_); armed_cfg_ = config_fd_; }
        const Deadline dd = d.blocks_forever() ? Deadline::forever() : Deadline::nanos(d.ns);
        const Ready<PtySource, WindowSource, RepeatSource, ConfigSource> ready = r_.wait(dd);
        // The config wake is handled entirely here (host-internal), then folded
        // out of the Readiness — exactly like the repeat timer. toe sees only
        // {pty, window}; a config-only wake looks like a benign spurious one.
        if (ready.template get<ConfigSource>() && on_config_) on_config_();
        return toe::Readiness{.pty = ready.get<PtySource>(), .window = ready.get<WindowSource>()};
    }

    [[nodiscard]] bool valid() const noexcept { return r_.valid(); }

private:
    TermReactor r_;
    int window_fd_ = -1, repeat_fd_ = -1, config_fd_ = -1;
    int armed_pty_ = -2, armed_win_ = -2, armed_rep_ = -2, armed_cfg_ = -2; // sentinels
    std::function<void()> on_config_{};
};

} // namespace hand

#endif // HAND_REACTOR_HPP
