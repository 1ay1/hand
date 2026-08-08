// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::App — THE window. The one type main names, the one thing hand implements.
//
// toe declares the whole host contract as `concept App` (present, input,
// lifecycle, and the `open()` factory). hand has three native backends —
// Wayland, X11, offscreen — that are genuinely distinct implementations of that
// contract (a 45-field wl_* surface has nothing in common with an EGL pbuffer
// but the SHAPE). Rather than force main to know which, hand::App is the single
// window type: its `open()` picks a backend from the environment and holds the
// live one behind a thin interface.
//
// The one indirection this costs (a virtual call through AppBackend) is paid
// only at FRAME BOUNDARIES — swap/poll_events/event_fd/flush run ~60×/sec, never
// per byte or per glyph. The throughput hot path (VT parse, screen model,
// render) lives entirely inside toe and stays fully monomorphic. So hand::App
// reads as one clean window type at zero cost where cost matters.
//
// main writes exactly:  return toe::run<hand::App>(cfg, {"hand", {800, 500}});

#ifndef HAND_APP_HPP
#define HAND_APP_HPP

#include <memory>
#include <string>
#include <string_view>

#include "toe/app.hpp"
#include "toe/core/types.hpp"

namespace hand {

// The per-frame operations a concrete backend (Wayland/X11/offscreen) provides.
// An abstract base rather than a template because the backend is chosen at
// runtime from the environment; every method here is called at most once per
// frame, so the virtual dispatch never touches the throughput hot path.
struct AppBackend {
    virtual ~AppBackend() = default;

    virtual void swap() = 0;
    virtual void swap_damaged(toe::DamageRect d) = 0; // partial present; may fall back to swap()
    [[nodiscard]] virtual toe::PixelSize pixel_size() const = 0;
    virtual void poll_events(const toe::EventSink &sink) = 0;
    [[nodiscard]] virtual int event_fd() const = 0;
    [[nodiscard]] virtual int repeat_fd() const = 0; // -1 if the backend has no repeat timer
    [[nodiscard]] virtual bool should_close() const = 0;
    virtual void flush() = 0;
    virtual void set_title(std::string_view t) = 0;
    virtual void set_clipboard(std::string_view t) = 0;
    [[nodiscard]] virtual std::string get_clipboard() = 0;
};

// Which backend to force, or automatic (environment-driven) selection.
enum class Backend { automatic, wayland, x11, offscreen };

// THE window. Models toe::App. Opens the right backend and forwards the
// per-frame contract to it. `open()` is the factory toe::run<hand::App> calls.
class App {
public:
    // toe::App factory. Picks a backend (env-driven unless `force` says
    // otherwise), opens it, and wraps it. A current GL context is live on
    // return. Returns a negative-carrying Result on failure.
    [[nodiscard]] static toe::Result<App> open(const toe::WindowConfig &win,
                                               Backend force = Backend::automatic);

    // --- toe::App contract (forwarded to the live backend) -----------------
    void swap() { backend_->swap(); }
    void swap_damaged(toe::DamageRect d) { backend_->swap_damaged(d); }
    [[nodiscard]] toe::PixelSize pixel_size() const { return backend_->pixel_size(); }
    void poll_events(const toe::EventSink &sink) { backend_->poll_events(sink); }
    [[nodiscard]] int event_fd() const { return backend_->event_fd(); }
    [[nodiscard]] int repeat_fd() const { return backend_->repeat_fd(); }
    [[nodiscard]] bool should_close() const { return backend_->should_close(); }
    void flush() { backend_->flush(); }
    void set_title(std::string_view t) { backend_->set_title(t); }
    void set_clipboard(std::string_view t) { backend_->set_clipboard(t); }
    [[nodiscard]] std::string get_clipboard() { return backend_->get_clipboard(); }

    App(App &&) noexcept = default;
    App &operator=(App &&) noexcept = default;

private:
    explicit App(std::unique_ptr<AppBackend> b) noexcept : backend_(std::move(b)) {}
    std::unique_ptr<AppBackend> backend_;
};

// Each backend TU provides its opener; backend.cpp does the env selection.
// Returns nullptr (not an error) when a backend is simply unavailable, so the
// selector can fall through to the next; a non-null unique_ptr on success.
std::unique_ptr<AppBackend> open_wayland(const toe::WindowConfig &win);
std::unique_ptr<AppBackend> open_x11(const toe::WindowConfig &win);
std::unique_ptr<AppBackend> open_offscreen(const toe::WindowConfig &win);

} // namespace hand

#endif // HAND_APP_HPP
