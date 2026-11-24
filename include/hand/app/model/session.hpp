#pragma once
// hand::model — one tab: its pty, its screen, and what animates over it.

#include <hand/app/model/modes.hpp>
#include <hand/config/config.hpp>
#include <hand/core/pty.hpp>
#include <hand/core/term.hpp>
#include <hand/core/types.hpp>
#include <hand/ssh/hosts.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/fuzzy.hpp>
#include <mayag/widget/text_editor.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hand {

/// A tab's pty, owned JOINTLY by the tab and its reader thread. The fd is
/// closed (and the child hung up) only when the last of them lets go, so a
/// tab closed mid-read can never leave the reader on a closed -- or reused --
/// fd. Empty when the tab has no shell (it ended, or ssh dropped).
class PtyRef {
  public:
    PtyRef() = default;
    explicit PtyRef(mayag::CoOwned<Pty> p) : p_(std::move(p)) {}
    /// Start `spec` on a new pty: empty when it didn't start.
    [[nodiscard]] static PtyRef make(const PtySpec& spec) {
        auto p = mayag::CoOwned<Pty>::make(spec);
        return p->ok() ? PtyRef{std::move(p)} : PtyRef{};
    }
    [[nodiscard]] Pty* operator->() const noexcept { return &p_->get(); }
    [[nodiscard]] Pty& operator*() const noexcept { return p_->get(); }
    explicit operator bool() const noexcept { return p_.has_value(); }
    void reset() noexcept { p_.reset(); }
    /// Another owner, for the reader thread.
    [[nodiscard]] mayag::CoOwned<Pty> share() const { return *p_; }

  private:
    std::optional<mayag::CoOwned<Pty>> p_;
};

/// ssh to a host: what the effect needs to build its command line (which
/// may create ssh's control-socket directory, so it is built there).
struct SshTarget { Host host; HostBook book; SshFlags flags; };
/// The program a session asks for: a shell (with hand's integration if
/// asked; the effect resolves $SHELL), a `-e` command line, or ssh.
struct Launch {
    PtySpec                  spec;
    bool                     integrate = false;
    std::optional<SshTarget> ssh;
};

/// A file open in a tab: what the editor holds, and where it came from.
/// A tab with one is an EDITOR tab: no pty is ever started for it (its Term
/// is a tiny blank grid, so code that asks any tab for its screen still
/// works), and the keyboard, the pointer and the view belong to the editor.
struct FileDoc {
    std::string                    path;          ///< absolute, on `host` (local when none)
    std::optional<Host>            host;          ///< the machine it lives on: read and saved over its shared ssh connection
    mayag::dsl::TextEditorState    st;            ///< the text, cursors, find
    bool                           loading = true;
    std::string                    error;         ///< could not open it (the tab says why, and edits nothing)
    std::string                    save_error;    ///< the last save failed: why (the text is still here)
    std::optional<std::uint64_t>   saving;        ///< a write of this save point is in flight
    bool                           close_after_save = false;   ///< "save" in the close dialog
    std::int64_t                   disk_mtime = 0; ///< the file's mtime when last read or written (watching for outside edits)
    std::optional<mayag::editor::Range> jump;     ///< where to land once the text arrives (a search hit)
};

/// ⌘P's files ranked against one query: (index into files, hits), best first.
struct QuickRanked {
    std::shared_ptr<const std::vector<std::string>>      files;   ///< the list it ranked (held: its address can't be reused)
    std::string                                          q;
    std::vector<std::pair<int, mayag::dsl::FuzzyMatch>>  rows;
};

/// ⌘P: every file of the project, fuzzy-found.
struct QuickOpenModel {
    bool                     open = false;
    bool                     loading = false;
    mayag::TextEditState     q;
    int                      sel = 0;
    std::string              root;              ///< absolute; files are relative to it
    std::optional<Host>      host;              ///< the machine listed (none: this one)
    /// Up to 50k paths, never edited once listed: shared, so copying the model
    /// (every update) does not copy them.
    std::shared_ptr<const std::vector<std::string>> files = std::make_shared<const std::vector<std::string>>();
    bool                     truncated = false; ///< the project has more than the list holds
    mutable std::shared_ptr<const QuickRanked> ranked;   // memo: quick_open_ranked of (files, q)
};

/// One line that matched (⌘⇧F): `file` relative to the searched root; the
/// match is bytes [col, col+len) of the line (0-based line).
struct SearchHit { std::string file; int line = 0; int col = 0; int len = 0; std::string text; };

/// ⌘⇧F: the project's files searched for text.
struct SearchAllModel {
    bool                       open = false;
    bool                       searching = false;
    mayag::TextEditState       q;
    int                        sel = 0;
    std::uint64_t              token = 0;        ///< the newest query asked
    std::string                root;             ///< hits' files are relative to it
    std::optional<Host>        host;             ///< the machine searched (none: this one)
    std::vector<SearchHit>     hits;
    bool                       truncated = false;
    std::string                error;
};

/// One tab: a shell on a pty, the screen toe keeps for it, and everything
/// that animates over it. Lives in Tabs; the window never holds one itself.
struct Session {
    SessionId               sid = kNoSession;     ///< stable id: routes pty output, keys the stream
    PtyRef                  pty;
    /// Engaged: this tab is a file in the editor, not a shell.
    std::optional<FileDoc>  file;
    // What this tab wants of its pty. update only SAYS it (append bytes, ask
    // for a program, resize the term); App's root update turns the difference
    // into effects after every step (App::drain, src/app/fx/ performs them),
    // so no handler ever touches the pty itself.
    std::string             input;                ///< bytes for the pty, written after this step
    std::optional<Launch>   launch;               ///< start this (taken by the next drain)
    bool                    starting = false;     ///< asked for; its pty has not arrived yet
    int                     pty_cols = 0, pty_rows = 0;   ///< the size the pty was last told
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
    std::optional<HintMode> hint;                 ///< engaged: hints are showing
    // what the tab strip shows while it is in the background
    bool                    unread = false;
    bool                    bell = false;
    /// Output since the tab opened. Never reset: the activity sample keeps its
    /// own mark, so a reader of the total (the echo gate, flood's MB/s) can't
    /// race the 1 Hz sample -- it did, and a key's echo read as lost.
    std::uint64_t           bytes = 0;
    std::uint64_t           bytes_sampled = 0;    ///< `bytes` at the last activity sample
    std::vector<float>      activity;             ///< output rate, one sample a second
    // what runs in it, as the last probe found (Probe / Probed)
    std::string             proc;                 ///< the foreground process ("fish", "nvim")
    // ssh: the destination (launched, or found by watching an `ssh host` you typed)
    std::optional<Host>     remote;
    Remote                  link;
    double                  opened_at = 0.0;      ///< the tab's own entrance animation
    // snapshot cache: a pure function of the term and the decorations, keyed
    mutable std::shared_ptr<const mayag::dsl::TermGrid> grid;   // memo: equals recomputing it
    mutable std::uint64_t   grid_key = 0;                       // memo: what `grid` was made from
};

}  // namespace hand
