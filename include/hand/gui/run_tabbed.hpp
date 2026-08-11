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
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>

#include "hand/actor/tab_actor.hpp"
#include "hand/gui/message.hpp"
#include "hand/gui/runtime.hpp"
#include "hand/platform/backend_base.hpp" // Chord + classify_chord (shared, one place)
#include "hand/platform/posix_pty.hpp" // spawn_pty (fresh PTY per tab)
#include "hand/platform/sokol_gl.hpp" // GL (glViewport) with prototypes
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

// Diagnostic: with HAND_TABS_TRACE=1 in the env, log every window event that
// reaches the tabbed loop's sink to stderr — pinpoints whether input arrives.
inline bool trace_on() {
    static const bool on = std::getenv("HAND_TABS_TRACE") != nullptr;
    return on;
}
inline void trace_event(const toe::win::Event &ev) {
    std::visit(
        [](auto &&e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, toe::win::KeyPressed>) {
                const auto &k = e.key;
                const char *kind = std::get_if<toe::TextInput>(&k.key) ? "text" : "special";
                const char *tk = k.kind == toe::KeyEvent::Kind::press    ? "press"
                                 : k.kind == toe::KeyEvent::Kind::repeat ? "repeat"
                                                                         : "release";
                std::string tx = std::get_if<toe::TextInput>(&k.key)
                                     ? std::get<toe::TextInput>(k.key).utf8 : std::string{};
                std::fprintf(stderr, "[tabs] Key(%s) %s '%s' ctrl=%d shift=%d alt=%d\n", tk, kind,
                             tx.c_str(), k.mods.ctrl, k.mods.shift, k.mods.alt);
            } else if constexpr (std::is_same_v<T, toe::win::TextEntered>) {
                std::fprintf(stderr, "[tabs] TextEntered '%.*s'\n",
                             static_cast<int>(e.utf8.size()), e.utf8.data());
            } else if constexpr (std::is_same_v<T, toe::win::MouseDown>) {
                std::fprintf(stderr, "[tabs] MouseDown %d,%d\n", e.x, e.y);
            } else {
                std::fprintf(stderr, "[tabs] (other event)\n");
            }
        },
        ev);
}

