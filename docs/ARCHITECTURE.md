# hand — architecture

hand is one Elm-style program on mayag (the GPU UI), with toe as its VT
engine. This page is the map and the rules. When code and this page disagree,
fix one of them in the same commit.

## The hierarchy: hand → mayag → jaal

```
  hand    the terminal: names mayag only, through its public surface
    │       <mayag/mayag.hpp>, <mayag/widget/...>, <mayag/core/event.hpp>
    ▼
  mayag   the GPU UI: widgets, theme, the window host. It reaches jaal only
    │       through jaal's public API, and re-exports what an app needs under
    │       its own names: Cmd, Sub, Sink, CoOwned, Shared, Guarded, Sendable,
    ▼       Sync, MAYAG_SYNC / MAYAG_SENDABLE / MAYAG_FROZEN
  jaal    the runtime: the loop, tasks, streams, the thread-safety concepts;
            names nothing above it
```

Each arrow is one-way and checked by ctest on every hand build:

| test | wall |
|------|------|
| `layering` | hand's own layers (below). hand never names jaal (no include, no `jaal::`, no jaal macro), never includes mayag's internals or `mayag::detail`, and `core` never pulls in the mayag runtime |
| `mayag_layering` | mayag's layers, and only mayag's runtime glue includes jaal |
| `mayag_uses_jaal_public_api` | mayag never reaches into `jaal::detail` or jaal's private keys |
| `jaal_upward` | jaal never names mayag or hand |
| `concurrency_*`, `mayag_concurrency` | jaal's own banlist over hand and over mayag (next section) |

When hand needs something only jaal has, it is added to mayag's vocabulary
first (for example `mayag::Sendable`, `mayag::Sync` and `MAYAG_SYNC`), never
reached around.

## Concurrency: race-free by construction

hand's app code has no threads, locks or atomics. Concurrency comes in three
shapes, and the compiler checks each one:

- **Background work** is a mayag `Cmd::task` or `Sub::stream`. Its body is
  captureless, every argument must be `Sendable` (owned, nothing borrowed),
  and it talks back only through a weak `Sink`. A stream stops when
  `subscribe` stops asking for it, and late messages from it are dropped.
- **Messages** cross threads, so `msg.hpp` ends with
  `static_assert(mayag::Sendable<msg::Any>)`. A pointer, view or
  `shared_ptr` in any leaf of the tree fails the build and names the field.
- **Shared mutable state** is a `mayag::CoOwned<T>`, which requires
  `mayag::Sync<T>`. There is one in hand: `OutputCredit`
  (`core/credit.hpp`), the pty reader's flow control. It travels inside
  each `Output`, and `update` gives it back when the bytes are fed, so at
  most one chunk is ever in flight. A firehose like `yes` is throttled by
  the kernel's pty buffer instead of the mailbox. Its Sync claim, with the
  reason, is in `msg.hpp`.

jaal's banlist runs over hand (`concurrency_src`, `concurrency_include_hand`).
`std::thread`, `mutex`, `atomic`, `detach`, `thread_local`, `async`,
`future` and `const_cast` are allowed only where `tests/allow_raw_*.txt`
grants them, each with a reason:
- the Windows ConPTY spawner and pipe-drain threads (joined);
- the one atomic inside `OutputCredit`;
- `exec`'s `char* const argv[]`.

The `_allowlist_tight` tests fail when a grant is no longer used, so the
lists can only shrink.

## Layers inside hand

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
                   every layer may use: toe, vibe, std, and mayag's public
                   headers (core: <mayag/core/event.hpp> only, no runtime)
                   no layer may use:    jaal, mayag internals
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
Model                               the window
├── cfg, palette, metrics, time, …  window-wide
├── tabs       Tabs                 every Session, in strip order    model/tabs.hpp
├── session_ids SessionIds          hands out each new tab's id
├── strip      StripModel           ┐
├── minimap    MinimapModel         │ model/chrome.hpp
├── settings   SettingsModel        │
├── dialog     DialogModel          │
├── toast      ToastModel           ┘
├── ssh        SshModel{sources, book, picker, manager}             model/ssh.hpp
└── pane       PaneModel                                            model/pane.hpp

Session                             one tab: pty, term, scroll, selection, motion   model/session.hpp
├── copy       optional<CopyMode>   ┐ model/modes.hpp: engaged exactly
├── hint       optional<HintMode>   │ while the mode is on
└── remote     optional<Host>       ┘
```

The window is not a tab. `m.tab()` is the one in front; `m.at(i)` is any.
`Tabs` keeps two invariants by construction: it is never empty, and the tab
in front stays in front while others are inserted, closed or moved around it
(`insert`, `erase`, `move_active`, `select`, `find(SessionId)`). Handlers say
what they mean, and the self-test suite `tab_list` checks the invariants.

A handler that works on the front tab binds it once:

```cpp
App::Cmd App::update(Model& m, ScrollToLine s) {
    Session& se = m.tab();
    ...
}
```

A handler that may switch, open or close tabs keeps saying `m.tab()`, because
a reference taken before that would point at the wrong tab or at freed memory.

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

Input handlers (`src/app/input_*.cpp`) are pure: they read the model, decide
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
| (input: keys) | `src/app/input_keys.cpp` — chords, text, the input method → Msg |
| (input: pointer) | `src/app/input_pointer.cpp` — wheel, press/move/release → Msg |
| (input: widgets) | `src/app/input_widgets.cpp` — resize, focus, gestures on nodes → Msg |
| (subscriptions) | `src/app/subscribe.cpp` — pty readers, blink, clock, frames |

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
   parameter. hand → mayag (public headers) → jaal, never around.
2. **No raw concurrency.** Use a task, a stream, or a `CoOwned<Sync T>`. A
   raw primitive needs a grant in `tests/allow_raw_*.txt` with its reason.
3. **One domain per file.** A file holds one domain's handlers (or one
   widget-tree branch, one run mode) and starts with a comment saying what it
   owns.
4. **State is typed data.** Use a sub-model struct per feature and
   `optional` for modes. No `bool` beside the data it guards.
5. **No magic values in messages.** Ids are `Strong`, directions are `Step`,
   choices are enums, and alternatives are variants.
6. **Changes are named.** Input decides; `update` changes. No anonymous model
   edits in the app or view (enforced by `layering`).
7. **Effects are returned.** `update` mutates the model and returns a `Cmd`:
   clipboard, title, window actions, tasks. Threads exist only as mayag
   streams and tasks (`Sub::stream`, `Task`).
8. **The UI is mayag.** View code composes widgets with theme tokens. A new
   visual is a mayag widget.
9. **Every behaviour has a check.** `hand --headless` (ctest `app_headless`)
   drives the app like a user. Add an `ok(...)` for every fix. Screens are
   reproducible with `hand --png out.png --shot NAME`.
10. **Zero warnings.** hand builds with `-Wall -Wextra -Wshadow -Wconversion`,
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
cmake --preset dev && cmake --build --preset dev     # build-rel: --preset rel, build-tsan: --preset tsan
cd build && ctest --timeout 120      # the self-test and every wall (ctest -L static: walls only)
```
