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
#include "hand/gui/frame_gate.hpp"
#include "hand/gui/message.hpp"
#include "hand/gui/runtime.hpp"
#include "hand/help_panel.hpp"
#include "hand/command_flyout.hpp"
#include "hand/search_bar.hpp"
#include "hand/platform/backend_base.hpp" // Chord + classify_chord (shared, one place)
#include "hand/platform/posix_pty.hpp" // spawn_pty (fresh PTY per tab)
#include "hand/platform/sokol_gl.hpp" // GL (glViewport) with prototypes
#include "hand/tabbed_view.hpp"
#include "toe/app.hpp"
#include "toe/core/event_router.hpp" // reuse toe's input policy (DRY)
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
// Return a copy of `ev` with any pointer Y coordinate shifted by `dy` (used to
// map window-space clicks into the terminal viewport, which sits below the
// chrome strip). Non-pointer events are returned unchanged.
inline toe::win::Event shift_pointer_y(const toe::win::Event &ev, int dy) {
    if (auto *e = std::get_if<toe::win::MouseDown>(&ev)) {
        auto c = *e; c.y += dy; return c;
    }
    if (auto *e = std::get_if<toe::win::MouseUp>(&ev)) {
        auto c = *e; c.y += dy; return c;
    }
    if (auto *e = std::get_if<toe::win::MouseMove>(&ev)) {
        auto c = *e; c.y += dy; return c;
    }
    return ev;
}

} // namespace detail