// Turn an OSC 7 cwd ("file://host/abs/path" or a bare path) into a filesystem
// path suitable for chdir(). Returns empty when it can't (=> don't chdir).
inline std::string osc7_to_path(std::string_view cwd) {
    if (cwd.empty()) return {};
    constexpr std::string_view kFile = "file://";
    if (cwd.substr(0, kFile.size()) == kFile) {
        std::string_view rest = cwd.substr(kFile.size());
        const auto slash = rest.find('/'); // skip the host component
        if (slash == std::string_view::npos) return {};
        return std::string{rest.substr(slash)};
    }
    return cwd.front() == '/' ? std::string{cwd} : std::string{};
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
                const auto &k = e.key;
                // Only PRESS drives chords + input. Release/repeat of a chord
                // must not re-fire it (that was the "events fire twice" bug);
                // key REPEAT still forwards to the shell (held-key autorepeat).
                const bool is_press = k.kind == toe::KeyEvent::Kind::press;
                const bool is_repeat = k.kind == toe::KeyEvent::Kind::repeat;

                // ONE chord layer via the shared classifier. A recognised chord
                // is CONSUMED here (never forwarded to the shell), on press only.
                const platform::Chord chord = platform::classify_chord(k);
                if (chord != platform::Chord::None) {
                    if (is_press) {
                        switch (chord) {
                        case platform::Chord::NewTab: rt.post(NewTab{}); break;
                        case platform::Chord::CloseTab: rt.post(CloseTab{}); break;
                        case platform::Chord::NextTab: rt.post(NextTab{}); break;
                        case platform::Chord::PrevTab: rt.post(PrevTab{}); break;
                        // Settings/Help/Search overlays aren't wired in tabbed
                        // mode yet; consume them so they don't type into the
                        // shell (a later pass opens the per-tab overlays).
                        default: break;
                        }
                    }
                    return; // consumed: never reaches the terminal
                }
                // Not a chord: forward the key to the focused tab on press OR
                // repeat (so held keys autorepeat); drop bare releases.
                if (is_press || is_repeat) rt.post(ForwardKey{k});
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
// config (its .source is IGNORED here — each tab spawns its own PTY); `base_sc`
// carries argv/term/grid so every tab's child is spawned the same way.
template <class App>
[[nodiscard]] int run_tabbed(App &app, const toe::Config &cfg, const SpawnCommand &base_sc) {
    const toe::PixelSize px0 = app.pixel_size();

    TabbedView view;

    // SpawnFn: a new tab is a FRESH forkpty'd child (its own PTY + AdoptFd), so
    // tabs never share an fd. `cwd` (from the focused tab's OSC 7) chdir's the
    // child before exec, so a new tab opens in the same directory.
    auto spawn = [&](const std::string &cwd) -> std::optional<toe::Terminal> {
        SpawnCommand sc = base_sc;
        const std::string dir = detail::osc7_to_path(cwd);
        if (!dir.empty()) {
            sc.pre_exec = [dir] {
                // async-signal-safe: chdir is on the safe list.
                if (::chdir(dir.c_str()) != 0) { /* ignore: keep inherited cwd */ }
            };
        }
        auto fd = spawn_pty(sc);
        if (!fd) return std::nullopt;
        toe::Config c = cfg;
        c.source = *fd;
        auto t = toe::Terminal::create(c, app.pixel_size());
        if (!t) return std::nullopt;
        return std::move(*t);
    };

    // PresentFn: render the active tab's grid + chrome into the window. The
    // chrome occupies a reserved strip at the TOP; the terminal renders into the
    // area below it. GL's origin is bottom-left, so glViewport(0,0,w,h-chrome)
    // places the terminal in the lower part = below the top chrome strip. The
    // terminal is resized to that reduced area so its grid fits exactly.
    GuiRuntime *rt_ptr = nullptr; // set below; present needs the frame counter
    auto present = [&](TabActor &active) {
        const toe::PixelSize px = app.pixel_size();
        app.begin_frame(px, 23, 23, 28, 1.0f); // clear; toe overdraws its own bg
        {
            std::lock_guard lk(active.render_lock());
            if (auto *s = active.terminal().poll().running) {
                const toe::Extent cell = s->cell_size();
                const int chrome_h =
                    (cell.rows > 0) ? ChromeBar::kRows * cell.rows : 0;
                const toe::PixelSize term_px{px.w, std::max(1, px.h - chrome_h)};

                // Keep the terminal sized to the area BELOW the chrome.
                if (s->grid_size().cols * cell.cols != term_px.w ||
                    s->grid_size().rows * cell.rows != term_px.h) {
                    s->resize(term_px);
                }

                auto rc = toe::gfx::RenderContext::adopt_current();
                // Terminal into the lower (h - chrome_h) region.
                glViewport(0, 0, term_px.w, term_px.h);
                s->render(rc, term_px);
                // Chrome overlay across the FULL window (its own top row).
                glViewport(0, 0, px.w, px.h);
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
    // CSD window controls: route the chrome's –/□ buttons to the backend.
    rt.window_ctl_ = [&](WinCtl a) {
        app.window_action(a == WinCtl::minimize ? 0 : 1); // 0=minimize, 1=toggle-max
    };
    rt.start(); // spawn the first tab's actor

    std::uint64_t frame = 0;
    const std::uint64_t start = detail::now_ms_();
    while (!app.should_close() && !rt.done()) {
        // Translate any pending window events into GuiMsgs. Mouse-down on the
        // chrome row is resolved here (needs the view's cell size); everything
        // else goes through translate_event.
        app.poll_events([&](const toe::win::Event &ev) {
            if (detail::trace_on()) detail::trace_event(ev);
            if (const auto *md = std::get_if<toe::win::MouseDown>(&ev)) {
                const ChromeHit hit = view.hit_test_px(md->x, md->y);
                switch (hit.kind) {
                case ChromeHit::Kind::ActivateTab: rt.post(FocusTabAt{hit.tab_index}); return;
                case ChromeHit::Kind::CloseTab: rt.post(CloseTabAt{hit.tab_index}); return;
                case ChromeHit::Kind::NewTab: rt.post(NewTab{}); return;
                case ChromeHit::Kind::WinMinimize: rt.post(WinMinimize{}); return;
                case ChromeHit::Kind::WinMaximize: rt.post(WinToggleMax{}); return;
                case ChromeHit::Kind::WinClose: rt.post(WinCloseReq{}); return;
                case ChromeHit::Kind::None:
                    // Empty area of the chrome row acts as a titlebar: a press
                    // there starts an interactive window MOVE (the chrome row is
                    // the top `view.chrome_px_h()` pixels).
                    if (md->y >= 0 && md->y < view.chrome_px_h()) {
                        app.window_move(md->x, md->y);
                        return;
                    }
                    break; // click in the terminal body
                }
            }
            translate_event(rt, ev);
        });

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
