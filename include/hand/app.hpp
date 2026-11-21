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
    using Cmd = mayag::Cmd<Msg, mayag::set_title, mayag::write_clipboard, mayag::read_clipboard, mayag::window_action>;
    using Sub = mayag::Sub<Msg>;
    // every leaf message under its own name (App::NewTab{} reads well at call sites)
    using Output = msg::Output;         using Exited = msg::Exited;
    using NewTab = msg::NewTab;         using CloseTab = msg::CloseTab;     using SelectTab = msg::SelectTab;
    using StepTab = msg::StepTab;       using MoveTab = msg::MoveTab;
    using Blink = msg::Blink;           using Tick = msg::Tick;             using Clock = msg::Clock;
    using Copy = msg::Copy;             using PasteReq = msg::PasteReq;     using Pasted = msg::Pasted;
    using CopyOutput = msg::CopyOutput; using CopyKey = msg::CopyKey;       using OpenUrl = msg::OpenUrl;
    using HintPick = msg::HintPick;     using HintUse = msg::HintUse;
    using SearchToggle = msg::SearchToggle; using SearchStep = msg::SearchStep;
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
    using Notice = msg::Notice;

    static inline Config config{};   ///< set by main() before run
    /// Advance of one mono cell at `size` pt in face `family` (main binds the font stack).
    static inline float (*cell_width)(float size, int family) = [](float size, int) { return size * 0.6f; };

    static Cmd init(Model& m);

    // ── update: one overload per leaf, grouped by domain (jaal checks every leaf is here) ──
    // ShellMsg (src/app/session.cpp)
    static Cmd update(Model& m, Output o);
    static Cmd update(Model& m, Exited e);
    // ClockMsg (src/app/session.cpp)
    static Cmd update(Model& m, Blink);
    static Cmd update(Model& m, Tick t);
    static Cmd update(Model& m, Clock);
    // DialogMsg (src/app/dialog.cpp)
    static Cmd update(Model& m, Answer a);
    static Cmd update(Model& m, RememberToggle);
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
    // SshMsg, ManagerMsg (src/app/ssh.cpp)
    static Cmd update(Model& m, OpenHosts);
    static Cmd update(Model& m, HostsKey k);
    static Cmd update(Model& m, ConnectHost c);
    static Cmd update(Model& m, ConnectTyped);
    static Cmd update(Model& m, Reconnect);
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
    /// The pane follows the active tab's directory: list it if it moved.
    static Cmd pane_sync(Model& m);
    // Edit: a direct model edit from an input gesture
    static Cmd update(Model& m, msg::Edit e) { e(m); settle_scroll(m); return {}; }

    // src/app/input.cpp: window input -> messages
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
    /// Start a shell in a new session (in `cwd`, "" = the current one).
    static Session spawn_session(Model& m, const std::string& cwd);
    /// A session whose program is ssh to `h` (no shell around it).
    static Session spawn_ssh(Model& m, const Host& h);
    /// Resize the grid (and the pty) to what the window holds.
    static void fit(Model& m);
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
