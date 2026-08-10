// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/message.hpp — the GUI's Elm message algebra.
//
// The GUI thread is itself an Elm program one level ABOVE toe: its Model is the
// tab workspace, its inputs are GuiMsgs (a closed sum arriving from the window
// AND from the per-tab actor threads over the Mailbox), and its effects are
// GuiCmds (a closed sum the impure runtime interprets — spawn a tab thread,
// send bytes to a tab, present a frame, ...).
//
//     gui_update : (GuiModel, GuiMsg) -> (GuiModel, [GuiCmd])   // PURE, total
//
// Because both directions are closed variants and update is pure, the GUI is
// exhaustively testable with zero windowing/threads, and a new message or
// effect is a COMPILE error until every site handles it. Data from actor
// threads only ever enters as an immutable GuiMsg value — never shared memory.

#ifndef HAND_GUI_MESSAGE_HPP
#define HAND_GUI_MESSAGE_HPP

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace hand {

// A stable per-tab identity assigned by the GUI when it spawns a tab actor.
// Never reused, so a late message from a closed tab is harmlessly ignored.
enum class TabId : std::uint64_t {};

// ===========================================================================
// GuiMsg — everything that can happen TO the GUI. Closed sum.
// ===========================================================================

// --- from the per-tab actor threads (via the Mailbox) ----------------------
struct TabOutput {                 // a tab's grid advanced (new child output)
    TabId id;
    std::uint64_t generation;      // toe Session::generation() snapshot
};
struct TabTitleChanged { TabId id; std::string title; };
struct TabDirChanged   { TabId id; std::string cwd; };
struct TabCommand {                // OSC 133 status change (for the Activity tab)
    TabId id;
    bool running;
    std::string cmd;
    bool have_exit;
    int exit_code;
};
struct TabExited { TabId id; int code; };

// --- from the window / user -------------------------------------------------
struct WinResized  { int w, h; };
struct WinFocus    { bool focused; };
struct WinCloseReq {};
struct Tick        { std::uint64_t frame; }; // ~per-frame, drives spinner/pulse

// Tab-management intents (produced by keybind/chrome-click translation).
struct NewTab      {};
struct CloseTab    {};             // close the focused tab
struct NextTab     {};
struct PrevTab     {};
struct FocusTabAt  { std::size_t index; };

// Input destined for the focused terminal (opaque bytes / a key the GUI didn't
// consume). The runtime forwards these to the focused tab actor.
struct ForwardBytes { std::string bytes; };

using GuiMsg =
    std::variant<TabOutput, TabTitleChanged, TabDirChanged, TabCommand, TabExited, WinResized,
                 WinFocus, WinCloseReq, Tick, NewTab, CloseTab, NextTab, PrevTab, FocusTabAt,
                 ForwardBytes>;

// ===========================================================================
// GuiCmd — effects as DATA. The impure runtime interprets these. Closed sum.
// ===========================================================================

struct SpawnTab   { std::string cwd; };      // start a new tab actor (in cwd)
struct KillTab    { TabId id; };             // stop a tab actor + join its thread
struct SendToTab  { TabId id; std::string bytes; }; // deliver input to a tab
struct Present     {};                        // draw a frame (active tab + chrome)
struct SetWindowTitle { std::string title; }; // reflect focused tab's title
struct Quit        {};                        // last tab closed / window closed

using GuiCmd = std::variant<SpawnTab, KillTab, SendToTab, Present, SetWindowTitle, Quit>;

using GuiCmds = std::vector<GuiCmd>;

} // namespace hand

#endif // HAND_GUI_MESSAGE_HPP
