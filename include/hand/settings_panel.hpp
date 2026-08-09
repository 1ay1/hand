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

#include <deque>
#include <string>
#include <vector>

#include "hand/glyph/glyph.hpp"
#include "hand/config/handconfig.hpp"
#include "hand/platform/fonts.hpp"
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
    bool animate_cursor = true; // smooth caret glide

    // Seed the editable view from the loaded config.
    static Settings from(const HandConfig &c);
    // Fold the edits back into a HandConfig (for persistence).
    void into(HandConfig &c) const;
};

class SettingsPanel {
public:
    SettingsPanel() = default;

    [[nodiscard]] bool active() const noexcept { return active_; }
    void open(const Settings &current) { s_ = current; active_ = true; queue_.clear(); dirty_ = false; focus_ = 0; dd_open_ = -1; sync_font_index(); }
    void close() { active_ = false; queue_.clear(); dd_open_ = -1; }
    void toggle(const Settings &current) { active_ ? close() : open(current); }

    // Bind the panel to the loaded config + the file it persists to. The panel
    // seeds its editable view from `cfg` and, on Save, folds edits back and
    // writes the VIBE file at `path`.
    void bind(const HandConfig &cfg, std::string path) {
        cfg_ = cfg;
        save_path_ = std::move(path);
        fonts_ = list_monospace_families();
        s_ = Settings::from(cfg_);
        sync_font_index();
    }
    [[nodiscard]] const HandConfig &config() const noexcept { return cfg_; }
    void toggle() {
        if (active_) { close(); }
        else { s_ = Settings::from(cfg_); active_ = true; queue_.clear(); dirty_ = false; focus_ = 0; dd_open_ = -1; sync_font_index(); }
    }

    [[nodiscard]] const Settings &state() const noexcept { return s_; }

    // Feed one window event. Returns true if the panel consumed it (so it must
    // not reach the terminal). Escape closes the panel.
    [[nodiscard]] bool handle(const toe::win::Event &ev);

    // Request a save (e.g. from ⌘S). Persists on the next render().
    void request_save() { force_save_ = true; }

    // Paint the form into `buf` (sized to the terminal grid in cells), using the
    // input accumulated since the last handle(). Out-params report a live edit
    // (apply to the running terminal) and a save request (persist to config).
    void render(glyph::Buffer &buf, bool &changed, bool &save);

private:
    Settings s_{};
    HandConfig cfg_{};       // the full loaded config (edits folded back here)
    std::string save_path_;  // where Save persists the VIBE file
    bool active_ = false;
    // A QUEUE of pending inputs, not a single slot: fast typing / held keys
    // produce several events between frames and none may be dropped. render()
    // drains one per frame (the overlay repaints every frame, so it keeps up).
    std::deque<glyph::Input> queue_;
    bool want_save_ = false;
    bool dirty_ = false;     // edits since last save/open (unsaved-changes hint)
    bool saved_flash_ = false; // show a brief "saved" confirmation next frame
    bool force_save_ = false;  // ⌘S requested a save this/next frame
    int focus_ = 0;          // persistent focus row (the Ctx is recreated per frame)
    // Font dropdown state.
    std::vector<std::string> fonts_{};  // installed monospace families
    int font_index_ = 0;                // selected index into fonts_
    int dd_open_ = -1;                  // which row's dropdown is open (-1 none)
    int dd_sel_ = 0, dd_top_ = 0;       // dropdown highlight + scroll

    static glyph::Input translate(const toe::win::Event &ev, bool &consumed);

    // Point font_index_ at the family currently in s_.font_family (or 0).
    void sync_font_index() {
        font_index_ = 0;
        for (int i = 0; i < static_cast<int>(fonts_.size()); ++i)
            if (fonts_[static_cast<std::size_t>(i)] == s_.font_family) { font_index_ = i; break; }
    }
};

// Process-wide binding for the settings panel: main() sets the loaded config +
// its path here before the window opens (the surface, created deep inside
// toe::run, reads it to bind() its panel). Keeps the config flow out of the
// window-open signature.
void set_settings_source(const HandConfig &cfg, std::string path);
[[nodiscard]] const HandConfig &settings_source_config() noexcept;
[[nodiscard]] const std::string &settings_source_path() noexcept;

} // namespace hand

#endif // HAND_SETTINGS_PANEL_HPP
