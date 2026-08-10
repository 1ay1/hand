// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/run_tabbed.hpp — the tabbed main loop, templated on a toe::App
// backend (X11App / WaylandApp). This is the impure top level that makes the
// whole Elm/actor architecture LIVE:
//
//   * owns a GuiRuntime (which owns the GuiModel + the TabActor threads),
//   * gives it a SpawnFn (make a toe::Terminal per tab) and a PresentFn
//     (render the active tab + chrome via TabbedView, under the tab's
//     render_lock — the one place the GUI reads a grid an actor writes),
//   * each iteration WAITS on { the window event fd, the GUI mailbox fd } in
//     one blocking wait (actors post to the mailbox + signal its fd; the GUI
//     never polls PTYs — the actor threads own those),
//   * translates window events -> GuiMsg (input, resize, close, tab chords,
//     chrome clicks) and pumps the runtime.
//
// The single-terminal toe::run<App> is untouched; this is a parallel entry the
// backend selects for tabbed mode.

#ifndef HAND_GUI_RUN_TABBED_HPP
#define HAND_GUI_RUN_TABBED_HPP

#include <chrono>
#include <optional>
#include <string>
#include <variant>

#include "hand/actor/tab_actor.hpp"
#include "hand/gui/message.hpp"
#include "hand/gui/runtime.hpp"
#include "hand/tabbed_view.hpp"
#include "toe/app.hpp"
#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

namespace hand {

namespace detail {
inline std::uint64_t now_ms_() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace detail

// Translate one backend window Event into a GuiMsg and post it to the runtime.
// Tab chords (Ctrl+Shift+T/W and Ctrl+Tab) become tab-management messages;
// everything else the terminal cares about becomes ForwardBytes/keys via the
// focused actor. Kept simple + explicit; the closed GuiMsg sum keeps it honest.
template <class Rt>
inline void translate_event(Rt &rt, const toe::win::Event &ev) {
    using namespace toe::win;
    std::visit(
        [&](auto &&e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, CloseRequested>) {
                rt.post(WinCloseReq{});
            } else if constexpr (std::is_same_v<T, Resized>) {
                rt.post(WinResized{e.size.w, e.size.h});
            } else if constexpr (std::is_same_v<T, FocusChanged>) {
                rt.post(WinFocus{e.focused});
            } else if constexpr (std::is_same_v<T, KeyPressed>) {
                // Tab chords first (Ctrl+Shift+T new, Ctrl+Shift+W close,
                // Ctrl+Tab / Ctrl+Shift+Tab cycle). Otherwise encode the key for
                // the child via toe's keymap and forward the bytes.
                const auto &k = e.key;
                if (k.mods.ctrl && k.mods.shift) {
                    if (const auto *ti = std::get_if<toe::TextInput>(&k.key)) {
                        if (ti->utf8 == "T" || ti->utf8 == "t") { rt.post(NewTab{}); return; }
                        if (ti->utf8 == "W" || ti->utf8 == "w") { rt.post(CloseTab{}); return; }
                    }
                }
                if (k.mods.ctrl) {
                    if (const auto *sk = std::get_if<toe::SpecialKey>(&k.key)) {
                        if (*sk == toe::SpecialKey::Tab) {
                            rt.post(k.mods.shift ? GuiMsg{PrevTab{}} : GuiMsg{NextTab{}});
                            return;
                        }
                    }
                }
                // Not a chord: hand the whole KeyEvent to the focused tab; its
                // Session encodes it (send_key) with the right mode/kitty flags.
                rt.post(ForwardKey{k});
            } else if constexpr (std::is_same_v<T, TextEntered>) {
                rt.post(ForwardText{std::string{e.utf8}});
            }
            // Mouse events: routed to the focused tab / chrome by a later pass;
            // for the first live cut, the terminal's own mouse handling suffices
            // through ForwardBytes when apps request it. (Chrome click routing
            // is added next.)
        },
        ev);
}

// Enter the tabbed loop. `app` is an opened backend; `cfg` is the terminal
// config used to spawn each tab's Terminal.
template <class App>
[[nodiscard]] int run_tabbed(App &app, const toe::Config &cfg) {
    const toe::PixelSize px0 = app.pixel_size();

    TabbedView view;

    // SpawnFn: a new tab is a fresh toe::Terminal at the current window size.
    // (The child inherits the process cwd; per-tab chdir is a later refinement.)
    auto spawn = [&](const std::string &cwd) -> std::optional<toe::Terminal> {
        (void)cwd;
        auto t = toe::Terminal::create(cfg, app.pixel_size());
        if (!t) return std::nullopt;
        return std::move(*t);
    };

    // PresentFn: render the active tab's grid + chrome into the window. Runs on
    // the GUI thread; takes the tab's render_lock so the actor isn't mutating
    // the grid mid-draw (the one synchronized object, per-tab).
    GuiRuntime *rt_ptr = nullptr; // set below; present needs the frame counter
    auto present = [&](TabActor &active) {
        const toe::PixelSize px = app.pixel_size();
        app.begin_frame(px, 23, 23, 28, 1.0f); // clear; toe overdraws its own bg
        {
            std::lock_guard lk(active.render_lock());
            if (auto *s = active.terminal().poll().running) {
                auto rc = toe::gfx::RenderContext::adopt_current();
                s->render(rc, px);
                // Chrome overlay from the GUI model (frame drives spinner/pulse).
                view.render_chrome(rc, *s, rt_ptr->model(), px,
                                   static_cast<std::uint32_t>(rt_ptr->model().frame()));
            }
        }
        app.end_frame();
        app.swap();
    };

    GuiRuntime rt{std::move(spawn), std::move(present)};
    rt_ptr = &rt;
    rt.set_title_ = [&](const std::string &t) { app.set_title(t); };
    rt.start(); // spawn the first tab's actor

    std::uint64_t frame = 0;
    const std::uint64_t start = detail::now_ms_();
    while (!app.should_close() && !rt.done()) {
        // Translate any pending window events into GuiMsgs.
        app.poll_events([&](const toe::win::Event &ev) { translate_event(rt, ev); });

        // A ~60fps tick drives the chrome spinner / attention pulse.
        rt.post(Tick{frame++});

        // Process everything (spawns, input routing, present).
        rt.pump();

        // Block until the window has events OR an actor posted to the mailbox.
        // We reuse wait_readable, passing the GUI mailbox fd as the "pty" fd —
        // the GUI never waits on real PTYs (the actor threads own those).
        app.wait_readable(rt.mailbox().wait_fd(), toe::WaitDeadline::millis(16));
        (void)start;
    }
    return 0;
}

} // namespace hand

#endif // HAND_GUI_RUN_TABBED_HPP
