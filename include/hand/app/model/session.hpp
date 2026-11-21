#pragma once
// hand::model — one tab: its pty, its screen, and what animates over it.

#include <hand/app/model/modes.hpp>
#include <hand/config/config.hpp>
#include <hand/core/pty.hpp>
#include <hand/core/term.hpp>
#include <hand/core/types.hpp>
#include <hand/ssh/hosts.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Threads share ONE Pty (CoOwned): the tab writes and resizes it, its reader
// reads and reaps it. Every member is a syscall on an fd the kernel makes
// thread-safe (write / ioctl / read / waitpid), fd_ and pid_ are set by
// spawn() before the pty is shared and never change, and reaped_ is written
// by the one reader and read only in ~Pty, after every owner let go.
MAYAG_SYNC(hand::Pty);

namespace hand {

/// A tab's pty, owned JOINTLY by the tab and its reader thread. The fd is
/// closed (and the child hung up) only when the last of them lets go, so a
/// tab closed mid-read can never leave the reader on a closed -- or reused --
/// fd. Empty when the tab has no shell (it ended, or ssh dropped).
class PtyRef {
  public:
    PtyRef() = default;
    explicit PtyRef(mayag::CoOwned<Pty> p) : p_(std::move(p)) {}
    /// A pty for a new tab, not yet spawned.
    [[nodiscard]] static PtyRef make() { return PtyRef{mayag::CoOwned<Pty>::make()}; }
    [[nodiscard]] Pty* operator->() const noexcept { return &p_->get(); }
    [[nodiscard]] Pty& operator*() const noexcept { return p_->get(); }
    explicit operator bool() const noexcept { return p_.has_value(); }
    void reset() noexcept { p_.reset(); }
    /// Another owner, for the reader thread.
    [[nodiscard]] mayag::CoOwned<Pty> share() const { return *p_; }

  private:
    std::optional<mayag::CoOwned<Pty>> p_;
};

/// One tab: a shell on a pty, the screen toe keeps for it, and everything
/// that animates over it. Lives in Tabs; the window never holds one itself.
struct Session {
    SessionId               sid = kNoSession;     ///< stable id: routes pty output, keys the stream
    PtyRef                  pty;
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

}  // namespace hand
