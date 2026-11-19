#pragma once
// hand::App — a foot-like terminal: one window, one shell, nothing else.
//
//   pty ──stream──▶ Output ──▶ Term::feed (toe) ──▶ view: terminal_view
//   keys/text/mouse ──▶ Term::encode_* ──▶ pty
//   ctrl+,  ──▶ mayag settings_panel over the terminal; every edit is live
//
// Shortcuts (foot's): ctrl+shift+c/v copy/paste, shift+insert paste,
// ctrl+= / ctrl+- / ctrl+0 font size, shift+pgup/pgdn scrollback.

#include "config.hpp"
#include "motion.hpp"
#include "pty.hpp"
#include "term.hpp"

#include <mayag/mayag.hpp>
#include <mayag/widget/settings_panel.hpp>
#include <mayag/widget/terminal_view.hpp>
#include <mayag/widget/tab_strip.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace hand {

struct App {
    /// One terminal: its shell, its screen, and everything that animates over
    /// it. The ACTIVE one lives in the Model itself (Model derives from it), so
    /// every handler works on "the terminal" unchanged; the others are parked
    /// in Model::tabs and swapped in on a tab switch.
    struct Session {
        std::uint64_t           sid = 0;              ///< stable id: routes pty output, keys the stream
        std::shared_ptr<Pty>    pty;
        std::shared_ptr<Term>   term;
        std::string             title = "hand";       ///< OSC 0/2
        std::string             name;                 ///< a name the user gave the tab ("" = automatic)
        double                  bell_at = -1.0;
        bool                    selecting = false;
        int                     mouse_button = -1;   ///< reported to the app while held
        bool                    search_open = false;
        mayag::TextEditState    search_query;
        std::string             hover_url;
        // smooth scrollback: the offset DRAWN (rows above live, fractional),
        // where it is heading, and the whole-row offset toe was last given
        float                   scroll_d = 0.0f, scroll_goal = 0.0f;
        int                     scroll_applied = 0;
        int                     new_below = 0;   ///< lines that arrived while scrolled back
        float                   sel_edge = 0.0f;  ///< selection drag past an edge scrolls (rows/s, + = up)
        mayag::Vec2             sel_pos{};
        int                     sel_clicks = 0;   ///< 1 plain, 2 word, 3 line: a plain click that never dragged selects nothing
        float                   tail_t = 0.0f;        ///< the live tail, 0..1 (eased)
        float                   sticky_t = 0.0f;      ///< the sticky command header, 0..1 (eased)
        float                   cur_a = 1.0f;         ///< the cursor's opacity: the blink, faded
        motion::State           fx;                   ///< cursor physics + particles
        double                  active_at = -10.0;    ///< last keystroke / output
        float                   out_shift = 0.0f;     ///< a new line at the live prompt: rows still to glide up
        std::uint64_t           sticky_id = 0;
        float                   cur_x = 0.0f, cur_y = 0.0f;   ///< the smooth cursor, in cells
        bool                    cur_known = false;
        std::uint64_t           notified = 0;         ///< the last command reported on
        // copy mode (ctrl+shift+m): a keyboard cursor through the scrollback, vim keys
        bool                    copy_mode = false;
        std::int64_t            cm_row = 0;           ///< absolute
        int                     cm_col = 0;
        int                     cm_vis = 0;           ///< 0 off, 1 v (characters), 2 V (lines), 3 ^v (block)
        std::int64_t            cm_arow = 0;          ///< where visual mode started
        int                     cm_acol = 0;
        double                  copy_flash_at = -10.0;
        std::string             pending_paste;        ///< a paste waiting on the dialog
        bool                    hinting = false;      ///< hints (ctrl+shift+space)
        std::vector<Term::Hint> hints;
        std::vector<std::string> hint_labels;
        std::string             hint_typed;
        // what the tab strip shows while it is in the background
        bool                    unread = false;       ///< output since it was last looked at
        bool                    bell = false;         ///< BEL since it was last looked at
        std::size_t             bytes = 0;            ///< output since the last activity sample
        std::vector<float>      activity;             ///< output rate, one sample a second
        mutable std::string     proc;                 ///< foreground process (from /proc), cached
        mutable double          proc_at = -10.0;
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
        float                   font_size = 13.0f;   ///< cfg.font_size, or zoomed
        bool                    focused = true;
        bool                    cursor_on = true;
        double                  time = 0.0;
        mayag::dsl::TermMetrics metrics{};           ///< cell size, from the font (fit())
        // tabs: the parked sessions, in strip order, with a hole where the active one goes
        std::vector<Session>    tabs;                 ///< tabs[active] is a placeholder (the Model holds it)
        std::size_t             active = 0;
        std::uint64_t           next_sid = 1;
        mayag::dsl::TabStripMotion tab_motion;       ///< the sliding pill
        float                   strip_t = 0.0f;       ///< the strip, 0..1 (eased in when a second tab opens)
        float                   switch_t = 1.0f;      ///< the content's slide on a tab switch, 0..1
        int                     switch_dir = 0;       ///< -1 came from the right, +1 from the left
        int                     drag_tab = -1;        ///< a tab being dragged to reorder
        float                   drag_dx = 0.0f;
        bool                    show_numbers = false; ///< alt pressed: alt+N hints on the tabs (for a moment)
        double                  numbers_at = -10.0;
        // settings panel (mayag settings_panel owns the interaction; we own its state)
        bool                    settings_open = false;
        mayag::dsl::SettingsPanelState panel;
        bool                    ctrl_held = false;
        // the minimap: open while the pointer is near the right edge (eased width)
        bool                    mm_open = false;
        float                   mm_w = 14.0f;
        float                   mm_hover_y = -1.0f;   ///< pointer over the minimap (terminal y), -1 = not
        float                   peek_t = 0.0f;        ///< the peek card, 0..1 (eased)
        float                   settings_t = 0.0f;    ///< the settings panel, 0..1 (eased in AND out)
        std::uint64_t           clock = 0;            ///< 1 Hz: live timers, activity samples
        double                  resized_at = -10.0;
        bool                    sized_once = false;
        std::optional<mayag::dsl::Toast> toast;
        mutable mayag::Rect     sel_chip{};           ///< where the selection toolbar was drawn (terminal coords)
        // the confirmation dialog: one at a time, eased in and out
        enum class Ask : std::uint8_t { none, paste, close_tab, quit };
        Ask                     ask = Ask::none;
        std::size_t             ask_tab = 0;          ///< close_tab: which
        int                     ask_focus = 0;        ///< the focused action
        bool                    ask_remember = false;
        float                   ask_t = 0.0f;         ///< 0..1, eased toward (ask != none)
        mutable mayag::dsl::TermDialogSpec ask_last;  ///< what was shown (drawn while it fades out)
        double                  toast_at = -10.0;

