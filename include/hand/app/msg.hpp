#pragma once
// hand::msg — everything that can happen, as a closed tree of sums.
//
//   Msg ─┬─ ShellMsg     Output Exited Spawned Probe Probed    a pty spoke
//        ├─ TabMsg       NewTab CloseTab SelectTab StepTab MoveTab
//        ├─ ClockMsg     Blink Tick Clock                       time passed
//        ├─ TextMsg      Copy PasteReq Pasted CopyOutput CopyKey OpenUrl HintPick
//        ├─ SearchMsg    SearchToggle SearchStep SearchKey SearchText
//        ├─ ScrollMsg    ScrollBy ScrollLive ScrollTop ScrollToPrompt ScrollToLine JumpPrompt ClearScrollback
//        ├─ SelectMsg    SelectBegin SelectExtend SelectOutput SelectEnd SelectClear
//        ├─ PointerMsg   ReportPress ReportMotion ReportRelease HoverLink MinimapHover PointerLeft
//        ├─ KeyboardMsg  Typed Swallow ModifierDown FontZoom Compose ComposeEnd Focused Resized
//        ├─ ModeMsg      ToggleCopyMode OpenHints CloseHints HintBackspace HintTyped PickerText PickerClose PaneBlur
//        ├─ DialogMsg    Answer RememberToggle DialogFocus      the question
//        ├─ WindowMsg    RequestQuit WindowAct Panel ToggleSettings
//        ├─ SshMsg       OpenHosts HostsKey ConnectHost ConnectTyped Reconnect HostsLoaded
//        ├─ ManagerMsg   ToggleManager Manager FilesListed FilesDone UploadPicked TunnelProbe TunnelScan
//        ├─ PaneMsg      TogglePane PaneOp PaneListed PaneResize PaneWatch PanePreview
//        ├─ EditorMsg    OpenFile FileLoaded EditorOp FileSaved EditorPasted FileWatch FileStat
//        │               QuickOpen QuickListed QuickKey QuickText QuickPick     files in tabs, ⌘P
//        │               SearchAll SearchAllKey SearchAllText SearchAllDone SearchAllPick   ⌘⇧F
//        └─ Edit         a direct model edit (self-test and shots only)
//
// Each leaf names an intent and carries typed data -- ids are SessionId /
// TabIndex, directions are Step, "already asked" is Confirm, never a bare int
// or bool. jaal routes the tree to update(Model&, Leaf) and refuses to compile
// a leaf nobody handles. A leaf converts implicitly into Msg (the domain
// accepting it is unique), so `return NewTab{};` works everywhere.
//
// Every message crosses threads (a pty reader, a task, the loop), so the
// whole tree is checked Sendable at the bottom of this file.

#include <hand/app/model.hpp>
#include <hand/core/credit.hpp>
#include <hand/core/types.hpp>

#include <mayag/mayag.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hand::msg {

// ── the shells ─────────────────────────────────────────────────────────────
/// A pty printed something. `credit` is the reader's flow control: the UI
/// gives it back once the bytes are fed, and only then does the next chunk come.
struct Output { std::string bytes; SessionId sid = kNoSession; std::optional<mayag::CoOwned<OutputCredit>> credit; };
struct Exited { int code = 0; SessionId sid = kNoSession; };        ///< a pty's program ended
/// A spawn effect finished: the session's pty (empty: it did not start), and
/// whether its shell rewraps its own prompt (fish with hand's integration).
struct Spawned { SessionId sid = kNoSession; std::optional<mayag::CoOwned<Pty>> pty; bool reflow_aware = false; };
/// What runs in each tab now (the probe effect read it off the ptys).
struct TabProc { SessionId sid = kNoSession; std::string proc; std::optional<Host> remote; };
struct Probe {};                                                   ///< ask the ptys what runs in them
struct Probed { std::vector<TabProc> tabs; };                      ///< ...and they answered (only on a change)
using ShellMsg = std::variant<Output, Exited, Spawned, Probe, Probed>;

// ── tabs ───────────────────────────────────────────────────────────────────
struct NewTab {};
struct CloseTab { TabIndex tab; Confirm confirm = Confirm::asking; };
struct SelectTab { TabIndex tab; };
struct StepTab { Step step; };                                     ///< focus the neighbour
struct MoveTab { Step step; };                                     ///< reorder the active tab
struct TabPress { TabIndex tab; };                                 ///< pressed on the strip: select it, start a drag
struct TabDrag { mayag::DragTravel travel; };                     ///< the pressed tab, dragged this far since the press
struct TabRelease {};                                              ///< the drag ends
using TabMsg = std::variant<NewTab, CloseTab, SelectTab, StepTab, MoveTab, TabPress, TabDrag, TabRelease>;

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
struct SearchKey { mayag::Key key; mayag::Mods mods; };            ///< editing the query
struct SearchText { std::string text; };                           ///< typed into the query
using SearchMsg = std::variant<SearchToggle, SearchStep, SearchKey, SearchText>;

