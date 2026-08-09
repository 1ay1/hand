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
#include <cstdlib>

#include "hand/glyph/buffer.hpp"
#include "hand/platform/fonts.hpp"
#include "hand/settings_panel.hpp"
#include "hand/help_panel.hpp"
#include "toe/app.hpp"      // toe::win::Event
#include "toe/terminal.hpp"

namespace hand::platform {

class SettingsHost {
public:
    // Seed the panel from the process-wide config source (main() installs it via
    // set_settings_source before the window opens).
    void bind() { panel_.bind(settings_source_config(), settings_source_path()); }

    // Any overlay pane open? (settings OR help). The run loop uses this to
    // capture input and repaint.
    [[nodiscard]] bool active() const noexcept { return panel_.active() || help_.active(); }

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
            session->render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px);
            return;
        }

        bool changed = false;
        panel_.render(buf_, changed);

        // Live-apply edits so you SEE them change. The panel's font size is a
        // LOGICAL POINT size; convert to pixels the same way backend.cpp does at
        // startup (pt * 96/72 * HiDPI scale) so the slider matches the launch
        // size exactly. Persistence is handled inside the panel (debounced), so
        // there is nothing to save here — config is live end to end.
        if (changed) {
            const hand::Settings &s = panel_.state();
            session->set_font_pixel_size(scale_font_px(s.font_size), px);
            std::string file = s.font_file.empty() ? resolve_font_file(s.font_family) : s.font_file;
            if (!file.empty()) session->set_font(file, px);
            session->set_default_colors(parse_hex(s.fg), parse_hex(s.bg));
            session->set_selection_color(parse_hex(s.selection));
            session->set_cursor_animation(s.animate_cursor, s.animate_ms, s.animate_trail);
            session->set_cursor_blink_ms(s.blink_cursor ? s.blink_ms : 0);
        }

        auto rc = toe::gfx::RenderContext::adopt_current();
        session->render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px);
    }

private:
    // Logical point size -> pixels @96dpi, times the HiDPI scale (GDK_SCALE).
    // Mirrors backend.cpp so the settings slider matches the launch size.
    static int scale_font_px(int pts) {
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
    glyph::Buffer buf_{};
};

} // namespace hand::platform

#endif // HAND_PLATFORM_SETTINGS_HOST_HPP