        [[nodiscard]] std::size_t tab_count() const noexcept { return std::max<std::size_t>(tabs.size(), 1); }
        [[nodiscard]] float strip_h() const noexcept;
        /// The session in slot i, whether it is the active one or parked.
        [[nodiscard]] const Session& at(std::size_t i) const { return i == active ? static_cast<const Session&>(*this) : tabs[i]; }
        [[nodiscard]] Session& at(std::size_t i) { return i == active ? static_cast<Session&>(*this) : tabs[i]; }
    };

    struct Output { std::string bytes; std::uint64_t sid = 0; };
    struct Exited { int code; std::uint64_t sid = 0; };
    struct NewTab {};
    struct CloseTab { std::size_t i; bool confirmed = false; };   ///< confirmed: no question asked
    struct SelectTab { std::size_t i; };
    struct StepTab { int d; };
    struct MoveTab { int d; };   ///< reorder the active tab
    struct Blink {};
    struct Tick { double dt; };
    struct Copy {};
    struct PasteReq {};
    struct Pasted { std::string text; bool confirmed = false; };   ///< confirmed: the dialog already said yes
    struct Panel { mayag::dsl::SettingsOp op; };
    struct ToggleSettings {};
    struct Search { int op; };   ///< 0 open/close, 1 next, -1 prev
    struct OpenUrl { std::string url; };
    struct HintPick { std::size_t i; bool insert; };   ///< act on hint i (insert: type it at the prompt)
    struct CopyOutput {};                              ///< the last command's output to the clipboard
    struct Clock {};                                   ///< 1 Hz: live timers
    struct CopyKey { mayag::Key key; mayag::Mods mods; std::string text; };   ///< a key in copy mode
    struct Answer { int action; };                     ///< the dialog: run action i (-1 = cancel)
    struct RequestQuit {};                             ///< the window's close button
    using Msg = std::variant<Output, Exited, Blink, Tick, Copy, PasteReq, Pasted, Panel, ToggleSettings, Search, OpenUrl,
                             HintPick, CopyOutput, Clock, CopyKey, Answer, RequestQuit, NewTab, CloseTab, SelectTab, StepTab, MoveTab,
                             mayag::Edit<Model>>;
    using Cmd = mayag::Cmd<Msg, mayag::set_title, mayag::write_clipboard, mayag::read_clipboard>;
    using Sub = mayag::Sub<Msg>;

