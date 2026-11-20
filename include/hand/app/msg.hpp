#pragma once
// hand::app messages — everything that can happen, as one closed sum.
//
// Each alternative names an intent ("close tab 2", "the shell in session 7
// printed this"); update(Model&, X) is the one place each is handled.

#include <hand/app/model.hpp>

#include <mayag/mayag.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace hand::msg {

// the shells
struct Output { std::string bytes; std::uint64_t sid = 0; };   ///< a pty printed something
struct Exited { int code; std::uint64_t sid = 0; };            ///< a pty's program ended
// tabs
struct NewTab {};
struct CloseTab { std::size_t i; bool confirmed = false; };   ///< confirmed: no question asked
struct SelectTab { std::size_t i; };
struct StepTab { int d; };
struct MoveTab { int d; };                                    ///< reorder the active tab
// clocks
struct Blink {};
struct Tick { double dt; };
struct Clock {};                                              ///< 1 Hz: live timers, activity
// text in and out
struct Copy {};
struct PasteReq {};
struct Pasted { std::string text; bool confirmed = false; };  ///< confirmed: the dialog already said yes
struct CopyOutput {};                                         ///< the last command's output to the clipboard
struct CopyKey { mayag::Key key; mayag::Mods mods; std::string text; };   ///< a key in copy mode
struct Search { int op; };                                    ///< 0 open/close, 1 next, -1 prev
struct OpenUrl { std::string url; };
struct HintPick { std::size_t i; bool insert; };              ///< act on hint i (insert: type it at the prompt)
// settings
struct Panel { mayag::dsl::SettingsOp op; };
struct ToggleSettings {};
// the dialog, the window
struct Answer { int action; };                                ///< run action i (-1 cancel, 100 toggle "remember")
struct RequestQuit {};                                        ///< the window's close button
struct WindowAct { mayag::WindowAction::Kind kind; };        ///< move / minimise / maximise (no system title bar)
// ssh
struct OpenHosts {};                                          ///< ctrl+shift+s (again: close)
struct HostsKey { mayag::Key key; mayag::Mods mods; };
struct ConnectHost { Host host; bool here = false; };         ///< here: in this tab (else a new one)
struct ConnectTyped {};                                       ///< the picker's query as a destination
struct Reconnect {};                                          ///< the dropped ssh tab, again
struct ToggleManager {};                                      ///< ctrl+shift+h: the host manager
struct Manager { mayag::dsl::SshManagerOp op; };              ///< an interaction in it
/// A remote listing came back (a task ran sftp).
struct FilesListed { std::string alias; std::string path; std::vector<mayag::dsl::SshRemoteEntry> entries; std::string error; };
/// A transfer finished: refresh `refresh_path` on `alias` if it is shown.
struct FilesDone { std::string alias; std::string refresh_path; std::string title; std::string error; std::string reveal; };
/// Files picked for upload (empty: cancelled).
struct UploadPicked { std::string alias; std::string dir; std::vector<std::string> files; };
struct TunnelProbe {};                                        ///< 1 Hz while the manager is open
struct TunnelScan { std::vector<int> ports; std::vector<std::string> owners; bool known = true; };
// the file pane beside the terminal
struct TogglePane {};                                          ///< ctrl+shift+e
struct PaneOp { mayag::dsl::FilePaneOp op; };
struct PaneListed { std::uint64_t token = 0; std::string path; std::vector<mayag::dsl::FileEntry> entries; std::string error; bool quiet = false; };
struct PaneResize { float dx = 0.0f; };                         ///< grow by dx px
struct PaneWatch {};                                           ///< re-read the pane's folder (files.watch)
struct PanePreview { std::string path; std::optional<mayag::dsl::FilePreview> preview; };

using Any = std::variant<Output, Exited, NewTab, CloseTab, SelectTab, StepTab, MoveTab, Blink, Tick, Clock,
                         Copy, PasteReq, Pasted, CopyOutput, CopyKey, Search, OpenUrl, HintPick, Panel, ToggleSettings,
                         Answer, RequestQuit, WindowAct, OpenHosts, HostsKey, ConnectHost, ConnectTyped, Reconnect,
                         ToggleManager, Manager, FilesListed, FilesDone, UploadPicked, TunnelProbe, TunnelScan,
                         TogglePane, PaneOp, PaneListed, PaneResize, PaneWatch, PanePreview, mayag::Edit<Model>>;

}  // namespace hand::msg
