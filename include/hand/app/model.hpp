#pragma once
// hand::app model — everything hand knows, as plain typed data.
//
//   Session   one tab: its pty, its screen, and what animates over it
//   Model     the window: config, the tabs (the active Session IS the Model,
//             so handlers work on "the terminal" unchanged), and the modes
//             that sit over it: copy mode, hints, the dialog, the host picker,
//             the settings window
//
// A mode that may be absent is an optional, never a bool next to its data:
// `copy` is engaged exactly while copy mode is on; `ask` exactly while a
// dialog is asking. What a value means is in its type.

#include <hand/app/model/chrome.hpp>
#include <hand/app/model/modes.hpp>
#include <hand/app/model/pane.hpp>
#include <hand/app/model/ssh.hpp>
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

struct Session {
    SessionId               sid = kNoSession;     ///< stable id: routes pty output, keys the stream
    std::shared_ptr<Pty>    pty;
    std::shared_ptr<Term>   term;
    std::string             title = "hand";       ///< OSC 0/2
    std::string             name;                 ///< a name the user gave the tab ("" = automatic)
    double                  bell_at = -1.0;
    bool                    selecting = false;
    int                     mouse_button = -1;    ///< reported to the app while held
    int                     mouse_cell_col = -1, mouse_cell_row = -1;   ///< last motion reported (once per cell)
    bool                    search_open = false;
    mayag::TextEditState    search_query;
    std::string             hover_url;
    // smooth scrollback: the offset DRAWN (rows above live, fractional),
    // where it is heading, and the whole-row offset toe was last given
    float                   scroll_d = 0.0f, scroll_goal = 0.0f;
    int                     scroll_applied = 0;
    int                     new_below = 0;        ///< lines that arrived while scrolled back
    float                   sel_edge = 0.0f;      ///< selection drag past an edge scrolls (rows/s, + = up)
    mayag::Vec2             sel_pos{};
    int                     sel_clicks = 0;       ///< 1 plain, 2 word, 3 line
    mayag::Vec2             sel_down{};           ///< where the press was (terminal coords): a drag must leave it
    float                   tail_t = 0.0f;        ///< the live tail, 0..1 (eased)
    float                   sticky_t = 0.0f;      ///< the sticky command header, 0..1 (eased)
    float                   cur_a = 1.0f;         ///< the cursor's opacity: the blink, faded
    motion::State           fx;                   ///< cursor physics + particles
    double                  active_at = -10.0;    ///< last keystroke / output
    float                   out_shift = 0.0f;     ///< a new line at the live prompt: rows still to glide up
    float                   cur_x = 0.0f, cur_y = 0.0f;   ///< the smooth cursor, in cells
    bool                    cur_known = false;
    std::uint64_t           sticky_id = 0;        ///< the command the sticky header shows
    std::uint64_t           notified = 0;         ///< the last command reported on
    std::optional<CopyMode> copy;                 ///< engaged: copy mode is on
    double                  copy_flash_at = -10.0;
    std::string             pending_paste;        ///< a paste waiting on the dialog
    std::optional<HintMode> hint;                 ///< engaged: hints are showing
    // what the tab strip shows while it is in the background
    bool                    unread = false;
    bool                    bell = false;
    std::size_t             bytes = 0;            ///< output since the last activity sample
    std::vector<float>      activity;             ///< output rate, one sample a second
    mutable std::string     proc;                 ///< foreground process name, cached
    mutable double          proc_at = -10.0;
    // ssh: the destination (launched, or found by watching an `ssh host` you typed)
    mutable std::optional<Host> remote;
    mutable double          remote_at = -10.0;
    Remote                  link;
    double                  opened_at = 0.0;      ///< the tab's own entrance animation
    // snapshot cache (view is const; the grid only changes with the term)
    mutable std::shared_ptr<const mayag::dsl::TermGrid> grid;
    mutable std::uint64_t   grid_key = 0;
};

struct Model : Session {
    Config                  cfg;
    Palette                 palette;
    mayag::Vec2             win{800, 540};        ///< the window
    mayag::Vec2             size{800, 500};       ///< the terminal area (the window below the tab strip)
    float                   font_size = 13.0f;    ///< cfg.font_size, or zoomed
    bool                    focused = true;
    std::string             preedit;              ///< the input method's composition, drawn at the cursor (not yet typed)
    bool                    cursor_on = true;
    double                  time = 0.0;
    mayag::dsl::TermMetrics metrics{};            ///< cell size, from the font (fit())
    // tabs: the parked sessions, in strip order, with a hole where the active one goes
    std::vector<Session>    tabs;                 ///< tabs[active] is a placeholder (the Model holds it)
    std::size_t             active = 0;
    SessionIds              session_ids;          ///< hands out each new session's id
    StripModel              strip;                ///< the tab strip
    bool                    ctrl_held = false;    ///< ctrl is down: links are armed
    MinimapModel            minimap;
    std::uint64_t           clock = 0;            ///< 1 Hz: live timers, activity samples
    double                  resized_at = -10.0;
    bool                    sized_once = false;
    ToastModel              toast;
    mutable mayag::Rect     sel_chip{};           ///< where the selection chip was drawn (terminal coords)
    // what floats over the terminal, and what sits beside it
    SettingsModel           settings;             ///< ctrl+,
    DialogModel             dialog;               ///< the one open question
    SshModel                ssh;                  ///< ctrl+shift+s picker, ctrl+shift+h manager
    PaneModel               pane;                 ///< ctrl+shift+e

    [[nodiscard]] std::size_t tab_count() const noexcept { return std::max<std::size_t>(tabs.size(), 1); }
    [[nodiscard]] float strip_h() const noexcept;
    /// The session in slot i, whether it is the active one or parked.
    [[nodiscard]] const Session& at(std::size_t i) const { return i == active ? static_cast<const Session&>(*this) : tabs[i]; }
    [[nodiscard]] Session& at(std::size_t i) { return i == active ? static_cast<Session&>(*this) : tabs[i]; }
};

}  // namespace hand
