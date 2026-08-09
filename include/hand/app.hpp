// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand's windows — three distinct toe::App models, one per native backend.
//
// toe declares the whole host contract as `concept App` and drives it with the
// MONOMORPHIC `toe::run<App>`. hand provides three models: WaylandApp, X11App,
// OffscreenApp. Each is a thin HANDLE (pimpl) — one opaque pointer to the real
// backend surface, whose ~45 wl_*/xcb_* fields stay sealed in that backend's TU.
// The handle's methods are DIRECT, non-virtual calls into that TU: no vtable, no
// tag, no per-op branch. `toe::run<WaylandApp>` inlines to Wayland calls; a
// separate `toe::run<X11App>` inlines to X11 calls.
//
// The Wayland-vs-X11 choice is a SINGLE runtime decision at startup (Linux ships
// both backends in one binary), made once in `hand::run` — which then enters the
// one fully-specialised loop for the chosen backend. After that `if`, every
// frame op is a statically-known direct call. So dispatch cost is: one branch
// per PROCESS, zero per frame.
//
//     int main() { return hand::run(cfg, {"hand", {800, 500}}); }

#ifndef HAND_APP_HPP
#define HAND_APP_HPP

#include <string>
#include <string_view>
#include <vector>

#include "toe/app.hpp"
#include "toe/core/types.hpp"
#include "toe/terminal.hpp" // toe::Config

namespace hand {

// One backend handle. `Impl` is the concrete surface type (incomplete here,
// complete in its TU); the App owns it and forwards the toe::App contract to it
// with direct calls. Declared once, instantiated per backend below — so each
// App is its own distinct, monomorphic type.
//
// Ops are declared (not defined) here and defined in the backend TU where `Impl`
// is complete, so no native window-system type ever appears in this header.
template <class Impl>
class BackendApp {
public:
    // toe::App factory: open this backend. Returns a negative-carrying Result on
    // failure (or when the backend is unavailable), so hand::run can fall back.
    // Defined in the backend TU (where Impl is complete and knows how to open).
    [[nodiscard]] static toe::Result<BackendApp> open(const toe::WindowConfig &win);

    // --- toe::App contract: direct, non-virtual forwards to *impl_ ---------
    // Inline here but instantiated only inside the backend TU (via toe::run<App>
    // being called there), where Impl is complete — so no native type leaks and
    // every call is a plain, inlinable direct call. No vtable, no branch.
    void swap() { impl_->swap(); }
    void begin_frame(toe::PixelSize px, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        impl_->begin_frame(px, r, g, b);
    }
    void end_frame() { impl_->end_frame(); }
    void swap_damaged(toe::DamageRect d) { impl_->swap_damaged(d); }
    [[nodiscard]] toe::PixelSize pixel_size() const { return impl_->pixel_size(); }
    void poll_events(const toe::EventSink &sink) { impl_->poll_events(sink); }
    [[nodiscard]] int event_fd() const { return impl_->event_fd(); }
    [[nodiscard]] int repeat_fd() const { return impl_->repeat_fd(); }
    [[nodiscard]] bool should_close() const { return impl_->should_close(); }
    void flush() { impl_->flush(); }
    void set_title(std::string_view t) { impl_->set_title(t); }
    void set_clipboard(std::string_view t) { impl_->set_clipboard(t); }
    [[nodiscard]] std::string get_clipboard() { return impl_->get_clipboard(); }
    void open_url(std::string_view u) { impl_->open_url(u); }
    [[nodiscard]] bool overlay_active() const { return impl_->overlay_active(); }
    bool overlay_event(const toe::win::Event &ev) { return impl_->overlay_event(ev); }
    void overlay_render(toe::Terminal &t, toe::PixelSize px) { impl_->overlay_render(t, px); }
    void bind_terminal(toe::Terminal &t, toe::PixelSize px) { impl_->bind_terminal(t, px); }
    [[nodiscard]] toe::Readiness wait_readable(int pty_fd, toe::WaitDeadline d) {
        return impl_->wait_readable(pty_fd, d);
    }

    BackendApp(BackendApp &&o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
    BackendApp &operator=(BackendApp &&o) noexcept {
        if (this != &o) { delete impl_; impl_ = o.impl_; o.impl_ = nullptr; }
        return *this;
    }
    ~BackendApp() { delete impl_; }
    BackendApp(const BackendApp &) = delete;
    BackendApp &operator=(const BackendApp &) = delete;

    // Construct from an owned Impl* (used by open() in the backend TU).
    explicit BackendApp(Impl *impl) noexcept : impl_(impl) {}

private:
    Impl *impl_ = nullptr; // owned; type complete only in the backend's TU
};

// The three concrete surface types, forward-declared (defined in their TUs, in
// hand::platform). Each names a distinct App instantiation.
namespace platform {
class WaylandSurface;
class X11Surface;
class CocoaSurface;
class OffscreenSurface;
} // namespace platform

using WaylandApp = BackendApp<platform::WaylandSurface>;
using X11App = BackendApp<platform::X11Surface>;
using CocoaApp = BackendApp<platform::CocoaSurface>;
using OffscreenApp = BackendApp<platform::OffscreenSurface>;

// Which backend to force, or automatic (environment-driven) selection.
enum class Backend { automatic, wayland, x11, cocoa, offscreen };

// The ONE runtime decision. Picks a backend from the environment (unless forced)
// and enters that backend's fully-monomorphic `toe::run<...>`. This is the line
// main writes. Returns the child exit code (or a negative startup-failure code).
[[nodiscard]] int run(const toe::Config &cfg, const toe::WindowConfig &win = {},
                      Backend force = Backend::automatic,
                      const std::vector<std::string> &child_argv = {});

// Per-backend entries, each defined in its own TU where it instantiates
// `toe::run<ThatApp>` — the fully-monomorphic loop for that backend. hand::run
// dispatches to one of these after the single runtime choice. Returns the child
// exit code, or <0 if the window couldn't be opened (so run() can fall back).
[[nodiscard]] int run_wayland(const toe::Config &cfg, const toe::WindowConfig &win);
[[nodiscard]] int run_x11(const toe::Config &cfg, const toe::WindowConfig &win);
[[nodiscard]] int run_cocoa(const toe::Config &cfg, const toe::WindowConfig &win);
[[nodiscard]] int run_offscreen(const toe::Config &cfg, const toe::WindowConfig &win);

} // namespace hand

#endif // HAND_APP_HPP