    static inline Config config{};   ///< set by main() before run
    /// Advance of one mono cell at `size` pt in face `family` (main binds the font stack).
    static inline float (*cell_width)(float size, int family) = [](float size, int) { return size * 0.6f; };

    static Cmd init(Model& m);
    static Cmd update(Model& m, Output o);
    static Cmd update(Model& m, Exited e);
    static Cmd update(Model& m, NewTab);
    static Cmd update(Model& m, CloseTab c);
    static Cmd update(Model& m, SelectTab s);
    static Cmd update(Model& m, StepTab s);
    static Cmd update(Model& m, MoveTab s);
    static Cmd update(Model& m, Blink);
    static Cmd update(Model& m, Tick t);
    static Cmd update(Model& m, HintPick h);
    static Cmd update(Model& m, CopyOutput);
    static Cmd update(Model& m, CopyKey k);
    static Cmd update(Model& m, Answer a);
    static Cmd update(Model& m, RequestQuit);
    static Cmd update(Model& m, Clock);
    static Cmd update(Model& m, Copy);
    static Cmd update(Model& m, PasteReq);
    static Cmd update(Model& m, Pasted p);
    static Cmd update(Model& m, Panel p);
    static Cmd update(Model& m, ToggleSettings);
    static Cmd update(Model& m, Search s);
    static Cmd update(Model& m, OpenUrl u);
    static Cmd update(Model& m, mayag::Edit<Model> e) { e(m); settle_scroll(m); return {}; }

    static std::optional<Msg> on(const Model& m, const mayag::KeyEvent& k);
    static std::optional<Msg> on(const Model& m, const mayag::TextEvent& t);
    static std::optional<Msg> on(const Model& m, const mayag::ScrollEvent& s_win);
    static std::optional<Msg> on(const Model& m, const mayag::ResizeEvent& r);
    static std::optional<Msg> on(const Model& m, const mayag::FocusEvent& f);
    static std::optional<Msg> on(const Model& m, const mayag::MouseDown& e_win);
    static std::optional<Msg> on(const Model& m, const mayag::MouseMove& e_win);
    static std::optional<Msg> on(const Model& m, const mayag::MouseUp& e_win);
    static std::optional<Msg> on(const Model& m, const mayag::Clicked& c);
    static std::optional<Msg> on(const Model& m, const mayag::Left& l);
    static std::optional<Msg> on(const Model& m, const mayag::CloseRequest&) { (void)m; return RequestQuit{}; }
    static std::optional<Msg> on(const Model& m, const mayag::Pressed& p);
    static std::optional<Msg> on(const Model& m, const mayag::Dragged& d);
    static std::optional<Msg> on(const Model& m, const mayag::Released& r);

    static Sub subscribe(const Model& m);
    static mayag::Theme theme(const Model& m);
    static mayag::Node view(const Model& m);

    /// Start a shell in a new session (in `cwd`, "" = the current one).
    static Session spawn_session(Model& m, const std::string& cwd);
    /// Resize the grid (and the pty) to what the window holds.
    static void fit(Model& m);
    /// Smooth scrollback. scroll_by: move the goal by `rows` (+ = into
    /// history). settle_scroll: anything that moved toe's view directly
    /// (typing, prompt jumps, search, the minimap) becomes the new goal and
    /// is animated to instead of jumped to.
    static void scroll_by(Model& m, float rows);
    static void settle_scroll(Model& m);
    /// Bring the terminal in line with m.cfg after an edit.
    static void apply(Model& m, const Config& before);
};

}  // namespace hand