// ── the scrollback ─────────────────────────────────────────────────────────
/// Where a jump lands.
struct ScrollBy { float rows = 0.0f; };                            ///< + = into history (fractional: trackpads)
struct ScrollLive {};                                              ///< back to the live screen
struct ScrollTop {};                                               ///< the oldest line kept
struct ScrollToPrompt { std::int64_t prompt_row = 0; };            ///< a command's prompt at the top
struct ScrollToLine { std::int64_t line = 0; };                    ///< a history line (0 = oldest) at the top
struct JumpPrompt { Step step; };                                  ///< the previous / next prompt
struct ClearScrollback {};                                         ///< screen and history; the shell redraws
using ScrollMsg = std::variant<ScrollBy, ScrollLive, ScrollTop, ScrollToPrompt, ScrollToLine, JumpPrompt, ClearScrollback>;

// ── the selection (the mouse, with reporting off) ──────────────────────────
/// What a press selects: from a cell (clicks: 1 chars, 2 a word, 3 a line).
enum class SelectShape : std::uint8_t { linear, block };
/// How a selection ends: kept (and maybe copied), or dropped as a mere click.
enum class SelectEnding : std::uint8_t { keep, discard };
struct SelectBegin { int col = 0, row = 0, clicks = 1; mayag::Vec2 at; SelectShape shape = SelectShape::linear; };
/// Extend the selection to a cell. `shift_click` starts a drag from the old
/// selection; otherwise it is a drag in progress (edge: rows/s past an edge).
enum class Extend : std::uint8_t { drag, shift_click };
struct SelectExtend { int col = 0, row = 0; mayag::Vec2 at; float edge = 0.0f; Extend how = Extend::drag; };
struct SelectOutput { std::int64_t first = 0, last = 0; };          ///< a command's whole output, as lines
struct SelectEnd { SelectEnding ending = SelectEnding::keep; };
struct SelectClear {};
using SelectMsg = std::variant<SelectBegin, SelectExtend, SelectOutput, SelectEnd, SelectClear>;

// ── the pointer: reported to the app, hovering, the minimap ────────────────
/// A press / motion / release already encoded for the pty (xterm mouse
/// reporting). `button` is the xterm button code the press holds.
struct ReportPress { int button = 0; std::string bytes; };
struct ReportMotion { int col = 0, row = 0; std::string bytes; };
struct ReportRelease { std::string bytes; };
struct HoverLink { int col = 0, row = 0; bool armed = false; std::string url; };   ///< url empty: none under the pointer
struct MinimapHover { bool open = false; std::optional<float> y; };             ///< y: the pointer over it
struct PointerLeft {};                                             ///< left the terminal: hovers end
using PointerMsg = std::variant<ReportPress, ReportMotion, ReportRelease, HoverLink, MinimapHover, PointerLeft>;

// ── the keyboard: bytes to the shell, held modifiers, composition, focus ───
enum class Modifier : std::uint8_t { ctrl, alt };
enum class Zoom : std::uint8_t { in, out, reset };
struct Typed { std::string bytes; };                               ///< to the active pty (snaps the view live)
struct Swallow {};                                                 ///< taken, nothing to do (never reaches the shell)
struct ModifierDown { Modifier mod; };
struct FontZoom { Zoom zoom; bool ui = false; };   ///< ctrl +/-/0: the terminal; with shift: the interface
struct Compose { std::string preedit; };                           ///< the input method's text so far
struct ComposeEnd {};
struct Focused { bool focused = false; std::string report; };      ///< report: the focus event for the app (mode 1004)
struct Resized { mayag::Vec2 size; };
using KeyboardMsg = std::variant<Typed, Swallow, ModifierDown, FontZoom, Compose, ComposeEnd, Focused, Resized>;

// ── modes over the terminal: copy mode, hints, and who has the keyboard ────
struct ToggleCopyMode {};                                          ///< ctrl+shift+m
struct OpenHints {};                                               ///< ctrl+shift+space
struct CloseHints {};
struct HintBackspace {};
struct HintTyped { std::string typed; };                           ///< the label so far (no match yet)
struct PickerText { std::string text; };                           ///< typed into the host picker
struct PickerClose {};
struct PaneBlur {};                                                ///< the terminal takes the keyboard back
using ModeMsg = std::variant<ToggleCopyMode, OpenHints, CloseHints, HintBackspace, HintTyped, PickerText, PickerClose, PaneBlur>;

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
struct DialogFocus { Step step; };                                 ///< tab / arrows between the actions
using DialogMsg = std::variant<Answer, RememberToggle, DialogFocus>;

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
/// A dropped ssh tab, again (kNoSession: the front tab -- enter, the button).
struct Reconnect { SessionId sid = kNoSession; };
/// 1 Hz while a dropped tab waits to reconnect: are any due?
struct RetryTick {};
/// The host book came back from disk (~/.ssh/config, known_hosts, hosts.vibe;
/// PuTTY and MobaXterm when the manager asked).
struct HostsLoaded { HostBook book; std::optional<std::vector<ImportSource>> imports; };
using SshMsg = std::variant<OpenHosts, HostsKey, ConnectHost, ConnectTyped, Reconnect, RetryTick, HostsLoaded>;

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
/// The pane's edge, dragged. `begin`: pressed (the width now is the base);
/// then `grow`: the pointer's travel since the press (px wider than the base).
struct PaneResize { bool begin = false; float grow = 0.0f; bool end = false; };
struct PaneWatch {};                                               ///< re-read the pane's folder (files.watch)
struct PanePreview { std::string path; std::optional<mayag::dsl::FilePreview> preview; };
using PaneMsg = std::variant<TogglePane, PaneOp, PaneListed, PaneResize, PaneWatch, PanePreview>;

