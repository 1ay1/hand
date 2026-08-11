// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/actor/tab_actor.hpp — a tab as an independent toe on its own thread.
//
// Each tab is exactly what "open the terminal again" is: a full toe::Terminal
// with its own PTY + grid, running its own blocking loop on its own std::thread.
// Tabs share NOTHING; they touch the outside world only through two mailboxes:
//
//   * inbound  (TabCmd): the GUI posts input bytes / resize / stop.
//   * outbound (GuiMsg): the actor posts output/title/dir/command/exited to the
//                        GUI's mailbox.
//
// Because the only cross-thread traffic is owned message VALUES over the
// race-free Mailbox, there is no shared mutable state and no lock on the grid —
// the actor OWNS its Terminal; the GUI never touches it. Rendering the active
// tab is done by the GUI asking the actor (via a TabCmd) to render into the
// frame — see the runtime; the actor performs the toe render on... no: the GL
// context is single-threaded, so RENDER happens on the GUI thread against a
// grid snapshot the actor publishes. To keep zero shared state, the actor sends
// a serialized grid snapshot? Too heavy per frame.
//
// Resolution (documented so the model stays honest): the ACTOR owns parse+PTY;
// the GUI owns GL. The single shared object per tab is that tab's Screen, read
// by the GUI at render and written by the actor at parse. We guard THAT one
// object with the tab's own std::mutex (render_lock) — held only for the
// microseconds of a pump or a render, and never across tabs, so contention is
// nil. This is the minimal, honest synchronization the single-GL-context
// constraint forces; everything else is pure message passing.

#ifndef HAND_ACTOR_TAB_ACTOR_HPP
#define HAND_ACTOR_TAB_ACTOR_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "hand/actor/mailbox.hpp"
#include "hand/gui/message.hpp"
#include "toe/terminal.hpp"

namespace hand {

// Inbound commands the GUI sends to a tab actor.
struct TabInput { std::string bytes; };          // deliver text bytes to the child
struct TabKey { toe::KeyEvent key; };            // deliver a key (Session encodes it)
struct TabStop {};                                // shut the actor down + join
using TabCmd = std::variant<TabInput, TabKey, TabStop>;
// NOTE: resize is intentionally NOT an actor command — Session::resize touches
// the (GL-adjacent) renderer, so it runs ONLY on the GUI thread (in present),
// keeping the invariant that actor threads never touch the renderer.

// The actor. Constructed with an already-created Terminal (moved in) + the GUI's
// outbound mailbox + this tab's stable id. start() spawns the thread; stop()
// (or a TabStop cmd) ends it. The GUI holds `inbox` to post TabCmds and the
// render_lock to read the grid on the GUI thread.
class TabActor {
public:
    TabActor(TabId id, toe::Terminal term, Mailbox<GuiMsg> &to_gui)
        : id_(id), term_(std::move(term)), to_gui_(to_gui) {}

    ~TabActor() { stop(); }
    TabActor(const TabActor &) = delete;
    TabActor &operator=(const TabActor &) = delete;

    // Post an inbound command (from the GUI thread).
    void send(TabCmd c) { inbox_.post(std::move(c)); }

    // The tab's stable id.
    [[nodiscard]] TabId id() const noexcept { return id_; }

    // The GUI holds this while rendering the tab's grid; the actor holds it
    // while mutating the grid (pump). Per-tab, so cross-tab contention is nil.
    [[nodiscard]] std::mutex &render_lock() noexcept { return render_lock_; }

    // Access the Terminal for rendering ONLY on the GUI thread, ONLY under
    // render_lock(). (The actor thread uses term_ under the same lock.)
    [[nodiscard]] toe::Terminal &terminal() noexcept { return term_; }

    void start() {
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        if (!thread_.joinable()) return;
        stopping_.store(true);
        inbox_.post(TabStop{});
        thread_.join();
    }

private:
    void run();  // in tab_actor.cpp — the blocking loop
    void publish_status(bool post_output); // post CHANGE-ONLY status GuiMsgs (under render_lock_)
    // The live grid generation, read under render_lock_ (0 if no running term).
    // Lets run() detect a coalesced-but-undelivered change so it never drops it.
    std::uint64_t grid_generation();

    TabId id_;
    toe::Terminal term_;
    Mailbox<GuiMsg> &to_gui_;
    Mailbox<TabCmd> inbox_;
    std::mutex render_lock_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;

    // Last-published status, so we only post GuiMsgs on CHANGE (no spam).
    std::uint64_t last_gen_ = 0;
    std::string last_title_;
    std::string last_cwd_;
    std::string last_cmd_;
    bool last_running_ = false;
    bool last_finished_ = false;
    std::int64_t last_notify_ms_ = 0; // last GUI output-notification time (rate limit)
    bool pending_output_ = false;     // a grid change was coalesced, not yet posted
};

} // namespace hand

#endif // HAND_ACTOR_TAB_ACTOR_HPP
