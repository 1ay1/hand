#pragma once
// hand::msg — everything that can happen, as a closed tree of sums.
//
//   Msg ─┬─ ShellMsg     Output Exited                          a pty spoke
//        ├─ TabMsg       NewTab CloseTab SelectTab StepTab MoveTab
//        ├─ ClockMsg     Blink Tick Clock                       time passed
//        ├─ TextMsg      Copy PasteReq Pasted CopyOutput CopyKey OpenUrl HintPick
//        ├─ SearchMsg    SearchToggle SearchStep
//        ├─ DialogMsg    Answer RememberToggle                  the question
//        ├─ WindowMsg    RequestQuit WindowAct Panel ToggleSettings
//        ├─ SshMsg       OpenHosts HostsKey ConnectHost ConnectTyped Reconnect
//        ├─ ManagerMsg   ToggleManager Manager FilesListed FilesDone UploadPicked TunnelProbe TunnelScan
//        ├─ PaneMsg      TogglePane PaneOp PaneListed PaneResize PaneWatch PanePreview
//        └─ Edit         a direct model edit (input gestures with no domain of their own)
//
// Each leaf names an intent and carries typed data -- ids are SessionId /
// TabIndex, directions are Step, "already asked" is Confirm, never a bare int
// or bool. jaal routes the tree to update(Model&, Leaf) and refuses to compile
// a leaf nobody handles. A leaf converts implicitly into Msg (the domain
// accepting it is unique), so `return NewTab{};` works everywhere.

#include <hand/app/model.hpp>
#include <hand/core/types.hpp>

#include <mayag/mayag.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hand::msg {

// ── the shells ─────────────────────────────────────────────────────────────
struct Output { std::string bytes; SessionId sid = kNoSession; };   ///< a pty printed something
struct Exited { int code = 0; SessionId sid = kNoSession; };        ///< a pty's program ended
using ShellMsg = std::variant<Output, Exited>;

// ── tabs ───────────────────────────────────────────────────────────────────
struct NewTab {};
struct CloseTab { TabIndex tab; Confirm confirm = Confirm::asking; };
struct SelectTab { TabIndex tab; };
struct StepTab { Step step; };                                     ///< focus the neighbour
struct MoveTab { Step step; };                                     ///< reorder the active tab
using TabMsg = std::variant<NewTab, CloseTab, SelectTab, StepTab, MoveTab>;

// ── clocks ─────────────────────────────────────────────────────────────────
struct Blink {};
struct Tick { double dt = 0.0; };                                  ///< a frame: seconds since the last
struct Clock {};                                                   ///< 1 Hz: live timers, activity
using ClockMsg = std::variant<Blink, Tick, Clock>;

// ── text in and out ────────────────────────────────────────────────────────
/// How a hint is used: copied / opened, or typed at the prompt.
enum class HintUse : std::uint8_t { act, insert };
struct Copy {};
struct PasteReq {};
struct Pasted { std::string text; Confirm confirm = Confirm::asking; };
struct CopyOutput {};                                              ///< the last command's output to the clipboard
struct CopyKey { mayag::Key key; mayag::Mods mods; std::string text; };   ///< a key in copy mode
struct OpenUrl { std::string url; };
struct HintPick { std::size_t hint = 0; HintUse use = HintUse::act; };
using TextMsg = std::variant<Copy, PasteReq, Pasted, CopyOutput, CopyKey, OpenUrl, HintPick>;

// ── search ─────────────────────────────────────────────────────────────────
struct SearchToggle {};                                            ///< open (seeded with the selection) / close
struct SearchStep { Step step; };                                  ///< previous / next match
using SearchMsg = std::variant<SearchToggle, SearchStep>;

// ── the dialog ─────────────────────────────────────────────────────────────
/// An answer to the open question. A choice is an index into the dialog's
/// actions (0 is always "cancel"); dismissing is its own case, not "-1".
struct Answer {
    struct Dismiss {};
    struct Choose { std::size_t action = 0; };
    std::variant<Dismiss, Choose> pick;
    [[nodiscard]] static Answer dismiss() { return {Dismiss{}}; }
    [[nodiscard]] static Answer choose(std::size_t i) { return {Choose{i}}; }
};
struct RememberToggle {};                                          ///< the "don't ask again" box
using DialogMsg = std::variant<Answer, RememberToggle>;

