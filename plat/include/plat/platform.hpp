// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/platform.hpp — the OS capability concepts and the Platform composition.
//
// Each OS facility is a CONCEPT: a backend PROVES it provides the facility by
// structure, and the compiler refuses any call to a facility the backend does
// not model. This is toe's App-refinement idiom generalized to the whole
// platform: Window / Waker / Clipboard / Sys / Gpu are orthogonal capabilities,
// and `Platform` is the product of the ones a host always needs.
//
//   Platform  ::=  Window  ∧  Waker  ∧  Gpu     (the required core)
//   (Clipboard, Sys, Chrome... are optional refinements a host queries with
//    `if constexpr (Clipboard<P>)`, folding away on backends that lack them —
//    exactly how toe handles DamageableApp / OverlayApp.)
//
// Header-only contract; concrete backends (wayland/x11/…) model these.

#ifndef PLAT_PLATFORM_HPP
#define PLAT_PLATFORM_HPP

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "plat/event.hpp"
#include "plat/frame.hpp"
#include "plat/gpu.hpp"

namespace plat {

// A wait deadline as a value (portable; the backend maps it to its primitive).
struct Deadline {
    std::int64_t ns = -1; // < 0 = block forever
    [[nodiscard]] static constexpr Deadline forever() noexcept { return {-1}; }
    [[nodiscard]] static constexpr Deadline nanos(std::int64_t n) noexcept { return {n < 0 ? 0 : n}; }
    [[nodiscard]] static constexpr Deadline millis(int ms) noexcept {
        return ms < 0 ? forever() : Deadline{static_cast<std::int64_t>(ms) * 1'000'000};
    }
};

// Which sources woke a wait. Extra fds (the tabs' PTYs) report by index bitset.
struct Woke {
    bool window = false;              // the display connection is readable
    std::uint64_t fds = 0;            // bit i set => the i-th watched fd is readable
};

// Window decorations mode the host requests (client-side => hand draws chrome).
enum class Decorations { server, client, none };

// Window control actions the host issues to drive its own chrome (CSD).
enum class WinAction { minimize, maximize, restore, close, begin_move, begin_resize };

// --- capability concepts ----------------------------------------------------

// Window: a resizable OS surface with a title and lifecycle.
template <class W>
concept Window = requires(W w, const W cw, std::string_view title, Decorations d, WinAction a) {
    { cw.size() } -> std::same_as<Size>;       // framebuffer size in px
    { cw.scale() } -> std::convertible_to<float>; // HiDPI factor
    { cw.should_close() } -> std::same_as<bool>;
    { w.set_title(title) } -> std::same_as<void>;
    { w.set_decorations(d) } -> std::same_as<void>; // server/client/none
    { w.window_action(a) } -> std::same_as<void>;   // minimize/maximize/move/resize/close
};

// Input: drain the OS event queue into the host's sink.
template <class I, class Sink>
concept Pollable = requires(I i, Sink sink) {
    { i.poll_events(sink) } -> std::same_as<void>;
};

// Waker: the ONE blocking step. Wait until the window OR any watched fd is
// readable, or the deadline elapses. toe/the host own no wait mechanism; the
// backend implements it with epoll/kqueue/MsgWait.
template <class K>
concept Waker = requires(K k, std::span<const int> fds, Deadline d) {
    { k.wait(fds, d) } -> std::same_as<Woke>;
};

// Clipboard (optional refinement): system clipboard + X11/Wayland PRIMARY.
template <class C>
concept Clipboard = requires(C c, std::string_view s) {
    { c.get_clipboard() } -> std::convertible_to<std::string>;
    { c.set_clipboard(s) } -> std::same_as<void>;
    { c.get_primary() } -> std::convertible_to<std::string>;
    { c.set_primary(s) } -> std::same_as<void>;
};

// Sys (optional refinement): misc OS services.
template <class S>
concept Sys = requires(S s, std::string_view url, std::string_view title, std::string_view body) {
    { s.open_url(url) } -> std::same_as<void>;
    { s.notify(title, body) } -> std::same_as<void>; // desktop notification
};

// --- the composed Platform --------------------------------------------------
// The required core every host relies on. Sink is the event-callback type the
// host passes to poll_events (deduced at the call site; here we require the
// member exists for SOME invocable — checked structurally via Pollable at use).
template <class P>
concept Platform = Window<P> && Waker<P> && requires(P p, Color clear) {
    // Gpu access: the platform exposes the device that renders into its window.
    { p.gpu() };                       // -> some Gpu& (checked at use with Gpu<...>)
    { p.begin_frame(clear) };          // convenience: forward to gpu().begin_frame
};

} // namespace plat

#endif // PLAT_PLATFORM_HPP
