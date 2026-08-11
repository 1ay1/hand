// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SettingsHost — the platform-neutral glue that makes the in-terminal settings
// panel work on any backend, factored out of the Cocoa backend so Wayland and
// X11 share exactly one copy of the logic.
//
// A backend embeds a SettingsHost, binds it once at open, calls toggle() from
// its own toggle keybind, and forwards the three OverlayApp<> hooks to it:
//
//     bool overlay_active() const           -> host_.active()
//     bool overlay_event(const Event&)      -> host_.handle(ev)
//     void overlay_render(Terminal&, px)    -> host_.render(term, px)
//
// toe::run_loop drives those automatically (see the OverlayApp concept): when
// the panel is active it captures input first and composites after the terminal
// renders, via Session::render_overlay.

#ifndef HAND_PLATFORM_SETTINGS_HOST_HPP
#define HAND_PLATFORM_SETTINGS_HOST_HPP

#include <string>
#include <vector>
#include <cstdlib>
#include <functional>

#include "hand/glyph/buffer.hpp"
#include "hand/platform/fonts.hpp"
#include "hand/settings_panel.hpp"
#include "hand/help_panel.hpp"
#include "hand/search_bar.hpp"
#include "hand/config/config.hpp" // load_hand_config for hot-reload
#include "hand/gui/host_config.hpp" // process-wide host-only knobs (flyout/autoscroll)
#include "hand/config_watch.hpp"  // inotify config watcher
#include "toe/app.hpp"      // toe::win::Event
#include "toe/terminal.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // MessageBeep for the audible bell
#else
#include <fcntl.h>   // open() for the audible bell (/dev/tty)
#include <unistd.h>  // write()/close()
#endif

namespace hand::platform {

class SettingsHost {
public:
    // Install the HOST's point-size -> device-pixel converter. A backend whose
    // launch-time DPI conversion differs from the default (macOS uses the
    // NSScreen backing scale factor, not GDK_SCALE) sets this BEFORE bind() so
    // live edits and the launch size agree. Optional; Linux uses the default.
    void set_font_scaler(std::function<int(int)> fn) { font_scaler_ = std::move(fn); }

    // Seed the panel from the process-wide config source (main() installs it via
    // set_settings_source before the window opens). Also arms the inotify watch
    // on the config file so external edits hot-reload.
    void bind() {
        panel_.bind(settings_source_config(), settings_source_path());
        watch_.start(settings_source_path());
        // Suppress the inotify echo of our own saves: when the pane writes the
        // file, mark it so on_config_fd_ready ignores the resulting event.
        panel_.set_on_saved([this] { watch_.note_self_write(); });
    }

    // The inotify fd for the config watch (-1 if none). The backend folds this
    // into its epoll wait via TerminalWait::watch_config.
    [[nodiscard]] int config_fd() const noexcept { return watch_.fd(); }
#if defined(_WIN32)
    // Windows has no fd: the backend folds this waitable EVENT into its
    // MsgWaitForMultipleObjectsEx instead. Null when no watch is live.
    [[nodiscard]] void *config_event() const noexcept {
        return watch_.active() ? watch_.event_handle() : nullptr;
    }
#endif

    // Install the BEL handler on the session from the current config. Audible:
    // write \a to the controlling terminal (/dev/tty) if hand was launched from
    // one — harmless no-op otherwise. Visual: flag a brief flash the renderer
    // shows. Re-called on reload so the toggles stay live.
    void install_bell(toe::Session &session) {
        const HandConfig &c = panel_.config();
        install_bell_from(session, c.behavior.audible_bell, c.behavior.visual_bell);
    }
    // Arm the on-bell handler with explicit modes (so a live panel edit can
    // re-install without going through the loaded config).
    void install_bell_from(toe::Session &session, bool audible, bool visual) {
        toe::Session *sp = &session;
        session.set_on_bell([audible, visual, sp] {
            if (audible) {
#if defined(_WIN32)
                // Windows has no controlling tty to forward \a to; the system
                // default sound IS the platform's bell.
                ::MessageBeep(MB_OK);
#else
                if (int fd = ::open("/dev/tty", O_WRONLY | O_NONBLOCK | O_CLOEXEC); fd >= 0) {
                    const char bel = '\a';
                    (void)::write(fd, &bel, 1);
                    ::close(fd);
                }
#endif
            }
            if (visual) sp->flash_visual_bell();
        });
    }

