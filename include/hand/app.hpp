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
#include "pty.hpp"
#include "term.hpp"

#include <mayag/mayag.hpp>
#include <mayag/widget/settings_panel.hpp>
#include <mayag/widget/terminal_view.hpp>
#include <mayag/widget/toast.hpp>

#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace hand {

struct App {
    struct Model {
        Config                  cfg;
        Palette                 palette;
        std::shared_ptr<Pty>    pty;
        std::shared_ptr<Term>   term;
        mayag::Vec2             size{800, 500};
        float                   font_size = 13.0f;   ///< cfg.font_size, or zoomed
        bool                    focused = true;
        bool                    cursor_on = true;
        double                  bell_at = -1.0, time = 0.0;
        mayag::dsl::TermMetrics metrics{};           ///< cell size, from the font (fit())
        bool                    selecting = false;
        int                     mouse_button = -1;   ///< reported to the app while held
        std::string             title = "hand";
        // settings panel (mayag settings_panel owns the interaction; we own its state)
        bool                    settings_open = false;
        mayag::dsl::SettingsPanelState panel;
        // scrollback search (ctrl+shift+f)
        bool                    search_open = false;
        mayag::TextEditState    search_query;
        // links: ctrl+click opens; ctrl+hover underlines
        bool                    ctrl_held = false;
        std::string             hover_url;
        // smooth scrollback: the offset DRAWN (rows above live, fractional),
        // where it is heading, and the whole-row offset toe was last given
        float                   scroll_d = 0.0f, scroll_goal = 0.0f;
        int                     scroll_applied = 0;
        int                     new_below = 0;   ///< lines that arrived while scrolled back
        // selection drag past the top/bottom edge scrolls (rows per second, + = up)
        float                   sel_edge = 0.0f;
        mayag::Vec2             sel_pos{};
        // the minimap: open while the pointer is near the right edge (eased width)
        bool                    mm_open = false;
        float                   mm_w = 14.0f;
        float                   mm_hover_y = -1.0f;   ///< pointer over the minimap (window y), -1 = not
        float                   peek_t = 0.0f;        ///< the peek card, 0..1 (eased)
        float                   tail_t = 0.0f;        ///< the live tail, 0..1 (eased)
        float                   sticky_t = 0.0f;      ///< the sticky command header, 0..1 (eased)
        std::uint64_t           sticky_id = 0;        ///< whose header is showing (a change replays the slide)
        std::uint64_t           clock = 0;            ///< 1 Hz while a running command's timer is on screen
        // the smooth cursor: where it is drawn, in cells (eases to the real cell)
        float                   cur_x = 0.0f, cur_y = 0.0f;
        bool                    cur_known = false;
        // resize HUD, toasts
        double                  resized_at = -10.0;
        bool                    sized_once = false;
        std::optional<mayag::dsl::Toast> toast;
        double                  toast_at = -10.0;
        // shell integration: the last command we reported on
        std::uint64_t           notified = 0;
        // hints (ctrl+shift+space): label the actionable things on screen
        bool                    hinting = false;
        std::vector<Term::Hint> hints;
        std::vector<std::string> hint_labels;
        std::string             hint_typed;
        // snapshot cache (view is const; the grid only changes with the term)
        mutable std::shared_ptr<const mayag::dsl::TermGrid> grid;
        mutable std::uint64_t   grid_key = 0;
    };

    struct Output { std::string bytes; };
    struct Exited { int code; };
    struct Blink {};
    struct Tick { double dt; };
    struct Copy {};
    struct PasteReq {};
    struct Pasted { std::string text; };
    struct Panel { mayag::dsl::SettingsOp op; };
    struct ToggleSettings {};
    struct Search { int op; };   ///< 0 open/close, 1 next, -1 prev
    struct OpenUrl { std::string url; };
    struct HintPick { std::size_t i; bool insert; };   ///< act on hint i (insert: type it at the prompt)
    struct CopyOutput {};                              ///< the last command's output to the clipboard
    struct Clock {};                                   ///< 1 Hz: live timers
    using Msg = std::variant<Output, Exited, Blink, Tick, Copy, PasteReq, Pasted, Panel, ToggleSettings, Search, OpenUrl,
                             HintPick, CopyOutput, Clock,
                             mayag::Edit<Model>>;
    using Cmd = mayag::Cmd<Msg, mayag::set_title, mayag::write_clipboard, mayag::read_clipboard>;
    using Sub = mayag::Sub<Msg>;

    static inline Config config{};   ///< set by main() before run
    /// Advance of one mono cell at `size` pt in face `family` (main binds the font stack).
    static inline float (*cell_width)(float size, int family) = [](float size, int) { return size * 0.6f; };

    static Cmd init(Model& m);
    static Cmd update(Model& m, Output o);
    static Cmd update(Model& m, Exited e);
    static Cmd update(Model& m, Blink);
    static Cmd update(Model& m, Tick t);
    static Cmd update(Model& m, HintPick h);
    static Cmd update(Model& m, CopyOutput);
    static Cmd update(Model& m, Clock) { ++m.clock; m.grid_key = 0; return {}; }
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
    static std::optional<Msg> on(const Model& m, const mayag::ScrollEvent& s);
    static std::optional<Msg> on(const Model& m, const mayag::ResizeEvent& r);
    static std::optional<Msg> on(const Model& m, const mayag::FocusEvent& f);
    static std::optional<Msg> on(const Model& m, const mayag::MouseDown& e);
    static std::optional<Msg> on(const Model& m, const mayag::MouseMove& e);
    static std::optional<Msg> on(const Model& m, const mayag::MouseUp& e);
    static std::optional<Msg> on(const Model& m, const mayag::Clicked& c);
    static std::optional<Msg> on(const Model& m, const mayag::Left& l);

    static Sub subscribe(const Model& m);
    static mayag::Theme theme(const Model& m);
    static mayag::Node view(const Model& m);

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
