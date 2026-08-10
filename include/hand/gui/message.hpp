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
#include <functional>
#include <optional>
#include <variant>
#include <vector>

#include "toe/input.hpp" // toe::KeyEvent

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

// Input destined for the focused terminal. Text is committed UTF-8; a key is a
// full KeyEvent the focused tab's Session encodes (send_key does the VT/kitty
// encoding, app-cursor mode, etc. — so the GUI stays encoding-agnostic).
struct ForwardText { std::string utf8; };
struct ForwardKey  { toe::KeyEvent key; };

using GuiMsg =
    std::variant<TabOutput, TabTitleChanged, TabDirChanged, TabCommand, TabExited, WinResized,
                 WinFocus, WinCloseReq, Tick, NewTab, CloseTab, NextTab, PrevTab, FocusTabAt,
                 ForwardText, ForwardKey>;

// ===========================================================================
// GuiCmd — effects as DATA. The impure runtime interprets these. Closed sum.
// ===========================================================================

struct SpawnTab   { std::string cwd; };      // start a new tab actor (in cwd)
struct KillTab    { TabId id; };             // stop a tab actor + join its thread
struct SendToTab  { TabId id; std::string bytes; }; // deliver text input to a tab
struct SendKeyToTab { TabId id; toe::KeyEvent key; }; // deliver a key to a tab
struct Present     {};                        // draw a frame (active tab + chrome)
struct SetWindowTitle { std::string title; }; // reflect focused tab's title
struct Quit        {};                        // last tab closed / window closed

using GuiCmd = std::variant<SpawnTab, KillTab, SendToTab, SendKeyToTab, Present, SetWindowTitle,
                            Quit>;

using GuiCmds = std::vector<GuiCmd>;

} // namespace hand

// TabId is an enum-class key for the runtime's actor map; give it a hash.
template <>
struct std::hash<hand::TabId> {
    std::size_t operator()(hand::TabId id) const noexcept {
        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(id));
    }
};

#endif // HAND_GUI_MESSAGE_HPP
