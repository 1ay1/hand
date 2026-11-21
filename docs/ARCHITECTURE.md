# hand — architecture

hand is one Elm-style program on mayag (the GPU UI), with toe as its VT
engine. This page is the map and the rules. When code and this page disagree,
fix one of them in the same commit.

## Layers

```
            ┌──────────── hand (src/main.cpp + src/cli/) ────────────┐
            │  parses argv, loads fonts and config, picks a mode     │
            └───────────────────────────┬────────────────────────────┘
                                        ▼
            ┌──────────── hand_app (src/app/, src/view/) ────────────┐
            │  Model, Msg, update (per domain), input → Msg, view    │
            └──────────┬──────────────────────────────┬──────────────┘
                       ▼                              ▼
     ┌──── hand_config (src/config/) ────┐  ┌──── hand_ssh (src/ssh/) ────┐
     │ settings table, hand.vibe, schemes│  │ ~/.ssh, hosts.vibe, sftp    │
     └─────────────────┬─────────────────┘  └──────────────┬──────────────┘
                       ▼                                   ▼
            ┌──────────── hand_core (src/core/) ─────────────────────┐
            │ Term (toe glue), Pty, procs, shell integration,        │
            │ the OS seam, Palette, vocabulary types (types.hpp)     │
            └────────────────────────────────────────────────────────┘
                   every layer may use: mayag, toe, vibe, std
                   no layer may use:    jaal (mayag's implementation)
```

Each layer is a static library that links only what is below it
(`hand_layer(...)` in `CMakeLists.txt`). Headers share one `include/` root, so
`tests/layering.cmake` (ctest `layering`) checks every `#include` against the
graph and fails on an upward one, or on any mention of jaal.

| layer  | headers               | sources                | may include `hand/...` |
|--------|-----------------------|------------------------|------------------------|
| core   | `include/hand/core`   | `src/core`             | core                   |
| config | `include/hand/config` | `src/config`           | core, config           |
| ssh    | `include/hand/ssh`    | `src/ssh`              | core, ssh              |
| app    | `include/hand/app*`   | `src/app`, `src/view`  | everything             |

A lower layer that needs something from a higher one takes it as a parameter.
For example, `ssh::resolve(HostSources)` gets its `store` path from the app
(`detail::host_sources`) rather than calling `config_path()` itself.

## The program

`hand::App` (`include/hand/app.hpp`) is a mayag/jaal program:
`init`, `update`, `on`, `subscribe`, `theme`, `view`.

### Model: a tree of plain typed data

```
Model : Session                     the active tab IS the model's terminal
├── cfg, palette, metrics, …        window-wide
├── tabs, active, session_ids       parked sessions, SessionIds hands out ids
├── strip      StripModel           ┐
├── minimap    MinimapModel         │ include/hand/app/model/chrome.hpp
├── settings   SettingsModel        │
├── dialog     DialogModel          │
├── toast      ToastModel           ┘
├── ssh        SshModel{sources, book, picker, manager}   model/ssh.hpp
└── pane       PaneModel                                  model/pane.hpp
Session                             one tab: pty, term, scroll, selection
├── copy       optional<CopyMode>   ┐ model/modes.hpp: engaged exactly
├── hint       optional<HintMode>   │ while the mode is on
└── remote     optional<Remote>     ┘
```

A feature's state lives in its own struct, in its own header. A mode that may
be absent is an `optional`, never a `bool` next to its data.

### Msg: a closed tree of sums

```
Msg ─┬─ ShellMsg    Output Exited
     ├─ TabMsg      NewTab CloseTab SelectTab StepTab MoveTab TabPress TabDrag TabRelease
     ├─ ClockMsg    Blink Tick Clock
     ├─ TextMsg     Copy PasteReq Pasted CopyOutput CopyKey OpenUrl HintPick
     ├─ SearchMsg   SearchToggle SearchStep SearchKey SearchText
     ├─ ScrollMsg   ScrollBy ScrollLive ScrollTop ScrollToPrompt ScrollToLine JumpPrompt ClearScrollback
     ├─ SelectMsg   SelectBegin SelectExtend SelectOutput SelectEnd SelectClear
     ├─ PointerMsg  ReportPress ReportMotion ReportRelease HoverLink MinimapHover PointerLeft
     ├─ KeyboardMsg Typed Swallow ModifierDown FontZoom Compose ComposeEnd Focused Resized
     ├─ ModeMsg     ToggleCopyMode OpenHints CloseHints HintBackspace HintTyped PickerText PickerClose PaneBlur
     ├─ DialogMsg   Answer RememberToggle DialogFocus
     ├─ WindowMsg   RequestQuit WindowAct Panel ToggleSettings
     ├─ SshMsg      OpenHosts HostsKey ConnectHost ConnectTyped Reconnect
     ├─ ManagerMsg  ToggleManager Manager FilesListed FilesDone UploadPicked TunnelProbe TunnelScan
     ├─ PaneMsg     TogglePane PaneOp PaneListed PaneResize PaneWatch PanePreview
     └─ Edit        a direct model edit -- the self-test and shots only
```

Input handlers (`src/app/input.cpp`) are pure: they read the model, decide
which message a key, click or drag is, encode any bytes, and return it. Every
change to the model is a named message with its own `update`. The `layering`
test rejects an anonymous `mayag::Edit` anywhere in `src/app` or `src/view`.