    // Any overlay pane open? (settings OR help). The run loop uses this to
    // capture input and repaint.
    [[nodiscard]] bool active() const noexcept { return panel_.active() || help_.active(); }

    // Push the loaded config's colours (theme palette + fg/bg/cursor/selection)
    // to the session ONCE at launch. to_toe() carries the font + fg/bg into
    // Terminal::create, but the 16-colour ANSI palette and cursor colour have
    // no toe::Config field — they're applied via the live setters. Without this
    // the theme's palette would only take effect after the first pane edit.
    void apply_startup(toe::Session &session, toe::PixelSize px) {
        apply(session, panel_.state(), px);
    }

    // Toggle the SETTINGS pane. Opening it closes help (one pane at a time).
    void toggle() {
        if (help_.active()) help_.close();
        panel_.toggle();
    }

    // Toggle the HELP pane. Opening it closes settings.
    void toggle_help() {
        if (panel_.active()) panel_.toggle();
        help_.toggle();
    }

    // Feed one window event; true if a pane consumed it (must not reach the
    // terminal). Only meaningful while active().
    [[nodiscard]] bool handle(const toe::win::Event &ev) {
        if (help_.active()) return help_.handle(ev);
        return panel_.handle(ev);
    }

    // Render the panel over the terminal this frame. Sizes the overlay buffer to
    // the grid, live-applies edits (font size/family, colours) to the running
    // session, and composites via the engine's overlay pass. Requires a current
    // GL context (the run loop calls it right after session.render()).
    void render(toe::Terminal &term, toe::PixelSize px) {
        auto *session = term.poll().running;
        if (!session) return;
        const toe::Extent cell = session->cell_size();
        if (cell.cols <= 0 || cell.rows <= 0) return;

        const int cols = std::max(1, px.w / cell.cols);
        const int rows = std::max(1, px.h / cell.rows);
        if (buf_.width() != cols || buf_.height() != rows) buf_.resize(cols, rows);

        // Help pane is read-only: paint the cheatsheet and composite, done.
        if (help_.active()) {
            help_.render(buf_);
            auto rc = toe::gfx::RenderContext::adopt_current();
            session->render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px, 0, 0, 1.0f,
                                    buf_.alpha_data());
            return;
        }

        bool changed = false;
        panel_.render(buf_, changed);

        // Live-apply edits so you SEE them change. Persistence is handled inside
        // the panel (debounced) — config is live end to end, no save step.
        if (changed) apply(*session, panel_.state(), px);

