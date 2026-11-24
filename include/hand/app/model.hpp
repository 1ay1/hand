#pragma once
// hand::app model — everything hand knows, as plain typed data.
//
//   Session   one tab: its pty, its screen, and what animates over it  (model/session.hpp)
//   Tabs      the sessions in strip order, and which is in front     (model/tabs.hpp)
//   Model     the window: config, the tabs, and what sits over and beside
//             them -- the strip, the dialog, the settings, ssh, the file pane
//
// A mode that may be absent is an optional, never a bool next to its data:
// `copy` is engaged exactly while copy mode is on; `ask` exactly while a
// dialog is asking. What a value means is in its type.

#include <hand/app/model/chrome.hpp>
#include <hand/app/model/modes.hpp>
#include <hand/app/model/pane.hpp>
#include <hand/app/model/ssh.hpp>
#include <hand/app/model/tabs.hpp>
#include <hand/config/config.hpp>
#include <hand/core/pty.hpp>
#include <hand/core/term.hpp>
#include <hand/core/types.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hand {

/// The world a run starts in, read ONCE by main (src/cli/env.cpp) and handed
/// in as Flags: update and view never ask the OS for any of it.
struct Env {
    std::string   home;            ///< ~ for paths (%USERPROFILE% on Windows: ssh's home)
    std::string   editor = "vi";   ///< $VISUAL, else $EDITOR
    std::string   shell;           ///< the shell an empty shell setting means ($SHELL; Windows: pwsh, ...)
    std::string   host_name;       ///< this machine (is an OSC 7 host here?)
    std::string   cwd;             ///< where hand started (a shell that reports no directory is here)
    std::string   config_path;     ///< hand.vibe (hosts.vibe sits beside it)
    std::int64_t  wall_epoch_ms = 0;   ///< the wall clock when the run began (unix ms)
    std::uint64_t entropy = 0;     ///< seeds what must be unguessable (DEC 2034 tokens)
    /// The keyboard conventions shortcuts are SHOWN in (⌘P vs Ctrl+P). Keys
    /// themselves carry theirs (Mods.keymap); a test sets this to draw the
    /// other platform's hints on any machine.
    mayag::Keymap keymap = mayag::native_keymap();
};

/// The window: config, its tabs, and what sits over and beside them.
struct Model {
    Config                  cfg;
    Env                     env;                  ///< from Flags
    std::shared_ptr<const mayag::typo::FontStack> fonts;   ///< from Flags: what cells are measured with
    // time, stamped by App's root update from the step time jaal passes in
    // (never read from a clock): every message one step folds agrees on it
    mayag::SteadyClock::time_point now{};          ///< this step
    mayag::SteadyClock::time_point began{};        ///< the first step (wall_ms counts from here)
    std::int64_t            wall_ms = 0;          ///< this step on the wall clock (unix ms): command timings, "3h ago"
    bool                    probe_due = false;    ///< a Probe is scheduled (output came: what runs may have changed)
    Palette                 palette;
    mayag::Vec2             win{800, 540};        ///< the window
    mayag::Vec2             size{800, 500};       ///< the terminal area (the window below the tab strip)
    float                   font_size = 13.0f;    ///< cfg.font_size, or zoomed
    float                   ui_font_size = 12.5f; ///< cfg.ui_font_size, or zoomed (ctrl+shift +/-)
    bool                    focused = true;
    std::string             preedit;              ///< the input method's composition, drawn at the cursor (not yet typed)
    bool                    cursor_on = true;
    /// The cursor stops blinking after a while with no keys and no output
    /// (GTK's cursor-blink-timeout): an idle window then draws no frames at
    /// all. `blink_seen` is what the front tab looked like at the last blink
    /// (content epoch, cursor, keys typed); `blinks_idle` how many blinks it
    /// has stayed so.
    std::uint64_t           blink_seen = 0;
    int                     blinks_idle = 0;
    std::uint64_t           keys_typed = 0;
    double                  time = 0.0;
    mayag::dsl::TermMetrics metrics{};            ///< cell size, from the font (fit())
    Tabs                    tabs;                 ///< every tab, in strip order; never empty
    SessionIds              session_ids;          ///< hands out each new session's id
    StripModel              strip;                ///< the tab strip
    bool                    ctrl_held = false;    ///< ctrl is down: links are armed
    MinimapModel            minimap;
    std::uint64_t           clock = 0;            ///< 1 Hz: live timers, activity samples
    /// The size HUD, up after a resize changed the grid. It is still, so it
    /// takes no frames: HudExpired{hud_serial} takes it down 0.8 s later (a
    /// newer resize bumps the serial and keeps it up).
    bool                    hud_up = false;
    std::uint64_t           hud_serial = 0;
    bool                    sized_once = false;
    ToastModel              toast;
    // what floats over the terminal, and what sits beside it
    SettingsModel           settings;             ///< ctrl+,
    DialogModel             dialog;               ///< the one open question
    SshModel                ssh;                  ///< ctrl+shift+s picker, ctrl+shift+h manager
    PaneModel               pane;                 ///< ctrl+shift+e
    QuickOpenModel          quick;                ///< ⌘P / ctrl+P: the project's files
    SearchAllModel          search_all;           ///< ⌘⇧F: text across the project

    /// The tab in front: what the keyboard, the mouse and the view act on.
    [[nodiscard]] Session& tab() noexcept { return tabs.active(); }
    [[nodiscard]] const Session& tab() const noexcept { return tabs.active(); }
    [[nodiscard]] std::size_t tab_count() const noexcept { return tabs.size(); }
    [[nodiscard]] std::size_t active() const noexcept { return tabs.index(); }
    [[nodiscard]] Session& at(std::size_t i) noexcept { return tabs[i]; }
    [[nodiscard]] const Session& at(std::size_t i) const noexcept { return tabs[i]; }
    [[nodiscard]] float strip_h() const noexcept;
};

}  // namespace hand
