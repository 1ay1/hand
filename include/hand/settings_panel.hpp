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
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hand/glyph/glyph.hpp"
#include "hand/config/handconfig.hpp"
#include "hand/platform/fonts.hpp"
#include "toe/app.hpp"      // toe::win::Event
#include "toe/terminal.hpp" // toe::Config

namespace hand {

// Declared in hand/config/config.hpp; forward-declared here so flush_pending()
// can live-persist without pulling the whole VIBE parser into every consumer.
[[nodiscard]] bool save_hand_config(const HandConfig &cfg, std::string_view path);

// The subset of options the panel edits live. Mirrors toe::Config plus a couple
// of host-owned bits (cursor style). Applied back to the running terminal and
// written to the VIBE config on save. This is the editable mirror of the WHOLE
// HandConfig — every option is reachable from the settings pane.
struct Settings {
    // Theme (built-in id, e.g. "catppuccin-mocha"). The BASE colour layer; the
    // hex colours below override it. Picking a theme in the pane recolours the
    // grid AND the pane chrome live.
    std::string theme = "catppuccin-mocha";

    // Font
    std::string font_family = "monospace";
    std::string font_file{};
    std::string font_fallback{};
    std::string font_bold{};        // real bold face file (empty = synthesise)
    std::string font_italic{};      // real italic face file
    std::string font_bold_italic{}; // real bold-italic face file
    int font_size = 13;      // logical points (host scales by DPI)
    bool ligatures = false;

    // Cursor
    int cursor_style = 0;    // 0 block, 1 bar, 2 underline
    bool blink_cursor = true;
    int blink_ms = 530;
    bool animate_cursor = true;
    int animate_ms = 55;
    bool animate_trail = true;
    int animate_trail_len = 3;

    // Colors (hex "#rrggbb")
    std::string fg = "#dcdcdc";
    std::string bg = "#171720";
    std::string cursor_color = "#dcdcdc";
    std::string selection = "#3c466e";
    bool selection_invert = true; // reverse-video selection (swap fg/bg)
    std::string search_match = "#786028";   // all search matches
    std::string search_current = "#ffb020";  // the match you're on
    int selection_contrast = 30;   // WCAG floor x10 (30 = 3.0:1)
    int selection_radius = 28;     // corner rounding, % of cell
    // The 16 ANSI palette (hex), from the active theme or explicit overrides.
    // Empty => toe's built-in palette. Drives live recolour on theme switch.
    std::vector<std::string> palette{};

    // Chrome (command minimap rail + hover flyout)
    bool rail = true;
    int rail_width = 7;
    std::string rail_ok = "#50c882";
    std::string rail_failed = "#eb5a5a";
    std::string rail_running = "#f0be46";
    int rail_alpha = 210;
    bool flyout = true;
    int flyout_rows = 12;
    std::string flyout_accent = "#7aa8ff";

    // Scroll
    int scrollback = 10000;  // lines
    int scroll_mult = 3;     // lines per wheel notch
    bool scroll_on_output = false;
    bool scroll_on_keystroke = true;
    int autoscroll_max = 45;   // drag-select autoscroll ceiling (rows/sec)
    int font_zoom_step = 2;     // px per Ctrl+= / Ctrl+- notch
    bool pointer_shapes = true; // context-aware mouse cursor

    // Behavior
    bool audible_bell = false;
    bool visual_bell = true;
    bool copy_on_select = false;
    bool confirm_close = false;
    std::string word_separators{}; // extra word-joining chars for double-click

    // Window
    std::string title = "hand";
    int padding = 0;
    float opacity = 1.0f;                  // window background (compositor)
    float overlay_panel_opacity = 0.95f;  // settings/help pane card
    float overlay_scrim_opacity = 0.25f;  // dim around the pane
    bool decorations = true;

    // Advanced (host-side; a restart may be needed for shell/TERM)
    std::string shell{};     // program to run (empty = $SHELL)
    std::string term_env = "xterm-256color"; // $TERM advertised to the child

    // Seed the editable view from the loaded config.
    static Settings from(const HandConfig &c);
    // Fold the edits back into a HandConfig (for persistence).
    void into(HandConfig &c) const;
};

class SettingsPanel {
public:
    SettingsPanel() = default;

    [[nodiscard]] bool active() const noexcept { return active_; }
    void open(const Settings &current) { s_ = current; active_ = true; queue_.clear(); focus_ = 0; section_ = 0; dd_open_ = -1; sync_font_index(); }
    void close() { flush_pending(); active_ = false; queue_.clear(); dd_open_ = -1; }
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
    // Invoked right after the panel writes the config file, so the host can mark
    // the inotify event that follows as a self-write (and not hot-reload it).
    void set_on_saved(std::function<void()> cb) { on_saved_ = std::move(cb); }

    [[nodiscard]] const HandConfig &config() const noexcept { return cfg_; }

