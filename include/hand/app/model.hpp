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

#include <hand/config/config.hpp>
#include <hand/core/pty.hpp>
#include <hand/core/term.hpp>
#include <hand/ssh/hosts.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/settings_panel.hpp>
#include <mayag/widget/tab_strip.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/terminal_view.hpp>
#include <mayag/widget/toast.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hand {

/// Copy mode (ctrl+shift+m): a keyboard cursor through the scrollback, vim keys.
struct CopyMode {
    enum class Visual : std::uint8_t { none, chars, lines, block };   ///< v, V, ^v
    std::int64_t row = 0;          ///< absolute line of the cursor
    int          col = 0;
    Visual       visual = Visual::none;
    std::int64_t anchor_row = 0;   ///< where visual mode started
    int          anchor_col = 0;
};

/// Hints (ctrl+shift+space): every URL / path / hash on screen, labelled.
struct HintMode {
    std::vector<Term::Hint>  hints;
    std::vector<std::string> labels;
    std::string              typed;
};

/// The confirmation dialog: one question at a time.
struct Ask {
    enum class Kind : std::uint8_t { paste, close_tab, quit };
    Kind        kind = Kind::paste;
    std::size_t tab = 0;            ///< close_tab: which
    int         focus = 0;          ///< the focused action
    bool        remember = false;   ///< "don't ask again" ticked
};

/// The machine a tab talks to over ssh.
struct Remote {
    bool   launched = false;        ///< hand ran ssh as the tab's program (so it can reconnect)
    bool   dropped = false;         ///< it ended badly: the tab stays, offering reconnect
    double dropped_at = -10.0;
    double connected_at = -10.0;
};

struct Session {
    std::uint64_t           sid = 0;              ///< stable id: routes pty output, keys the stream
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
    std::uint64_t           next_sid = 1;
    mayag::dsl::TabStripMotion tab_motion;        ///< the sliding pill
    float                   strip_t = 0.0f;       ///< the strip, 0..1 (eased in when a second tab opens)
    float                   switch_t = 1.0f;      ///< the content's slide on a tab switch, 0..1
    int                     switch_dir = 0;       ///< -1 came from the right, +1 from the left
    int                     drag_tab = -1;        ///< a tab being dragged to reorder
    float                   drag_dx = 0.0f;
    bool                    show_numbers = false; ///< alt pressed: alt+N hints on the tabs (for a moment)
    double                  numbers_at = -10.0;
    // settings (ctrl+,): a panel over the terminal. settings_panel owns the
    // interaction; we own its state.
    bool                    settings_open = false;
    mayag::dsl::SettingsPanelState panel;
    float                   settings_t = 0.0f;    ///< the panel, 0..1 (eased in AND out)
    bool                    ctrl_held = false;
    // the minimap: open while the pointer is near the right edge (eased width)
    bool                    mm_open = false;
    float                   mm_w = 14.0f;
    float                   mm_hover_y = -1.0f;   ///< pointer over the minimap (terminal y), -1 = not
    float                   peek_t = 0.0f;        ///< the peek card, 0..1 (eased)
    std::uint64_t           clock = 0;            ///< 1 Hz: live timers, activity samples
    double                  resized_at = -10.0;
    bool                    sized_once = false;
    std::optional<mayag::dsl::Toast> toast;
    double                  toast_at = -10.0;
    mutable mayag::Rect     sel_chip{};           ///< where the selection chip was drawn (terminal coords)
    // the confirmation dialog: one at a time, eased in and out
    std::optional<Ask>      ask;
    float                   ask_t = 0.0f;         ///< 0..1, eased toward ask.has_value()
    mutable mayag::dsl::TermDialogSpec ask_last;  ///< what was shown (drawn while it fades out)
    // the ssh host picker (ctrl+shift+s)
    bool                    hosts_open = false;
    mayag::TextEditState    hosts_query;
    int                     hosts_sel = 0;
    HostSources             host_src;
    HostBook                book;                 ///< loaded when the picker opens (files can change)

    [[nodiscard]] std::size_t tab_count() const noexcept { return std::max<std::size_t>(tabs.size(), 1); }
    [[nodiscard]] float strip_h() const noexcept;
    /// The session in slot i, whether it is the active one or parked.
    [[nodiscard]] const Session& at(std::size_t i) const { return i == active ? static_cast<const Session&>(*this) : tabs[i]; }
    [[nodiscard]] Session& at(std::size_t i) { return i == active ? static_cast<Session&>(*this) : tabs[i]; }
};

}  // namespace hand