// ── files in tabs: the editor ──────────────────────────────────────────────
/// Open `path` in an editor tab (or bring its tab to the front); with a
/// `line` (0-based), select [col, col+len) on it and centre it.
/// `host`: the file is on that machine (read and saved over ssh).
struct OpenFile { std::string path; int line = -1; int col = 0; int len = 0; std::optional<Host> host = std::nullopt; };
/// The file's bytes arrived (or why they couldn't).
/// `reload`: a re-read of an open file (it changed on disk, or "reload").
struct FileLoaded { SessionId sid; std::string path; std::string text; std::string error; bool binary = false;
                    std::int64_t mtime = 0; bool reload = false; };
/// An interaction in the editor of the active tab.
struct EditorOp { mayag::dsl::TextEditorOp op; };
/// A save of `point` finished; `error` empty when the bytes are on disk.
struct FileSaved { SessionId sid; std::uint64_t point = 0; std::string error; std::int64_t mtime = 0; };
/// The clipboard's text, for the editor that asked.
struct EditorPasted { SessionId sid; std::string text; };
/// Look whether the front file changed on disk (1.5 s while one is in front).
struct FileWatch {};
/// What the disk says about a file now (mtime 0: it is gone).
struct FileStat { SessionId sid; std::int64_t mtime = 0; };
/// ⌘P: open (or close) the quick-open list of the project's files.
struct QuickOpen {};
/// The project's files, listed (relative to `root`).
struct QuickListed { std::string root; std::vector<std::string> files; bool truncated = false; };
struct QuickKey { mayag::Key key; mayag::Mods mods; };
struct QuickText { std::string text; };
/// Open the ranked entry `index` (a click, or enter).
struct QuickPick { int index = 0; };
/// ⌘⇧F: search the project's files for text.
struct SearchAll {};
struct SearchAllKey { mayag::Key key; mayag::Mods mods; };
struct SearchAllText { std::string text; };
using hand::SearchHit;
/// A search finished (`token`: which query it answers; older ones are dropped).
struct SearchAllDone { std::uint64_t token = 0; std::string root; std::vector<SearchHit> hits; bool truncated = false; std::string error; };
struct SearchAllPick { int index = 0; };
using EditorMsg = std::variant<OpenFile, FileLoaded, EditorOp, FileSaved, EditorPasted, FileWatch, FileStat,
                               QuickOpen, QuickListed, QuickKey, QuickText, QuickPick,
                               SearchAll, SearchAllKey, SearchAllText, SearchAllDone, SearchAllPick>;

// ── the root ───────────────────────────────────────────────────────────────
/// A direct model edit. For the self-test and scripted shots only: the app
/// itself names every change (tests/layering.cmake rejects E{...} in src/app
/// and src/view).
using Edit = mayag::Edit<Model>;
using AnyBase = std::variant<ShellMsg, TabMsg, ClockMsg, TextMsg, SearchMsg, ScrollMsg, SelectMsg, PointerMsg, KeyboardMsg,
                             ModeMsg, DialogMsg, WindowMsg, SshMsg, ManagerMsg, PaneMsg, EditorMsg, Edit>;
/// The message. Derived (not an alias) so it is one nominal type in errors and
/// forward declarations. A leaf converts straight in -- `Any m = NewTab{}` --
/// because std::variant's converting constructor finds the one domain that
/// holds a NewTab; a leaf in two domains would be ambiguous and not compile.
struct Any : AnyBase {
    using AnyBase::AnyBase;
    using AnyBase::operator=;
};

}  // namespace hand::msg

// The proof, at compile time: every message may move between threads (it
// holds nothing the sender can still reach), except Edit, which the Sink keeps
// on the loop (jaal::loop_only). A borrowed pointer, a view or a
// shared_ptr in any leaf fails the build here, naming the culprit.
static_assert(mayag::Postable<hand::msg::Any>, "a hand message holds something borrowed");
// ...and every leaf except the loop-only Edit really is Sendable: a message
// that reaches update() from a reader or a task carries only owned values.
static_assert(!mayag::Sendable<hand::msg::Edit>, "Edit is loop-only: it must never cross threads");
// App::update(Model&, Msg, now) is the seam: it stamps the time, dispatches
// the leaf and drains the ptys' requests -- so jaal hands it the whole Msg.
MAYAG_HANDLED_AS_GROUP(hand::msg::Any);