        auto rc = toe::gfx::RenderContext::adopt_current();
        session->render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px, 0, 0, 1.0f,
                                buf_.alpha_data());
    }

    // --- tabbed-mode API (global settings pane over N tabs) ------------------
    // Toggle just the settings PANEL (not help/search — the tabbed loop owns
    // those separately). Seeds from the bound config.
    void toggle_panel() { panel_.toggle(); }
    [[nodiscard]] bool panel_active() const noexcept { return panel_.active(); }
    // Feed one event to the settings panel while it's open. Returns consumed.
    bool panel_event(const toe::win::Event &ev) {
        if (!panel_.active()) return false;
        return panel_.handle(ev);
    }
    // Render the settings panel over `session` and, if an edit changed the
    // Settings this frame, set `changed` so the caller can fan the new state out
    // to EVERY tab (apply_to). GL context must be current.
    void render_panel(toe::Session &session, toe::PixelSize px, bool &changed) {
        changed = false;
        if (!panel_.active()) return;
        const toe::Extent cell = session.cell_size();
        if (cell.cols <= 0 || cell.rows <= 0) return;
        const int cols = std::max(1, px.w / cell.cols);
        const int rows = std::max(1, px.h / cell.rows);
        if (buf_.width() != cols || buf_.height() != rows) buf_.resize(cols, rows);
        panel_.render(buf_, changed);
        if (changed) apply(session, panel_.state(), px); // focused tab immediately
        auto rc = toe::gfx::RenderContext::adopt_current();
        session.render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px, 0, 0, 1.0f,
                               buf_.alpha_data());
    }
    // Apply the panel's CURRENT state to an arbitrary session (fan-out to the
    // non-focused tabs so a settings change hits ALL of them).
    void apply_to(toe::Session &session, toe::PixelSize px) { apply(session, panel_.state(), px); }

    // Render the scrollback search bar over the terminal. Reuses the same
    // overlay buffer + composite path as the panes, but draws only the one-line
    // bar (leaving the rest transparent so matches stay visible underneath).
    void render_search(toe::Terminal &term, toe::PixelSize px, const hand::SearchBar &bar) {
        auto *session = term.poll().running;
        if (!session) return;
        const toe::Extent cell = session->cell_size();
        if (cell.cols <= 0 || cell.rows <= 0) return;
        const int cols = std::max(1, px.w / cell.cols);
        const int rows = std::max(1, px.h / cell.rows);
        if (buf_.width() != cols || buf_.height() != rows) buf_.resize(cols, rows);
        // Everything transparent so the highlighted matches show through; only
        // the bar row (drawn by bar.render) is opaque.
        buf_.clear(glyph::Style{});
        buf_.clear_alpha(0);
        if (buf_.height() > 0) buf_.set_alpha({0, buf_.height() - 1, buf_.width(), 1}, 255);
        bar.render(buf_);
        auto rc = toe::gfx::RenderContext::adopt_current();
        session->render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px, 0, 0, 1.0f,
                                buf_.alpha_data());
    }

    // Called when the config-watch fd wakes: drain inotify, and if a real
    // (non-self) change landed, hot-reload. Runs on the loop thread from inside
    // the backend's wait_readable, so no locking is needed.
    void on_config_fd_ready(toe::Terminal &term, toe::PixelSize px) {
        if (!watch_.drained()) return; // spurious / self-write / unrelated file
        reload_from_disk(term, px);
    }

    // Hot-reload: the config FILE changed on disk (edited in $EDITOR, or synced).
    // Re-read it and live-apply through the SAME path a pane edit takes, then
    // refresh the panel's in-memory form so an open pane shows the new values.
    // This is the other half of "live config": file <-> running terminal, both
    // directions, one apply path. Skipped while the user is actively editing the
    // pane so a reload can't fight a live edit.
    void reload_from_disk(toe::Terminal &term, toe::PixelSize px) {
        auto *session = term.poll().running;
        if (!session) return;
        if (panel_.active()) return; // don't stomp an in-progress edit
        HandConfig fresh = load_hand_config(settings_source_path());
        panel_.reload(fresh);        // re-seed cfg_ + Settings from disk
        apply(*session, panel_.state(), px);
        install_bell(*session);      // bell toggles may have changed
    }

