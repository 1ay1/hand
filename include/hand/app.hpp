#pragma once
// hand::App — a foot-like terminal: one window, a shell per tab.
//
//   pty ──stream──▶ msg::Output ──▶ Term::feed (toe) ──▶ view: the terminal widget tree
//   keys/text/mouse ──▶ Term::encode_* ──▶ pty
//   ctrl+,  ──▶ the settings panel over the terminal; every edit is live
//   ctrl+shift+s ──▶ the SSH host picker; a host opens as a tab
//
// The program, in jaal's shape: init / update / on / subscribe / view. hand
// owns state and behaviour; every pixel is a mayag widget (src/view only
// composes them).
//
//   include/hand/app/model.hpp   the state      src/app/*.cpp   update + on, by concern
//   include/hand/app/msg.hpp     what happens   src/view/*.cpp  the widget tree

#include <hand/app/model.hpp>
#include <hand/app/msg.hpp>

#include <mayag/mayag.hpp>

#include <optional>
#include <vector>

namespace hand {

struct App {
    using Session = hand::Session;
    using Model = hand::Model;
    using Msg = msg::Any;
    /// `run`: an effect on the loop thread after the step (a pty write, a
    /// save); `task` (core): one that may block, on the pool.
    using Cmd = mayag::Cmd<Msg, mayag::set_title, mayag::write_clipboard, mayag::read_clipboard, mayag::window_action,
                           mayag::world>;
    using Sub = mayag::Sub<Msg>;
    /// update is handed the step's time (jaal reads the clock, never hand).
    using Clock = mayag::SteadyClock;
    using Time = Clock::time_point;
    // every leaf message under its own name (App::NewTab{} reads well at call sites)
    using Output = msg::Output;         using Exited = msg::Exited;
    using NewTab = msg::NewTab;         using CloseTab = msg::CloseTab;     using SelectTab = msg::SelectTab;
    using StepTab = msg::StepTab;       using MoveTab = msg::MoveTab;
    using Blink = msg::Blink;           using Tick = msg::Tick;             using Second = msg::Clock;
    using Copy = msg::Copy;             using PasteReq = msg::PasteReq;     using Pasted = msg::Pasted;
    using CopyOutput = msg::CopyOutput; using CopyKey = msg::CopyKey;       using OpenUrl = msg::OpenUrl;
    using HintPick = msg::HintPick;     using HintUse = msg::HintUse;
    using SearchToggle = msg::SearchToggle; using SearchStep = msg::SearchStep;
    using SearchKey = msg::SearchKey;   using SearchText = msg::SearchText;
    using ScrollBy = msg::ScrollBy;     using ScrollLive = msg::ScrollLive; using ScrollTop = msg::ScrollTop;
    using ScrollToPrompt = msg::ScrollToPrompt; using ScrollToLine = msg::ScrollToLine;
    using JumpPrompt = msg::JumpPrompt; using ClearScrollback = msg::ClearScrollback;
    using SelectBegin = msg::SelectBegin; using SelectExtend = msg::SelectExtend; using SelectOutput = msg::SelectOutput;
    using SelectEnd = msg::SelectEnd;   using SelectClear = msg::SelectClear;
    using SelectShape = msg::SelectShape; using SelectEnding = msg::SelectEnding; using Extend = msg::Extend;
    using ReportPress = msg::ReportPress; using ReportMotion = msg::ReportMotion; using ReportRelease = msg::ReportRelease;
    using HoverLink = msg::HoverLink;   using MinimapHover = msg::MinimapHover; using PointerLeft = msg::PointerLeft;
    using Typed = msg::Typed;           using Swallow = msg::Swallow;       using ModifierDown = msg::ModifierDown;
    using FontZoom = msg::FontZoom;     using Compose = msg::Compose;       using ComposeEnd = msg::ComposeEnd;
    using Focused = msg::Focused;       using Resized = msg::Resized;
    using Modifier = msg::Modifier;     using Zoom = msg::Zoom;
    using ToggleCopyMode = msg::ToggleCopyMode; using OpenHints = msg::OpenHints; using CloseHints = msg::CloseHints;
    using HintBackspace = msg::HintBackspace; using HintTyped = msg::HintTyped;
    using PickerText = msg::PickerText; using PickerClose = msg::PickerClose; using PaneBlur = msg::PaneBlur;
    using TabPress = msg::TabPress;     using TabDrag = msg::TabDrag;       using TabRelease = msg::TabRelease;
    using DialogFocus = msg::DialogFocus;
    using Answer = msg::Answer;         using RememberToggle = msg::RememberToggle;
    using RequestQuit = msg::RequestQuit; using WindowAct = msg::WindowAct;
    using Panel = msg::Panel;           using ToggleSettings = msg::ToggleSettings;
    using OpenHosts = msg::OpenHosts;   using HostsKey = msg::HostsKey;     using ConnectHost = msg::ConnectHost;
    using ConnectTyped = msg::ConnectTyped; using Reconnect = msg::Reconnect; using Where = msg::Where;
    using ToggleManager = msg::ToggleManager; using Manager = msg::Manager;
    using FilesListed = msg::FilesListed; using FilesDone = msg::FilesDone; using UploadPicked = msg::UploadPicked;
    using TunnelProbe = msg::TunnelProbe; using TunnelScan = msg::TunnelScan;
    using TogglePane = msg::TogglePane; using PaneOp = msg::PaneOp;         using PaneListed = msg::PaneListed;
    using PaneResize = msg::PaneResize; using PaneWatch = msg::PaneWatch;   using PanePreview = msg::PanePreview;
    using OpenFile = msg::OpenFile;     using FileLoaded = msg::FileLoaded; using EditorOp = msg::EditorOp;
    using FileSaved = msg::FileSaved;   using EditorPasted = msg::EditorPasted;
    using FileWatch = msg::FileWatch;   using FileStat = msg::FileStat;     using QuickOpen = msg::QuickOpen;
    using QuickListed = msg::QuickListed; using QuickKey = msg::QuickKey;   using QuickText = msg::QuickText;
    using QuickPick = msg::QuickPick;
    using SearchAll = msg::SearchAll;   using SearchAllKey = msg::SearchAllKey; using SearchAllText = msg::SearchAllText;
    using SearchAllDone = msg::SearchAllDone; using SearchAllPick = msg::SearchAllPick; using SearchHit = msg::SearchHit;
    using Notice = msg::Notice;
    using Spawned = msg::Spawned;       using Probe = msg::Probe;           using Probed = msg::Probed;
    using HostsLoaded = msg::HostsLoaded;

