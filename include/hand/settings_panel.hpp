// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SettingsPanel — the live, in-terminal settings form, built on the glyph
// immediate-mode toolkit. It owns the editable config state, describes the form
// every frame, translates window events into glyph::Input, paints into a Buffer
// sized to the terminal, and reports edits so the host can live-apply + persist.
//
// It knows nothing about Cocoa/GL: the host (CocoaSurface) drives it with events
// and a cell size, then composites its Buffer via Session::render_overlay.

#ifndef HAND_SETTINGS_PANEL_HPP
#define HAND_SETTINGS_PANEL_HPP

#include <string>
#include <vector>

#include "hand/glyph/glyph.hpp"
#include "toe/app.hpp"      // toe::win::Event
#include "toe/terminal.hpp" // toe::Config

namespace hand {

// The subset of options the panel edits live. Mirrors toe::Config plus a couple
// of host-owned bits (cursor style). Applied back to the running terminal and
// written to the VIBE config on save.
struct Settings {
    int font_size = 13;      // logical points (host scales by DPI)
    bool ligatures = false;
    int cursor_style = 0;    // 0 block, 1 bar, 2 underline
    std::string font_family = "monospace";
    std::string fg = "#dcdcdc";
    std::string bg = "#171720";
    int scrollback = 10000;  // lines
    bool blink_cursor = true;
};

class SettingsPanel {
public:
    SettingsPanel() = default;

    [[nodiscard]] bool active() const noexcept { return active_; }
    void open(const Settings &current) { s_ = current; active_ = true; pending_ = glyph::Input{}; }
    void close() { active_ = false; }
    void toggle(const Settings &current) { active_ ? close() : open(current); }

    [[nodiscard]] const Settings &state() const noexcept { return s_; }

    // Feed one window event. Returns true if the panel consumed it (so it must
    // not reach the terminal). Escape closes the panel.
    [[nodiscard]] bool handle(const toe::win::Event &ev);

    // Paint the form into `buf` (sized to the terminal grid in cells), using the
    // input accumulated since the last handle(). Out-params report a live edit
    // (apply to the running terminal) and a save request (persist to config).
    void render(glyph::Buffer &buf, bool &changed, bool &save);

private:
    Settings s_{};
    bool active_ = false;
    glyph::Input pending_{}; // the event to process on the next render()
    bool want_save_ = false;

    static glyph::Input translate(const toe::win::Event &ev, bool &consumed);
};

} // namespace hand

#endif // HAND_SETTINGS_PANEL_HPP