`include/hand/app/msg.hpp` defines each domain as a `std::variant` of leaf
structs, and `msg::Any` as a struct deriving from the variant of domains.
A leaf converts straight into `Msg` (`return NewTab{};`), because only one
domain holds it. jaal walks the tree to `update(Model&, Leaf)`. A leaf
without a handler does not compile, and the error names its path.

To add a message: add the leaf to its domain's variant, declare
`update(Model&, Leaf)` in `app.hpp` under that domain, and define it in that
domain's file.

| domain     | handlers in             |
|------------|-------------------------|
| ShellMsg, ClockMsg | `src/app/session.cpp` |
| TabMsg     | `src/app/tabs.cpp`      |
| TextMsg, SearchMsg | `src/app/clipboard.cpp` |
| ScrollMsg  | `src/app/scroll.cpp`    |
| SelectMsg, PointerMsg | `src/app/pointer.cpp` |
| KeyboardMsg | `src/app/keyboard.cpp` |
| ModeMsg    | `src/app/modes.cpp`     |
| DialogMsg, RequestQuit | `src/app/dialog.cpp` |
| Panel, ToggleSettings | `src/app/settings.cpp` |
| SshMsg, ManagerMsg | `src/app/ssh.cpp` |
| PaneMsg    | `src/app/pane.cpp`      |
| (input)    | `src/app/input.cpp` — window events → Msg |

### View: composition only

`src/view/*.cpp` turns the model into mayag widget props and composes the
widgets (`tabbed_window`, `tab_strip`, `terminal_view`, `term_dialog`,
`settings_panel`, `ssh_manager`, `file_pane`, …). Nothing in hand draws. A
missing visual is added to mayag as a widget, not hand-built in the view.

## Types

`include/hand/core/types.hpp` is the vocabulary:

- **`Strong<Tag, T>`**: a newtype. Explicit in (`SessionId{7}`), explicit out
  (`.get()`), compares and hashes like `T`, and has no arithmetic. It is an
  aggregate so jaal can prove messages carrying it `Sendable`.
- **`SessionId`** routes pty output. **`TabIndex`** is a slot in the strip.
  The two never convert into each other or into an integer silently.
- **`Step`** is `backward` / `forward` (never `-1` / `1`). `wrap(i, step, n)`
  steps i around a ring of n.
- **`Confirm`** is `asking` / `given`. It replaces `bool confirmed`, so a call
  site reads `CloseTab{tab, Confirm::given}`.
- **`overloaded{...}`** turns lambdas into one visitor for `std::visit`.

Messages follow the same rules:
- A choice is an enum: `Where::this_tab`, `HintUse::insert`, `Notice::quiet`.
- An answer is a sum: `Answer::dismiss()` / `Answer::choose(i)`, and the
  "don't ask again" box is its own message (`RememberToggle`).

## Rules

1. **Layering.** Include only downward. A lower layer gets what it needs as a
   parameter. Never include jaal.
2. **One domain per file.** A file holds one domain's handlers (or one
   widget-tree branch, one run mode) and starts with a comment saying what it
   owns.
3. **State is typed data.** Use a sub-model struct per feature and
   `optional` for modes. No `bool` beside the data it guards.
4. **No magic values in messages.** Ids are `Strong`, directions are `Step`,
   choices are enums, and alternatives are variants.
5. **Changes are named.** Input decides; `update` changes. No anonymous model
   edits in the app or view (enforced by `layering`).
6. **Effects are returned.** `update` mutates the model and returns a `Cmd`:
   clipboard, title, window actions, tasks. Threads exist only as mayag
   streams and tasks (`Sub::stream`, `Task`).
7. **The UI is mayag.** View code composes widgets with theme tokens. A new
   visual is a mayag widget.
8. **Every behaviour has a check.** `hand --headless` (ctest `app_headless`)
   drives the app like a user. Add an `ok(...)` for every fix. Screens are
   reproducible with `hand --png out.png --shot NAME`.
9. **Zero warnings.** hand builds with `-Wall -Wextra -Wshadow -Wconversion`,
   and a new warning is a bug.

## Run modes (`src/cli/`)

| flag | file | what |
|------|------|------|
| *(none)* | `main.cpp` | the window |
| `--headless` | `cli/selftest.cpp` → `cli/checks/*.cpp` | the self-test, one suite per feature; exit code = failures |
| `--png F --shot NAME` | `cli/shots.cpp` | one staged frame to PNG |
| `--bench`, `--flood CMD` | `cli/shots.cpp` | frame cost, throughput, memory |
| `--repro-narrow[-fish]`, `--repro-resize` | `cli/repro.cpp` | resize repros on real shells |
| `--emit-red`, `--cat-v` | `cli/fixtures.cpp` | hand as a pty test fixture |

The self-test's suites (`src/cli/checks/`) run in a fixed order on one
runtime: `pty`, `settings_panel`, `every_setting`, `visual_settings`,
`terminal_features`, `fish_integration`, `resize_fish`, `wide_squeeze`,
`wrap_spaces`, `selection`, `tabs`, `tui_mouse`, `mac`, `ssh`,
`config_roundtrip`. Each reports through a `Check`. To add a suite, declare it
in `checks/suites.hpp`, add a file, and list it in `selftest.cpp`.

## Build and test

```sh
cmake -S . -B build && cmake --build build --target hand -j8
cd build && ctest --timeout 120      # app_headless + layering
```