    /// What a run starts with (Elm's flags): the configuration main loaded
    /// and the fonts mayag loaded, and the world it started in. Data in,
    /// never a global.
    struct Flags {
        Config                                  config;
        std::shared_ptr<const mayag::typo::FontStack> fonts;   ///< null: measure with a nominal 0.6 em cell
        Env                                     env;
    };
    static Cmd init(Model& m, const Flags& f);

    /// THE seam (Msg is handled as one group): stamp the step's time into the
    /// model, dispatch to the leaf's update, then drain what the step asked of
    /// the ptys (input, launches, sizes) into effects. Every handler below is
    /// therefore a pure function of (Model, leaf).
    static Cmd update(Model& m, Msg msg, Time now);
    /// The same, for a caller with no clock (scripted edits in the self-test):
    /// the model's own time stands.
    static Cmd step(Model& m, Msg msg);

    // ── update: one overload per leaf, grouped by domain (jaal checks every leaf is here) ──
    // ShellMsg (src/app/session.cpp)
    static Cmd update(Model& m, Output o);
    static Cmd update(Model& m, Exited e);
    static Cmd update(Model& m, Spawned s);
    static Cmd update(Model& m, Probe);
    static Cmd update(Model& m, Probed p);
    // ClockMsg (src/app/session.cpp)
    static Cmd update(Model& m, Blink);
    static Cmd update(Model& m, Tick t);
    static Cmd update(Model& m, Second);
    // DialogMsg (src/app/dialog.cpp)
    static Cmd update(Model& m, Answer a);
    static Cmd update(Model& m, RememberToggle);
    static Cmd update(Model& m, DialogFocus f);
    // WindowMsg (src/app/dialog.cpp, src/app/settings.cpp)
    static Cmd update(Model& m, RequestQuit);
    static Cmd update(Model&, WindowAct w) { return mayag::WindowAction{.kind = w.kind}; }
    static Cmd update(Model& m, Panel p);
    static Cmd update(Model& m, ToggleSettings);
    // TabMsg (src/app/tabs.cpp)
    static Cmd update(Model& m, NewTab);
    static Cmd update(Model& m, CloseTab c);
    static Cmd update(Model& m, SelectTab s);
    static Cmd update(Model& m, StepTab s);
    static Cmd update(Model& m, MoveTab s);
    static Cmd update(Model& m, TabPress p);
    static Cmd update(Model& m, TabDrag d);
    static Cmd update(Model& m, TabRelease);
    // TextMsg, SearchMsg (src/app/clipboard.cpp)
    static Cmd update(Model& m, Copy);
    static Cmd update(Model& m, PasteReq);
    static Cmd update(Model& m, Pasted p);
    static Cmd update(Model& m, CopyOutput);
    static Cmd update(Model& m, CopyKey k);
    static Cmd update(Model& m, OpenUrl u);
    static Cmd update(Model& m, HintPick h);
    static Cmd update(Model& m, SearchToggle);
    static Cmd update(Model& m, SearchStep s);
    static Cmd update(Model& m, SearchKey k);
    static Cmd update(Model& m, SearchText t);
    // ScrollMsg (src/app/scroll.cpp)
    static Cmd update(Model& m, ScrollBy s);
    static Cmd update(Model& m, ScrollLive);
    static Cmd update(Model& m, ScrollTop);
    static Cmd update(Model& m, ScrollToPrompt s);
    static Cmd update(Model& m, ScrollToLine s);
    static Cmd update(Model& m, JumpPrompt j);
    static Cmd update(Model& m, ClearScrollback);
    // SelectMsg, PointerMsg (src/app/pointer.cpp)
    static Cmd update(Model& m, SelectBegin s);
    static Cmd update(Model& m, SelectExtend s);
    static Cmd update(Model& m, SelectOutput s);
    static Cmd update(Model& m, SelectEnd s);
    static Cmd update(Model& m, SelectClear);
    static Cmd update(Model& m, ReportPress r);
    static Cmd update(Model& m, ReportMotion r);
    static Cmd update(Model& m, ReportRelease r);
    static Cmd update(Model& m, HoverLink h);
    static Cmd update(Model& m, MinimapHover h);
    static Cmd update(Model& m, PointerLeft);
    // KeyboardMsg (src/app/keyboard.cpp)
    static Cmd update(Model& m, Typed t);
    static Cmd update(Model&, Swallow) { return {}; }
    static Cmd update(Model& m, ModifierDown d);
    static Cmd update(Model& m, FontZoom z);
    static Cmd update(Model& m, Compose c);
    static Cmd update(Model& m, ComposeEnd);
    static Cmd update(Model& m, Focused f);
    static Cmd update(Model& m, Resized r);
    // ModeMsg (src/app/modes.cpp)
    static Cmd update(Model& m, ToggleCopyMode);
    static Cmd update(Model& m, OpenHints);
    static Cmd update(Model& m, CloseHints);
    static Cmd update(Model& m, HintBackspace);
    static Cmd update(Model& m, HintTyped h);
    static Cmd update(Model& m, PickerText t);
    static Cmd update(Model& m, PickerClose);
    static Cmd update(Model& m, PaneBlur);
    // SshMsg, ManagerMsg (src/app/ssh.cpp)
    static Cmd update(Model& m, OpenHosts);
    static Cmd update(Model& m, HostsKey k);
    static Cmd update(Model& m, ConnectHost c);
    static Cmd update(Model& m, ConnectTyped);
    static Cmd update(Model& m, Reconnect);
    static Cmd update(Model& m, HostsLoaded h);
    static Cmd update(Model& m, ToggleManager);
    static Cmd update(Model& m, Manager);
    static Cmd update(Model& m, FilesListed);
    static Cmd update(Model& m, FilesDone);
    static Cmd update(Model& m, UploadPicked);
    static Cmd update(Model& m, TunnelProbe);
    static Cmd update(Model& m, TunnelScan);
    // PaneMsg (src/app/pane.cpp)
    static Cmd update(Model& m, TogglePane);
    static Cmd update(Model& m, PaneOp);
    static Cmd update(Model& m, PaneListed);
    static Cmd update(Model& m, PaneResize);
    static Cmd update(Model& m, PaneWatch);
    static Cmd update(Model& m, PanePreview);
    // EditorMsg (src/app/editor.cpp): files open in tabs
    static Cmd update(Model& m, OpenFile);
    static Cmd update(Model& m, FileLoaded);
    static Cmd update(Model& m, EditorOp);
    static Cmd update(Model& m, FileSaved);
    static Cmd update(Model& m, EditorPasted);
    static Cmd update(Model& m, FileWatch);
    static Cmd update(Model& m, FileStat);
    static Cmd update(Model& m, QuickOpen);
    static Cmd update(Model& m, QuickListed);
    static Cmd update(Model& m, QuickKey);
    static Cmd update(Model& m, QuickText);
    static Cmd update(Model& m, QuickPick);
    static Cmd update(Model& m, SearchAll);
    static Cmd update(Model& m, SearchAllKey);
    static Cmd update(Model& m, SearchAllText);
    static Cmd update(Model& m, SearchAllDone);
    static Cmd update(Model& m, SearchAllPick);
    /// Carry out what the active editor asked for (save, clipboard).
    static Cmd editor_actions(Model& m);
    /// Write tab `i`'s file (the state now); empty when there is nothing to do.
    static Cmd save_file(Model& m, std::size_t i);
    /// The pane follows the active tab's directory: list it if it moved.
    static Cmd pane_sync(Model& m);
    // Edit: a direct model edit (the self-test and scripted shots only)
    static Cmd update(Model& m, msg::Edit e) { e(m); settle_scroll(m); return {}; }

