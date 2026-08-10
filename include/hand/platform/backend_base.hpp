// SPDX-License-Identifier: LGPL-2.0-or-later
//
// BackendBase — the shared half of every hand window backend.
//
// `toe::App` (toe/app.hpp) is the CONTRACT: ~20 operations a host must provide.
// But only a handful of those are genuinely platform-specific. The rest —
// overlay routing, the settings/help panes, the config watcher, the keybindings
// that open them, the "is this key ours or the child's" decision — are identical
// on Wayland, X11, Cocoa and Win32, and were being copy-pasted into each backend
// (where they inevitably drifted: the Win32 port shipped without the settings
// keybinding at all, because that logic lived four times in four files).
//
// This CRTP base owns everything portable. A backend inherits it and implements
// ONLY what the OS actually differs on:
//
//   ── required (the OS genuinely differs) ─────────────────────────────────
//     static Result<unique_ptr<Impl>> open(title, size)
//     void begin_frame(px, r, g, b, a) / void end_frame()
//     void swap()
//     PixelSize pixel_size() const
//     void poll_events(sink)
//     bool should_close() const
//     Readiness wait_readable(pty_fd, deadline)
//     void set_title(sv) / set_clipboard(sv) / get_clipboard() / open_url(sv)
//
//   ── provided here (override only if the OS can do better) ───────────────
//     flush()               no-op          (Wayland overrides: wl_display flush)
//     swap_damaged(rect)    -> swap()      (Wayland overrides: buffer damage)
//     event_fd/repeat_fd    -1             (Linux overrides with real fds)
//     overlay_active/_event/_render        (settings + help panes)
//     bind_terminal(term, px)              (binds panes, bell, theme, watcher)
//
// The payoff is concrete: a new platform is "implement ~10 obvious functions",
// and a fix to shared behaviour lands on every platform at once instead of
// three-quarters of them.

#ifndef HAND_PLATFORM_BACKEND_BASE_HPP
#define HAND_PLATFORM_BACKEND_BASE_HPP

#include <string>
#include <string_view>

#include "hand/platform/settings_host.hpp"
#include "hand/platform/surface.hpp"
#include "toe/app.hpp"
#include "toe/terminal.hpp"

namespace hand::platform {

// The chords hand reserves for itself, in platform-neutral terms. A backend
// translates its native key event into one of these and asks the base to handle
// it; the base owns what each one DOES, so the bindings can never disagree
// across platforms.
enum class Chord {
    None,
    ToggleSettings, // Ctrl+Shift+,   (macOS: Cmd+,)
    ToggleHelp,     // Ctrl+Shift+?   (macOS: Cmd+?)
};

// `Derived` is the concrete surface (Win32Surface, X11Surface, ...). CRTP lets
// the base call into it with zero virtual dispatch, preserving the monomorphic
// `toe::run<App>` property the whole design rests on.
template <typename Derived>
class BackendBase {
public:
    // --- overlay panes (settings + help) -----------------------------------
    // Identical on every platform: the panes are drawn by hand's own glyph UI
    // into the terminal grid, so there is nothing OS-specific to do.
    [[nodiscard]] bool overlay_active() const noexcept { return settings_.active(); }
    bool overlay_event(const toe::win::Event &ev) { return settings_.handle(ev); }
    void overlay_render(toe::Terminal &term, toe::PixelSize px) { settings_.render(term, px); }

    // --- defaults a backend may override -----------------------------------
    // flush(): push buffered protocol traffic. Only Wayland has any (its
    // wl_display request queue); D3D11/Cocoa/X11 present synchronously.
    void flush() {}

    // Present only the damaged region when the platform supports it. The
    // default presents everything, which is always correct.
    void swap_damaged(toe::DamageRect) { self().swap(); }

    // Readiness sources as pollable fds. Meaningful on Linux (the epoll
    // reactor); -1 elsewhere, where wait_readable() does the whole job.
    [[nodiscard]] int event_fd() const noexcept { return -1; }
    [[nodiscard]] int repeat_fd() const noexcept { return -1; }

    // --- terminal binding ---------------------------------------------------
    // Called once, after the Terminal exists. Wires the panes to the live
    // session: the bell handler, the startup theme/palette, and the config-file
    // watcher. Every backend needs exactly this, so it lives here; a backend
    // that must ALSO register the watcher with an fd-based reactor overrides
    // and calls this first.
    void bind_terminal(toe::Terminal &term, toe::PixelSize px) {
        (void)px; // reload reads the live pixel_size() (the window may resize)
        term_ = &term;
        settings_.bind();
        if (auto *s = term.poll().running) {
            settings_.install_bell(*s);
            settings_.apply_startup(*s, self().pixel_size());
        }
    }

    // --- reserved chords ----------------------------------------------------
    // The ONE place that decides what hand's own keybindings do. A backend
    // classifies its native key event into a Chord and calls this; returning
    // true means "consumed, do not forward to the child".
    bool handle_chord(Chord c) {
        switch (c) {
        case Chord::ToggleSettings: settings_.toggle(); return true;
        case Chord::ToggleHelp: settings_.toggle_help(); return true;
        case Chord::None: break;
        }
        return false;
    }

    // Re-read the config file after the watcher fired. Backends call this from
    // whichever readiness mechanism they use.
    void reload_config_if_watched() {
        if (term_) settings_.on_config_fd_ready(*term_, self().pixel_size());
    }

protected:
    // Non-virtual, non-polymorphic: this type is never used through a base
    // pointer, only as a mixin. Protected dtor keeps that honest.
    ~BackendBase() = default;

    [[nodiscard]] SettingsHost &settings() noexcept { return settings_; }
    [[nodiscard]] const SettingsHost &settings() const noexcept { return settings_; }
    [[nodiscard]] toe::Terminal *terminal() noexcept { return term_; }

    SettingsHost settings_{};
    toe::Terminal *term_ = nullptr; // bound in bind_terminal; for config reloads

private:
    [[nodiscard]] Derived &self() noexcept { return static_cast<Derived &>(*this); }
    [[nodiscard]] const Derived &self() const noexcept {
        return static_cast<const Derived &>(*this);
    }
};

} // namespace hand::platform

#endif // HAND_PLATFORM_BACKEND_BASE_HPP
