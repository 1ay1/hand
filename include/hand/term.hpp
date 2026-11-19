#pragma once
// hand::Term — toe's terminal model, plus what a host needs around it.
//
//   bytes from the pty ──feed()──▶ toe::term::Model ──snapshot()──▶ mayag TermGrid
//   keys / text / paste ──encode──▶ bytes for the pty
//
// toe owns every VT semantic (parser, screen, scrollback, selection, modes).
// This file only translates between toe's types and mayag's widget types.
// Kept out of the app header so editing the view never recompiles toe glue.

#include "config.hpp"

#include <mayag/core/event.hpp>
#include <mayag/widget/scrollback_bar.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hand {

/// What the host must do after feeding output.
struct Effects {
    std::string reply;          ///< bytes to write back to the pty (query answers)
    std::string title;          ///< new window title, if set
    bool        title_set = false;
    std::string clipboard;      ///< OSC 52 copy
    bool        clipboard_set = false;
    bool        bell = false;
};

class Term {
  public:
    Term(int cols, int rows, const Palette& pal, int scrollback = 10000);
    ~Term();
    Term(Term&&) noexcept;
    Term& operator=(Term&&) noexcept;

    [[nodiscard]] int cols() const;
    [[nodiscard]] int rows() const;

    /// Fold pty output into the model.
    Effects feed(std::string_view bytes);
    void resize(int cols, int rows);
    /// Swap the colour scheme live (keeps colours the app set via OSC 4).
    void set_palette(const Palette& p);
    /// Change scrollback capacity (drops history beyond it).
    void set_scrollback(int lines);

    // ── input → bytes for the pty ──────────────────────────────────────
    /// A named key (arrows, enter, F-keys, ctrl-letters...). Empty when the
    /// key produces nothing (pure modifiers).
    [[nodiscard]] std::string encode_key(mayag::Key key, mayag::Mods mods) const;
    /// Committed text (typing, IME).
    [[nodiscard]] std::string encode_text(std::string_view utf8, mayag::Mods mods) const;
    /// A paste, bracketed when the app asked for it.
    [[nodiscard]] std::string encode_paste(std::string_view utf8) const;
    /// Focus in/out report (DEC 1004), or "" when not requested.
    [[nodiscard]] std::string encode_focus(bool focused) const;

    // ── mouse ──────────────────────────────────────────────────────────
    /// Does the running app want the mouse (vim, htop, tmux)? Shift bypasses.
    [[nodiscard]] bool mouse_reporting() const;
    /// SGR/X10 mouse report for a button press(0..2)/release/motion/wheel(64/65).
    [[nodiscard]] std::string encode_mouse(int button, int col, int row, bool press, bool motion, mayag::Mods mods) const;

    // ── scrollback + selection ─────────────────────────────────────────
    void scroll(int lines);   ///< +up into history, -down
    void scroll_to_bottom();
    [[nodiscard]] bool alt_screen() const;
    [[nodiscard]] int  scroll_offset() const;
    [[nodiscard]] int  history() const;
    void select_begin(int col, int row, int clicks);   ///< viewport coords; 2 = word, 3 = line
    void select_extend(int col, int row);
    void select_clear();
    [[nodiscard]] bool has_selection() const;
    [[nodiscard]] std::string selection_text() const;

    // ── links ───────────────────────────────────────────────────────────────
    /// The link under a cell: an OSC 8 hyperlink, else a URL in the text.
    [[nodiscard]] std::string link_at(int col, int row) const;
    /// Underline the OSC 8 link under a cell (-1,-1 clears). True if it changed.
    bool hover(int col, int row);

    // ── search ─────────────────────────────────────────────────────────────
    /// Search scrollback + screen; returns the match count (smart case).
    std::size_t search(std::string_view query);
    void search_next();
    void search_prev();
    void search_clear();
    [[nodiscard]] std::size_t search_count() const;
    [[nodiscard]] std::size_t search_current() const;   ///< 1-based, 0 = none
    /// Search matches as absolute rows (for the minimap).
    [[nodiscard]] std::vector<std::int64_t> search_rows() const;

    // ── shell integration (OSC 133 / 7) ───────────────────────────────────────
    struct Command {
        std::uint64_t id = 0;
        std::int64_t  prompt_row = -1, output_row = -1, end_row = -1;   ///< absolute
        bool          ran = false;       ///< a command was entered (C seen)
        bool          finished = false;
        int           exit_code = 0;
        std::int64_t  duration_ms = 0;
        std::int64_t  start_ms = 0;      ///< wall clock (ms since epoch) when output began; 0 = unknown
        std::string   cwd;               ///< OSC 7 directory at its prompt, as a path ("" = unknown)
    };
    [[nodiscard]] std::vector<Command> commands() const;
    [[nodiscard]] std::string command_text(const Command& c) const;   ///< the typed line
    [[nodiscard]] std::string output_text(const Command& c) const;
    /// Scroll to the previous (-1) / next (+1) prompt. False when there is none.
    bool jump_prompt(int dir);
    [[nodiscard]] std::string cwd() const;   ///< OSC 7, as a path ("" if unknown)

    // ── the scrollback as a whole (minimap) ──────────────────────────────────
    struct ScrollInfo { int total = 0, first = 0, visible = 0; };   ///< in absolute rows
    [[nodiscard]] ScrollInfo scroll_info() const;
    /// The scrollback binned into `buckets` minimap rows: one sampled line per
    /// bucket, as coloured runs (cached until the content changes).
    [[nodiscard]] const std::vector<mayag::dsl::ScrollbackRun>& minimap(int buckets) const;
    void scroll_to_line(int first);

    // ── cursor (viewport cell; visible = false when off screen / hidden) ──
    struct Cursor { int col = 0, row = 0; bool visible = false; };
    [[nodiscard]] Cursor cursor() const;

    // ── hints: the actionable things on screen ─────────────────────────────
    struct Hint {
        enum class Kind : std::uint8_t { url, path, hash, ip, number } kind = Kind::url;
        int row = 0, col = 0, len = 0;   ///< viewport cells
        std::string text;
    };
    /// URLs, paths, git hashes, IPs on the visible screen (bottom first).
    [[nodiscard]] std::vector<Hint> hints() const;

    /// The visible screen as mayag's TermGrid. `above`: also the row just
    /// above the viewport, as line 0 (for sub-row smooth scrolling); every
    /// row index in the grid is then +1.
    [[nodiscard]] mayag::dsl::TermGrid snapshot(bool above = false) const;
    /// Any absolute rows as a grid (a peek / live tail), at most `max_cols` wide.
    [[nodiscard]] mayag::dsl::TermGrid grid_at(std::int64_t first_abs, int rows, int max_cols = 0) const;
    [[nodiscard]] std::int64_t total_rows() const;
    [[nodiscard]] std::int64_t cursor_abs() const;   ///< the live cursor's absolute row
    /// Set the scrollback position directly (0 = live, history() = top).
    void set_scroll_offset(int offset);
    /// The command whose OUTPUT is at the top of the view, with its prompt
    /// scrolled away (the sticky header), if any.
    [[nodiscard]] std::optional<Command> command_at_top() const;
    /// Has the running app chosen a cursor shape (DECSCUSR)? Else the config's applies.
    [[nodiscard]] bool app_cursor_shape() const;
    /// Bumps on every change that affects snapshot(); for caching.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    mutable std::vector<mayag::dsl::ScrollbackRun> mm_;
    mutable std::uint64_t mm_key_ = 0;
    std::uint64_t         epoch_ = 1;
    std::uint64_t         content_epoch_ = 1;   ///< output / resize / palette only (not scrolling)
};

}  // namespace hand