    // Re-seed from a freshly-loaded config (external file edit). Refreshes the
    // editable form so an open pane reflects on-disk values. The caller only
    // calls this when the pane is inactive, so it can't fight a live edit.
    void reload(const HandConfig &fresh) {
        cfg_ = fresh;
        s_ = Settings::from(cfg_);
        pending_save_ = false; // now in sync with disk
        sync_font_index();
    }
    void toggle() {
        if (active_) { close(); }
        else { s_ = Settings::from(cfg_); active_ = true; queue_.clear(); focus_ = 0; section_ = 0; dd_open_ = -1; sync_font_index(); }
    }

    [[nodiscard]] const Settings &state() const noexcept { return s_; }

    // Feed one window event. Returns true if the panel consumed it (so it must
    // not reach the terminal). Escape closes the panel.
    [[nodiscard]] bool handle(const toe::win::Event &ev);

    // Paint the form into `buf` (sized to the terminal grid in cells), using the
    // input accumulated since the last handle(). `changed` reports a live edit
    // this frame — the host applies it to the running terminal immediately. The
    // panel PERSISTS every change itself (debounced), so there is no save step.
    void render(glyph::Buffer &buf, bool &changed);

private:
    Settings s_{};
    HandConfig cfg_{};       // the full loaded config (edits folded back here)
    std::string save_path_;  // VIBE file every edit live-persists to
    bool active_ = false;
    // A QUEUE of pending inputs, not a single slot: fast typing / held keys
    // produce several events between frames and none may be dropped. render()
    // drains one per frame (the overlay repaints every frame, so it keeps up).
    std::deque<glyph::Input> queue_;
    // Live persistence: an edit sets pending_save_ + stamps edited_ms_; the
    // panel flushes to disk once the edits settle (kSaveDebounceMs) so a slider
    // drag writes the file once, not on every tick. flush_pending() also runs on
    // close so nothing is lost.
    bool pending_save_ = false;
    std::uint64_t edited_ms_ = 0;
    std::function<void()> on_saved_{}; // host hook: mark our own writes
    static constexpr std::uint64_t kSaveDebounceMs = 400;
    int focus_ = 0;          // persistent focus row (the Ctx is recreated per frame)
    int section_ = 0;        // active settings tab (Appearance/Font/Colors/…)
    // Font dropdown state.
    std::vector<std::string> fonts_{};  // installed monospace families
    int font_index_ = 0;                // selected index into fonts_
    int dd_open_ = -1;                  // which row's dropdown is open (-1 none)
    int dd_sel_ = 0, dd_top_ = 0;       // dropdown highlight + scroll

    // Theme picker state. Labels drive the dropdown; ids are what we persist.
    std::vector<std::string> theme_labels_{};
    std::vector<std::string> theme_ids_{};
    int theme_index_ = 0;              // selected index into theme_ids_
    int theme_dd_sel_ = 0, theme_dd_top_ = 0;
    // Live incremental filter text for the theme + font dropdowns (persists
    // across frames since the Ctx is recreated each frame).
    std::string theme_filter_{};
    std::string font_filter_{};
    // "Save as theme" export: the name the user is typing + a transient status
    // line ("Saved → ...") shown after a successful write.
    std::string export_name_{"My Theme"};
    std::string export_status_{};

    static glyph::Input translate(const toe::win::Event &ev, bool &consumed);

    // Fold pending edits into cfg_ and write the VIBE file now (called by the
    // debounce in render() and unconditionally on close, so nothing is lost).
    // Notifies on_saved_ AFTER a successful write so the host can suppress the
    // inotify echo of our own save.
    void flush_pending() {
        if (!pending_save_) return;
        pending_save_ = false;
        s_.into(cfg_);
        if (!save_path_.empty() && save_hand_config(cfg_, save_path_)) {
            if (on_saved_) on_saved_();
        }
    }
    [[nodiscard]] static std::uint64_t now_ms() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Point font_index_ at the family currently in s_.font_family (or 0).
    void sync_font_index() {
        font_index_ = 0;
        for (int i = 0; i < static_cast<int>(fonts_.size()); ++i)
            if (fonts_[static_cast<std::size_t>(i)] == s_.font_family) { font_index_ = i; break; }
    }

    // Lazily fill theme_labels_/theme_ids_ from the built-in table and point
    // theme_index_ at the active theme id.
    void ensure_themes();
    // Write the current colours (from s_) to a user theme file named by
    // export_name_. Returns the path, or empty on failure. Reloads the theme
    // registry so it shows up in the picker at once.
    std::string export_current_theme();
    void sync_theme_index() {
        theme_index_ = 0;
        for (int i = 0; i < static_cast<int>(theme_ids_.size()); ++i)
            if (theme_ids_[static_cast<std::size_t>(i)] == s_.theme) { theme_index_ = i; break; }
    }

public:
    // The UI-chrome theme derived from the active terminal theme. The host reads
    // this to tint the whole overlay (panels/borders/accents) so the settings
    // pane recolours with the terminal. Falls back to a default if unresolved.
    [[nodiscard]] glyph::Theme ui_theme() const;
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