private:
    // The ONE live-apply path: push a Settings snapshot into the running session.
    // Both a pane edit and an external file reload funnel through here, so the
    // two config sources are guaranteed to behave identically.
    void apply(toe::Session &session, const hand::Settings &s, toe::PixelSize px) {
        session.set_font_pixel_size(scale_font_px(s.font_size), px);
        std::string file = s.font_file.empty() ? resolve_font_file(s.font_family) : s.font_file;
        if (!file.empty()) session.set_font(file, px);
        session.set_default_colors(parse_hex(s.fg), parse_hex(s.bg));
        session.set_selection_color(parse_hex(s.selection));
        session.set_selection_invert(s.selection_invert);
        session.set_cursor_color(parse_hex(s.cursor_color));
        // The 16 ANSI palette (from the active theme, or explicit overrides).
        // Empty => keep the built-in palette. This is what makes a theme switch
        // recolour existing on-screen text, not just future output.
        if (!s.palette.empty()) {
            std::vector<toe::Rgb> pal;
            pal.reserve(s.palette.size());
            for (const auto &h : s.palette) pal.push_back(parse_hex(h));
            session.set_palette(pal);
        }
        session.set_cursor_animation(s.animate_cursor, s.animate_ms, s.animate_trail);
        session.set_cursor_trail_len(s.animate_trail_len);
        session.set_cursor_blink_ms(s.blink_cursor ? s.blink_ms : 0);
        session.set_behavior({s.scroll_mult, s.scroll_on_output, s.scroll_on_keystroke,
                              s.copy_on_select});
        session.set_word_separators(s.word_separators);
        session.set_cursor_shape(s.cursor_style);
        session.set_ligatures(s.ligatures, px);
        session.set_padding(s.padding, px);
        session.set_opacity(s.opacity);
        // Search-match highlight + selection fine-tuning (live).
        session.set_search_colors(parse_hex(s.search_match), parse_hex(s.search_current));
        session.set_selection_tuning(static_cast<float>(s.selection_contrast) / 10.0f,
                                     static_cast<float>(s.selection_radius) / 100.0f, 0.11f);
        // Command-minimap rail (live).
        session.set_rail(s.rail, s.rail_width, parse_hex(s.rail_ok), parse_hex(s.rail_failed),
                         parse_hex(s.rail_running));
        session.set_rail_alpha(s.rail_alpha, 90);
        // Host-only GUI knobs the run loop reads (autoscroll/zoom/pointer/
        // flyout) — update the process-wide host_config so they take effect
        // immediately on the next interaction, no relaunch.
        auto &host = host_config();
        host.autoscroll_max = static_cast<float>(s.autoscroll_max);
        host.font_zoom_step = s.font_zoom_step;
        host.pointer_shapes = s.pointer_shapes;
        host.flyout = s.flyout;
        host.flyout_rows = s.flyout_rows;
        // Tab-bar toggles that can update live (position needs a relaunch to
        // re-carve the viewport, so it's not pushed here).
        host.tab_controls = s.tab_controls;
        host.tab_plus = s.tab_plus;
        host.tab_side_width = s.tab_side_width;
        {
            const toe::Rgb a = parse_hex(s.flyout_accent);
            host.flyout_accent = (static_cast<std::uint32_t>(a.r) << 16) |
                                 (static_cast<std::uint32_t>(a.g) << 8) |
                                 static_cast<std::uint32_t>(a.b);
        }
        // Bell mode is live too: re-arm the on-bell handler from the CURRENT
        // (edited) settings, not the loaded config, so toggling audible/visual
        // bell in the panel takes effect immediately.
        install_bell_from(session, s.audible_bell, s.visual_bell);
    }

    // Logical point size -> device pixels. The exact conversion is HOST policy
    // and differs by platform, so a backend can install its own via
    // set_font_scaler(); this MUST mirror the launch-time conversion in that
    // backend so the settings slider lands on the same size the window opened
    // with. Default (Linux/X11/Wayland): points -> px @96dpi (pt * 96/72) times
    // GDK_SCALE. macOS installs one using the NSScreen backing scale factor.
    int scale_font_px(int pts) const {
        if (font_scaler_) return font_scaler_(pts);
        double dpi_scale = 1.0;
        if (const char *g = std::getenv("GDK_SCALE"); g && *g) {
            double v = std::atof(g);
            if (v >= 1.0 && v <= 8.0) dpi_scale = v;
        }
        return static_cast<int>(static_cast<double>(pts) * (96.0 / 72.0) * dpi_scale + 0.5);
    }

    static toe::Rgb parse_hex(const std::string &h) {
        if (h.size() != 7 || h[0] != '#') return toe::rgb(200, 200, 200);
        auto d = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        auto v = [&](int i) { return static_cast<std::uint8_t>(d(h[i]) * 16 + d(h[i + 1])); };
        return toe::rgb(v(1), v(3), v(5));
    }

    hand::SettingsPanel panel_{};
    hand::HelpPanel help_{};
    hand::ConfigWatch watch_{}; // config-file watcher (inotify on Linux, kqueue on macOS/BSD)
    glyph::Buffer buf_{};
    // Host-installed point-size -> device-pixel converter (see scale_font_px).
    std::function<int(int)> font_scaler_{};
};

} // namespace hand::platform

#endif // HAND_PLATFORM_SETTINGS_HOST_HPP
