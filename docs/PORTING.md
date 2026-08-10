# Porting `hand` to a new platform

`hand` is the frontend; [`toe`](../../toe) is the engine. Everything that turns
bytes into pixels — VT parsing, the screen model, the GPU renderer, the frame
loop — already works everywhere. A port supplies only the things an OS actually
owns: **a window, input, a PTY, fonts, and a way to wait.**

The current backends are:

| Platform | Window | GPU | PTY | Fonts |
|---|---|---|---|---|
| Linux | Wayland / X11 | GL (EGL) | `forkpty` | dir globbing |
| macOS | Cocoa | Metal | `forkpty` | Core Text |
| Windows | Win32 | D3D11 | ConPTY | DirectWrite |

---

## The five pieces

### 1. The window — `BackendBase<T>`

`toe::App` is a ~20-operation contract, but most of it is portable. Inherit
[`hand/platform/backend_base.hpp`](../include/hand/platform/backend_base.hpp)
and you implement only the OS-specific half:

```cpp
class MySurface final : public BackendBase<MySurface> {
    static Result<std::unique_ptr<MySurface>> open(std::string_view title, PixelSize);
    void begin_frame(PixelSize, uint8_t r, uint8_t g, uint8_t b, float a);
    void end_frame();
    void swap();
    PixelSize pixel_size() const;
    void poll_events(const toe::EventSink &);
    bool should_close() const;
    toe::Readiness wait_readable(int pty_fd, toe::WaitDeadline);
    void set_title(std::string_view);
    void set_clipboard(std::string_view);
    std::string get_clipboard();
    void open_url(std::string_view);
};
```

The base already provides the settings/help panes, `bind_terminal`,
`handle_chord`, `flush`, `swap_damaged`, and the `event_fd`/`repeat_fd` stubs.
Override one only when your OS can do better (Wayland overrides `flush` and
`swap_damaged`; Linux overrides the fd accessors).

**Do not re-implement keybindings.** Classify your native key event into a
`Chord` and call `handle_chord()`. What a chord *does* is shared; only the
physical key mapping is yours. (The Windows port originally shipped without a
settings keybinding precisely because that logic was copy-pasted per backend —
which is why the base exists.)

Register the type in `hand/app.hpp` (`MyApp = BackendApp<platform::MySurface>`,
a `Backend` enum entry, a `run_myplatform` declaration) and instantiate
`toe::run<MyApp>` at the bottom of your TU, where the surface type is complete.
That keeps the frame loop **monomorphic** — direct calls, no vtable.

### 2. Readiness — one wait, no polling

The loop blocks in exactly one place: `wait_readable(pty_fd, deadline)`. It must
wake on child output, window events, and a timer deadline, and never spin.

- Linux: `epoll_pwait2` over three fds ([`reactor.hpp`](../include/hand/reactor.hpp)).
- Windows: `MsgWaitForMultipleObjectsEx` over handles + the message queue
  ([`win_reactor.hpp`](../include/hand/platform/win_reactor.hpp)).

If your OS can't wait on your PTY handle directly, convert it: run one reader
thread and have it signal an event you *can* wait on. Never poll.

### 3. The PTY — the host creates it

`toe` never forks. Produce a `toe::AdoptFd{master_fd, child, owns_fd}` and the
engine adopts it. On POSIX that's `forkpty`. If your OS has no fds, add a
registry that maps an `int` to real handles — see
[`toe/pty/win_io.hpp`](../../toe/include/toe/pty/win_io.hpp). Keeping the
fd-shaped API means no `#ifdef` leaks into the engine.

### 4. Fonts — resolve a file path

`toe` rasterises with `stb_truetype`, so it needs a **path**. Implement
[`hand/platform/fonts.hpp`](../include/hand/platform/fonts.hpp):
`resolve_font_file`, `list_monospace_families`, `resolve_font_styles`.

### 5. Config paths

`find_config()` and `user_themes_dir()` must resolve identically, or the
settings pane writes one place and reads another. Built-in themes are compiled
in (`themes.gen.hpp`); the user themes dir is optional.

---

## Hard-won gotchas

**Windows / ConPTY.** These cost real debugging time; the symptoms are all
identical and misleading (a blank grid, or child output appearing in the
console that launched `hand`):

- Channels must be **synchronous** pipes. A `FILE_FLAG_OVERLAPPED` pipe makes
  the pseudoconsole silently fail to bind the child.
- Set **`STARTF_USESTDHANDLES` with NULL std handles**, or the child inherits
  your console and writes there instead of the pty.
- Do **not** pass `CREATE_NO_WINDOW` or `CREATE_UNICODE_ENVIRONMENT` (with a
  null environment block) — both break the pseudoconsole binding.
- Release the pty-side handles only **after** `CreateProcess`.
- Call `InitializeProcThreadAttributeList` exactly twice (size probe, then
  init). A third call silently wipes the pseudoconsole attribute.
- ConPTY **repaints its whole viewport on every resize**, so spawn the pty at
  the real grid size (`SpawnCommand::cols/rows`, via
  `FontAtlas::probe_cell_size`) — otherwise every command's output renders
  twice.

**Win32 windowing.** Adopt the `HWND` in `WM_NCCREATE`: messages are dispatched
*during* `CreateWindowExW`, before it returns, and a falsy `WM_NCCREATE` result
aborts window creation.

**Testing.** Tests must not hardcode `/tmp` or use `setenv` — use
`std::filesystem::temp_directory_path()` and `_putenv_s` on Windows.

---

## Verifying a port

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure   # 5 platform-independent tests
./build/hand                                  # a real shell, in a real window
```

Then check by hand: type; run a full-screen app (`vim`, `htop`); resize;
open the settings pane; select and copy; confirm output isn't duplicated.

To see exactly what the child sent, set `HAND_PTY_DUMP=<path>` and decode the
capture — that single trick localises most "the terminal looks wrong" bugs to
either the PTY layer or the renderer.

---

## Measuring performance

Two numbers matter, and they are measured differently.

**Throughput** (parse + screen model, no GPU) — `tests/throughput_bench.cpp`:

```sh
cmake --build build --target throughput_bench
./build/tests/throughput_bench
```

It reports **best-of-7**. This is not fussiness: run-to-run spread on a laptop
is easily 10% (turbo, thermals, E-core placement), which is larger than most
real optimisations — a single sample will happily "confirm" a change that did
nothing. Two optimisations were rejected here precisely because best-of-N showed
their apparent wins were noise.

**Input-to-photon latency** — the number that decides how instant the terminal
feels. It cannot be measured on the CPU side: it spans the PTY round trip, the
parse, the GPU frame and the compositor. `hand` measures it itself:

```sh
# Linux/macOS: the HUD prints to stderr a few times a second.
HAND_LATENCY_HUD=1 ./hand

# Windows: a GUI-subsystem build has no console, so name a FILE instead.
set HAND_LATENCY_HUD=%TEMP%\lat.log
hand.exe
```

Then drive real keystrokes through the OS input path (Windows):

```sh
latency_driver.exe 300 30      # 300 keys, 30 ms apart
```

Measured on a 60 Hz laptop panel: **avg ~4-7 ms, p99 ~30 ms**.

That p99 is not a bug to chase: 30 ms is exactly **two vsync intervals at
60 Hz**, i.e. the DWM compositor's floor for a windowed app. The useful signal
is the *average*, and whether p99 sits near a multiple of the refresh interval
(compositor) or near one of the loop's own constants such as `kFloodPresentMs`
(our bug). Check the refresh rate before concluding anything:

```sh
powershell -c "(Get-CimInstance Win32_VideoController).CurrentRefreshRate"
```