    // src/app/input_{keys,pointer,widgets}.cpp: window input -> messages (pure: decide, never change)
    static std::optional<Msg> on(const Model& m, const mayag::KeyEvent& k);
    static std::optional<Msg> on(const Model& m, const mayag::TextEvent& t);
    static std::optional<Msg> on(const Model& m, const mayag::ComposeEvent& c);
    static std::optional<Msg> on(const Model& m, const mayag::ComposeEndEvent&);
    static std::optional<Msg> on(const Model& m, const mayag::ScrollEvent& s_win);
    static std::optional<Msg> on(const Model& m, const mayag::ResizeEvent& r);
    static std::optional<Msg> on(const Model& m, const mayag::FocusEvent& f);
    static std::optional<Msg> on(const Model& m, const mayag::MouseDown& e_win);
    static std::optional<Msg> on(const Model& m, const mayag::MouseMove& e_win);
    static std::optional<Msg> on(const Model& m, const mayag::MouseUp& e_win);
    static std::optional<Msg> on(const Model& m, const mayag::Clicked& c);
    static std::optional<Msg> on(const Model& m, const mayag::Left& l);
    static std::optional<Msg> on(const Model&, const mayag::CloseRequest&) { return RequestQuit{}; }
    static std::optional<Msg> on(const Model& m, const mayag::Pressed& p);
    static std::optional<Msg> on(const Model& m, const mayag::Dragged& d);
    static std::optional<Msg> on(const Model& m, const mayag::Released& r);
    static Sub subscribe(const Model& m);

    // src/view/*.cpp: the widget tree
    static mayag::Theme theme(const Model& m);
    static mayag::Node view(const Model& m);

    // src/app/session.cpp: shared mechanics
    /// A new session asking for a shell (in `cwd`, "" = the current one); the
    /// drain after this step starts it.
    static Session spawn_session(Model& m, const std::string& cwd);
    /// A session asking for ssh to `h` (no shell around it).
    static Session spawn_ssh(Model& m, const Host& h);
    /// Size the grid to what the window holds (the drain tells the ptys).
    static void fit(Model& m);
    /// The most cells one interactive frame may re-wrap (~2.5 ms of moving
    /// cells). A drag that would re-wrap more defers it to the drag's end;
    /// the pane_drag self-test holds every frame of a drag to this.
    static constexpr std::uint64_t kFrameReflowCells = 300'000;
    /// What the step asked of the ptys, as effects: launches, sizes, input.
    static Cmd drain(Model& m);
    /// Ask the ptys what runs in them, in a moment (at most one pending).
    static Cmd probe_soon(Model& m);
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