// ── the window and its settings ────────────────────────────────────────────
struct RequestQuit {};                                             ///< the window's close button
struct WindowAct { mayag::WindowAction::Kind kind; };             ///< move / minimise / maximise (no title bar)
struct Panel { mayag::dsl::SettingsOp op; };                      ///< an interaction in the settings panel
struct ToggleSettings {};                                          ///< ctrl+,
using WindowMsg = std::variant<RequestQuit, WindowAct, Panel, ToggleSettings>;

// ── ssh: the picker and connecting ─────────────────────────────────────────
/// Where a connection opens.
enum class Where : std::uint8_t { new_tab, this_tab };
struct OpenHosts {};                                               ///< ctrl+shift+s (again: close)
struct HostsKey { mayag::Key key; mayag::Mods mods; };
struct ConnectHost { Host host; Where where = Where::new_tab; };
struct ConnectTyped {};                                            ///< the picker's query as a destination
struct Reconnect {};                                               ///< the dropped ssh tab, again
using SshMsg = std::variant<OpenHosts, HostsKey, ConnectHost, ConnectTyped, Reconnect>;

// ── ssh: the host manager ──────────────────────────────────────────────────
struct ToggleManager {};                                           ///< ctrl+shift+h
struct Manager { mayag::dsl::SshManagerOp op; };                  ///< an interaction in it
/// A remote listing came back (a task ran sftp).
struct FilesListed { std::string alias; std::string path; std::vector<mayag::dsl::SshRemoteEntry> entries; std::string error; };
/// A transfer finished: refresh `refresh_path` on `alias` if it is shown.
struct FilesDone { std::string alias; std::string refresh_path; std::string title; std::string error; std::string reveal; };
/// Files picked for upload (empty: cancelled).
struct UploadPicked { std::string alias; std::string dir; std::vector<std::string> files; };
struct TunnelProbe {};                                             ///< 1 Hz while the manager is open
struct TunnelScan { std::vector<int> ports; std::vector<std::string> owners; bool known = true; };
using ManagerMsg = std::variant<ToggleManager, Manager, FilesListed, FilesDone, UploadPicked, TunnelProbe, TunnelScan>;

// ── the file pane ──────────────────────────────────────────────────────────
/// Whether a listing should announce itself (a watch re-read is quiet).
enum class Notice : std::uint8_t { loud, quiet };
struct TogglePane {};                                              ///< ctrl+shift+e
struct PaneOp { mayag::dsl::FilePaneOp op; };
struct PaneListed { std::uint64_t token = 0; std::string path; std::vector<mayag::dsl::FileEntry> entries; std::string error; Notice notice = Notice::loud; };
struct PaneResize { float dx = 0.0f; };                            ///< grow by dx px
struct PaneWatch {};                                               ///< re-read the pane's folder (files.watch)
struct PanePreview { std::string path; std::optional<mayag::dsl::FilePreview> preview; };
using PaneMsg = std::variant<TogglePane, PaneOp, PaneListed, PaneResize, PaneWatch, PanePreview>;

// ── the root ───────────────────────────────────────────────────────────────
using Edit = mayag::Edit<Model>;
using AnyBase = std::variant<ShellMsg, TabMsg, ClockMsg, TextMsg, SearchMsg, DialogMsg, WindowMsg, SshMsg, ManagerMsg,
                             PaneMsg, Edit>;
/// The message. Derived (not an alias) so it is one nominal type in errors and
/// forward declarations. A leaf converts straight in -- `Any m = NewTab{}` --
/// because std::variant's converting constructor finds the one domain that
/// holds a NewTab; a leaf in two domains would be ambiguous and not compile.
struct Any : AnyBase {
    using AnyBase::AnyBase;
    using AnyBase::operator=;
};

}  // namespace hand::msg