// Enter the tabbed loop. `app` is an opened backend; `cfg` is the terminal
// config (its .source is IGNORED here — each tab spawns its own PTY); `base_sc`
// carries argv/term/grid so every tab's child is spawned the same way.
template <class App>
[[nodiscard]] int run_tabbed(App &app, const toe::Config &cfg, const SpawnCommand &base_sc) {
    const toe::PixelSize px0 = app.pixel_size();

    TabbedView view;
    hand::HelpPanel help;   // Ctrl+Shift+?  (read-only cheatsheet)
    hand::SearchBar search; // Ctrl+Shift+F  (scrollback search, per focused tab)
    hand::CommandFlyout flyout; // rail-hover command list (click to jump)
    hand::platform::SettingsHost settings; // Ctrl+Shift+,  (GLOBAL pane, all tabs)
    settings.bind();        // config file + inotify watch, same as single-terminal
    glyph::Buffer overlay_buf; // cell buffer for the help/search overlays

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
    bool settings_changed_this_frame = false; // set by render_panel; fanned out below
    FrameGate gate;                          // frame-skip decision (render key)
    const bool loop_hz_present = std::getenv("HAND_LOOP_HZ") != nullptr;
    std::uint64_t present_count = 0;
    auto present = [&](TabActor &active) {
        const toe::PixelSize px = app.pixel_size();
        std::lock_guard lk(active.render_lock());
        auto *s = active.terminal().poll().running;
        if (!s) return;

        // The caret glide / bell fade changes the pixels every frame even when
        // generation() doesn't. Its live state is only known AFTER render()
        // updates it, so we publish it post-render (below). Use the animator's
        // current view for the key so an in-flight glide isn't frame-skipped.
        const std::uint32_t chrome_frame = static_cast<std::uint32_t>(rt_ptr->model().frame());

        RenderKey rk;
        rk.generation = s->generation();
        rk.scroll = s->scroll_offset();
        rk.w = px.w; rk.h = px.h;
        rk.overlay = help.active() ? 1u : search.active() ? 2u : settings.panel_active() ? 3u : 0u;
        // Fold the frame index UNCONDITIONALLY: the loop only advances it (via
        // set_frame) while something animates, so it's constant on a static
        // screen (key stable -> GPU skipped) but changes every 16ms during any
        // animation (glide/spinner/pulse) -> a fresh frame is guaranteed, no
        // chicken-and-egg with the caret-animating flag.
        rk.anim_frame = chrome_frame;
        // Fold the interaction revision: a selection drag / scrollback jump /
        // hover changes VISIBLE pixels without bumping generation(), so this is
        // what makes the gate repaint them instead of frame-skipping (the fix
        // for laggy text selection).
        rk.interaction = rt_ptr->interaction_revision();
        // Fold the flyout's visibility so it can't be frame-skipped on the
        // frame it first appears (its content rides the interaction revision).
        if (flyout.active()) rk.overlay = rk.overlay ? rk.overlay : 4u;
        if (!gate.should_present(rk)) return;          // pixel-identical: no GPU work
        if (loop_hz_present) ++present_count;

        app.begin_frame(px, 23, 23, 28, 1.0f); // clear; toe overdraws its own bg
        {
            {
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
                // Publish the caret-glide state NOW — render() just updated it.
                // This is what keeps the loop waking at 60fps until the glide
                // settles (the fix for the choppy cursor).
                rt_ptr->set_focus_animating(s->cursor_animating());
                // Chrome overlay across the FULL window (its own top row).
                glViewport(0, 0, px.w, px.h);
                view.render_chrome(rc, *s, rt_ptr->model(), px, chrome_frame);
                // Help / search overlays (full-window cell buffer over the grid).
                if (help.active() || search.active()) {
                    const int cols = std::max(1, px.w / std::max(1, cell.cols));
                    const int rows = std::max(1, px.h / std::max(1, cell.rows));
                    if (overlay_buf.width() != cols || overlay_buf.height() != rows)
                        overlay_buf.resize(cols, rows);
                    if (help.active()) {
                        overlay_buf.clear(glyph::Style{});
                        overlay_buf.clear_alpha(220);
                        help.render(overlay_buf);
                        s->render_overlay(rc, overlay_buf.data(), cols, rows, px, 0, 0, 0.86f,
                                          overlay_buf.alpha_data());
                    } else { // search: one-line bar, rest transparent
                        overlay_buf.clear(glyph::Style{});
                        overlay_buf.clear_alpha(0);
                        if (rows > 0) overlay_buf.set_alpha({0, rows - 1, cols, 1}, 255);
                        search.render(overlay_buf);
                        s->render_overlay(rc, overlay_buf.data(), cols, rows, px, 0, 0, 1.0f,
                                          overlay_buf.alpha_data());
                    }
                } else if (flyout.active()) {
                    // Command-list flyout: a floating card beside the rail. No
                    // scrim — only the card composites (its render() sets alpha).
                    const toe::Extent cell = s->cell_size();
                    const int cols = std::max(1, px.w / std::max(1, cell.cols));
                    const int rows = std::max(1, px.h / std::max(1, cell.rows));
                    if (overlay_buf.width() != cols || overlay_buf.height() != rows)
                        overlay_buf.resize(cols, rows);
                    overlay_buf.clear(glyph::Style{});
                    flyout.render(overlay_buf);
                    s->render_overlay(rc, overlay_buf.data(), cols, rows, px, 0, 0, 1.0f,
                                      overlay_buf.alpha_data());
                }
                // Settings panel: renders + applies to the focused tab; a change
                // this frame is flagged so the loop fans it out to ALL tabs
                // (below, after this tab's lock is released).
                if (settings.panel_active()) {
                    settings.render_panel(*s, px, settings_changed_this_frame);
                }
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

    const std::uint64_t start = detail::now_ms_();
    // Elapsed-ms baseline: the loop compares against `now = now_ms_() - start`
    // (elapsed), so this MUST be 0, not the absolute `start` — otherwise
    // `now - last_anim_ms` is permanently a huge negative number and the
    // animation branch never fires (the caret glide limps, its trail freezes).
    std::int64_t last_anim_ms = 0; // last elapsed-ms an animation frame was posted
    // --- drag-selection autoscroll --------------------------------------
    // While the left button is held and the pointer leaves the grid vertically,
    // the view auto-scrolls so a selection can span more than one screen of
    // scrollback (like every polished terminal / editor). This is driven as an
    // Animator source (Anim::Autoscroll) so the loop keeps waking at ~60fps for
    // as long as the pointer stays past the edge, then stops — zero idle cost.
    bool drag_sel = false;   // left button held after a body mouse-down
    int drag_col = 0;        // last in-grid column the pointer was over
    int drag_dir = 0;        // -1 scroll up (pointer above), +1 down, 0 none
    float drag_vel = 0.0f;   // autoscroll speed in ROWS PER SECOND (edge distance)
    float drag_accum = 0.0f; // fractional-row accumulator (integrates vel*dt)
    std::int64_t drag_last_ms = 0; // elapsed-ms of the last autoscroll step
    bool running = true;
    // Diagnostic: HAND_LOOP_HZ=1 prints GUI-loop iterations/sec to stderr.
    const bool loop_hz = std::getenv("HAND_LOOP_HZ") != nullptr;
    std::uint64_t loop_iters = 0, loop_win = start, anim_wakes = 0;
    while (!app.should_close() && !rt.done() && running) {
        if (loop_hz) {
            ++loop_iters;
            const std::uint64_t now = detail::now_ms_();
            if (now - loop_win >= 1000) {
                std::fprintf(stderr, "[loop] GUI %llu iters/sec, %llu presents/sec, %llu anim-wakes/sec\n",
                             (unsigned long long)loop_iters, (unsigned long long)present_count,
                             (unsigned long long)anim_wakes);
                loop_iters = 0; loop_win = now; present_count = 0; anim_wakes = 0;
            }
        }
        app.poll_events([&](const toe::win::Event &ev) {
            // --- 1. overlay (help/search/settings) owns the keyboard ----------
            if (const auto *kp = std::get_if<toe::win::KeyPressed>(&ev)) {
                const auto &k = kp->key;
                const bool press = k.kind == toe::KeyEvent::Kind::press;
                const platform::Chord ch = platform::classify_chord(k);
                if (press && ch == platform::Chord::ToggleSettings) {
                    help.close();
                    if (search.active())
                        rt.with_focus_session([&](toe::Session &s) { search.close(s); });
                    settings.toggle_panel();
                    return;
                }
                if (press && ch == platform::Chord::ToggleHelp) {
                    if (search.active())
                        rt.with_focus_session([&](toe::Session &s) { search.close(s); });
                    help.toggle();
                    return;
                }
                if (press && ch == platform::Chord::OpenSearch) {
                    rt.with_focus_session([&](toe::Session &s) {
                        if (search.active()) search.close(s);
                        else { help.close(); search.open(s); }
                    });
                    return;
                }
                if (help.active()) { help.handle(ev); return; }
                if (settings.panel_active()) { settings.panel_event(ev); return; }
                if (search.active()) {
                    rt.with_focus_session([&](toe::Session &s) { search.handle(ev, s); });
                    return;
                }
                // --- 2. tab-management chords -------------------------------
                if (press) {
                    switch (ch) {
                    case platform::Chord::NewTab: rt.post(NewTab{}); return;
                    case platform::Chord::CloseTab: rt.post(CloseTab{}); return;
                    case platform::Chord::NextTab: rt.post(NextTab{}); return;
                    case platform::Chord::PrevTab: rt.post(PrevTab{}); return;
                    default: break;
                    }
                }
            } else if (help.active() || search.active() || settings.panel_active()) {
                if (std::get_if<toe::win::TextEntered>(&ev)) {
                    if (settings.panel_active()) { settings.panel_event(ev); return; }
                    if (search.active())
                        rt.with_focus_session([&](toe::Session &s) { search.handle(ev, s); });
                    return;
                }
            }

            // --- 3. chrome (tab bar + window controls) ------------------------
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
                    if (md->y >= 0 && md->y < view.chrome_px_h()) {
                        app.window_move(md->x, md->y);
                        return;
                    }
                    break; // click in the terminal body -> falls through
                }
            }

            // --- 4. everything else = terminal input for the FOCUSED tab ------
            // Reuse toe's EventRouter (the SAME input policy as single-terminal
            // mode) so scroll, wheel, selection, copy/paste, OSC-8 links,
            // command-block nav and focus all Just Work — no reimplementation.
            // Pointer Y is shifted down by the chrome strip; correct it so the
            // grid's top cell maps right.
            const int ch_h = view.chrome_px_h();
            toe::win::Event adj = detail::shift_pointer_y(ev, -ch_h);
            rt.with_focus_session([&](toe::Session &s) {
                toe::PixelSize tpx = app.pixel_size();
                tpx.h = std::max(1, tpx.h - ch_h);
                toe::EventRouter<App> router{s, app, tpx, running};
                std::visit(router, adj);
                // The router just updated the rail-hover row (mouse-move) and
                // may have jumped on a rail click. Refresh the command-list
                // flyout from that state: it shows/hides itself based on whether
                // the pointer is on the rail. Click-to-jump is already handled
                // by the router's rail_click (snaps to the block under the
                // pointer) — exactly the flyout row the user sees highlighted.
                const bool was = flyout.active();
                flyout.update(s);
                // Repaint on any flyout change: appear/disappear OR the hovered
                // row moving. A bare hover (MouseMove w/o button) isn't in the
                // pixel_affecting set below, so request here while it's live.
                if (flyout.active() || was) rt.request_present();
            });
            // A pointer/scroll event can change the VISIBLE pixels (selection
            // highlight, scrollback view, hover) WITHOUT bumping the terminal's
            // content generation() — which the RenderKey keys on. Request ONE
            // present so the change isn't frame-skipped and lagged. Gate it to
            // the events that actually move those pixels: a DRAG (mouse move
            // with a button held = selection extend), a click (anchor/clear),
            // and the wheel (scrollback). A plain hover / bare move must NOT
            // request — that would repaint every idle mouse twitch. (Keyboard
            // input already presents via the actor's TabOutput → generation
            // bump; animation goes through the Animator. This is the one-shot
            // INTERACTION repaint path — the other half of the unified trigger.)
            const bool pixel_affecting =
                std::holds_alternative<toe::win::MouseWheel>(adj) ||
                std::holds_alternative<toe::win::MouseDown>(adj) ||
                std::holds_alternative<toe::win::MouseUp>(adj) ||
                (std::holds_alternative<toe::win::MouseMove>(adj) &&
                 std::get<toe::win::MouseMove>(adj).button_down);
            if (pixel_affecting) rt.request_present();

            // --- drag-selection autoscroll arming ---------------------------
            // Decide whether the view should auto-scroll: the pointer is being
            // dragged (left button held) PAST the top or bottom of the grid.
            // We use the CHROME-ADJUSTED y (`adj`), so 0 == the grid's top edge
            // and grid_px_h == its bottom. Distance past an edge sets both the
            // direction and a distance-scaled speed (faster the farther out you
            // drag) for a natural feel. The actual scroll+extend happens once
            // per frame in the loop while Anim::Autoscroll is live.
            if (const auto *md = std::get_if<toe::win::MouseDown>(&adj)) {
                if (md->button == toe::win::MouseButton::left) drag_sel = true;
            } else if (std::holds_alternative<toe::win::MouseUp>(adj)) {
                drag_sel = false; drag_dir = 0;
                rt.animator().set(Anim::Autoscroll, false);
            } else if (const auto *mm = std::get_if<toe::win::MouseMove>(&adj);
                       mm && drag_sel && mm->button_down) {
                const int grid_px_h = std::max(1, app.pixel_size().h - ch_h);
                rt.with_focus_session([&](toe::Session &s) {
                    const int cw = std::max(1, s.cell_width());
                    const int chh = std::max(1, s.cell_height());
                    drag_col = std::max(0, mm->x / cw);
                    const int prev_dir = drag_dir;
                    // Distance past the edge in CELLS maps to a velocity in ROWS
                    // PER SECOND — a gentle ramp so it starts as a slow crawl
                    // (~3 rows/s just past the edge) and accelerates smoothly to
                    // a brisk-but-readable ceiling (~45 rows/s) when dragged far.
                    const auto vel_for = [chh](int past_px) {
                        const float cells = static_cast<float>(past_px) /
                                            static_cast<float>(chh);
                        return std::min(3.0f + cells * 12.0f, 45.0f);
                    };
                    if (mm->y < 0) {
                        drag_dir = -1;                   // above the grid -> up
                        drag_vel = vel_for(-mm->y);
                    } else if (mm->y >= grid_px_h) {
                        drag_dir = +1;                   // below the grid -> down
                        drag_vel = vel_for(mm->y - grid_px_h);
                    } else {
                        drag_dir = 0;                    // inside: no autoscroll
                    }
                    if (drag_dir != 0 && prev_dir == 0) {
                        drag_accum = 0.0f;               // fresh engagement
                        drag_last_ms = static_cast<std::int64_t>(detail::now_ms_() - start);
                    }
                });
                rt.animator().set(Anim::Autoscroll, drag_dir != 0);
            }
        });

        // --- process actor messages (output/status/exit) --------------------
        // pump() drains the GUI mailbox, runs gui_update, interprets Cmds, and
        // presents at most once if a Present cmd fired. So a repaint happens
        // ONLY in response to real input or new child output — never on a timer.
        rt.pump();

        // --- interaction-driven present (selection / scroll / hover) --------
        // The input path (section 4) called request_present() after routing a
        // pointer/scroll event that may have changed VISIBLE pixels without
        // bumping content generation(). Present once here so it isn't frame-
        // skipped and lagged. FrameGate still de-dups via the interaction
        // revision folded into the RenderKey, so redundant requests are free.
        if (rt.take_present_request()) rt.present_focused();

        // --- drag-selection autoscroll step ---------------------------------
        // While the pointer is held past a vertical edge, scroll the view and
        // extend the selection to the edge row so the highlight keeps growing
        // into scrollback. Runs once per loop wake; Anim::Autoscroll keeps the
        // loop waking at ~60fps (see the animation clock below) and is cleared
        // on MouseUp or when the pointer re-enters the grid.
        if (drag_dir != 0) {
            const std::int64_t now_e = static_cast<std::int64_t>(detail::now_ms_() - start);
            const float dt = std::clamp(static_cast<float>(now_e - drag_last_ms), 0.0f, 100.0f) /
                             1000.0f; // seconds since last step (clamped vs. stalls)
            drag_last_ms = now_e;
            drag_accum += drag_vel * dt;          // rows/sec * sec = rows (fractional)
            const int step = static_cast<int>(drag_accum);
            if (step > 0) {
                drag_accum -= static_cast<float>(step);
                rt.with_focus_session([&](toe::Session &s) {
                    s.scroll(-drag_dir * step); // scroll(+n)=older; dir<0=up=older
                    const int rows = s.grid_size().rows;
                    const int edge_row = drag_dir < 0 ? 0 : std::max(0, rows - 1);
                    const int col = std::min(drag_col, std::max(0, s.grid_size().cols - 1));
                    s.select_extend(edge_row, col);
                });
                rt.request_present();
            }
        }

        if (settings_changed_this_frame) {
            settings_changed_this_frame = false;
            const toe::PixelSize px = app.pixel_size();
            rt.for_each_live_session([&](toe::Session &s) { settings.apply_to(s, px); });
        }

        // --- animation clock: advance only while something animates ---------
        // A spinner / done-attention pulse / caret glide runs at ~60fps; a
        // steady cursor blink at its half-period; nothing animating -> block
        // FOREVER. The frame index is derived from ELAPSED TIME (not a per-loop
        // counter), and we present DIRECTLY — no Tick round-trip through the
        // mailbox (that indirection was the earlier busy-loop hazard).
        const int dl_ms = rt.animation_deadline_ms();
        if (dl_ms > 0) {
            const std::int64_t now = static_cast<std::int64_t>(detail::now_ms_() - start);
            if (now - last_anim_ms >= dl_ms) {
                last_anim_ms = now;
                if (loop_hz_present && dl_ms == 16) ++anim_wakes;
                rt.set_frame(FrameGate::frame_index(now)); // time-derived spinner phase
                rt.present_focused();                       // direct present; FrameGate skips if static
            }
        }

        // --- ONE blocking wait: window fd + GUI mailbox fd ------------------
        // Zero idle CPU: with nothing animating and no input, this blocks until
        // the compositor sends an event OR an actor posts to the mailbox (its
        // wakeup fd is the "pty" fd; wait_readable folds in the window fd).
        // Deadline: the animation cadence, else forever.
        const toe::WaitDeadline wd =
            dl_ms < 0 ? toe::WaitDeadline::forever() : toe::WaitDeadline::millis(dl_ms);
        app.wait_readable(rt.mailbox().wait_fd(), wd);
    }
    return 0;
}

} // namespace hand

#endif // HAND_GUI_RUN_TABBED_HPP
