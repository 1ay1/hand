# hand ✋

> A terminal emulator. It's a pun on [`foot`](https://codeberg.org/dnkl/foot).
> Get it? Foot… hand. We'll be here all week.

`hand` is a native, keyboard-driven terminal emulator with **no GTK, no VTE, and
no SDL** — just a bare Wayland or X11 surface (EGL) and a from-scratch GPU
terminal engine underneath. It is a thin, well-mannered frontend over
[`libtoe`](../toe): `hand` opens the window, reads your config, and gets out of
the way. `toe` does the actual terminal-ing.

Think of it as the appendage on the end of the arm that is `toe`. Foot has toes;
hand has fingers; we didn't overthink the anatomy.

## Why does this exist?

Because someone looked at `foot` — a genuinely excellent terminal, we love it,
no notes — and thought: *"you know what would make this better? A worse name and
a completely different codebase."*

More seriously: `hand` is the successor to the original GTK/VTE `termite`,
reborn on a GPU stack that we own top to bottom. No 400 MB of GNOME libraries to
render `ls`. No mystery `VTE` version pinning. Just pixels, a PTY, and vibes
(literally — see config).

## Build

```sh
cmake -S . -B build
cmake --build build -j     # the -j is not optional if you value your afternoon
./build/hand
```

The build pulls in `libtoe` from the sibling `../toe` directory if it's there,
and downloads it from GitHub if it isn't. Same deal for the config parser. It's
polite like that — checks the neighborhood before ordering online.

> **Pro tip:** if `./build/hand` opens a window and a shell appears, congratulations,
> you now have a hand. Wave it around a bit.

## Configuration

`hand` is configured with a [VIBE](https://github.com/1ay1/vibe) file — a small,
opinionated config format where `key value`, objects go in `{ }`, `#` starts a
comment, and there is emphatically **no `=` and no `:`**. If you type `port = 8080`
into a VIBE file, VIBE will look at you with quiet disappointment and parse
nothing.

The config is read from the first of:

1. `-c PATH` / `--config PATH`
2. `$XDG_CONFIG_HOME/hand/config.vibe` (or `~/.config/hand/config.vibe`)

If your config has a typo, `hand` prints the error to stderr and calmly falls
back to built-in defaults instead of throwing a tantrum and refusing to start.
A terminal that won't open because a hex color is malformed is not a friend.

A sample lives in [`config.vibe`](config.vibe):

```vibe
font {
    family "monospace"   # any fontconfig name
    size   11            # points — hand scales it to pixels at 96 DPI for you
}

colors {
    foreground "#dcdccc"
    background "#171720"
}
```

That's the whole config surface today. If you were hoping for 200 tunable knobs,
this may not be your terminal — but the four things you actually change are all
here.

## What's actually in the box

`hand` is small and layered — each concern is its own tiny, typed unit rather
than one big procedural loop:

| File | Role |
|------|------|
| `main.cpp` | just the *shape of a frame*: five named steps |
| `event_router.hpp` | input policy as an **exhaustive `std::visit`** over the closed event sum — one named `handle()` per event kind, no `if` ladder |
| `config.{hpp,cpp}` | VIBE → `toe::Config`, with a `HexColor` newtype (invalid strings can't masquerade as colours) and RAII over the C parser handle |
| `blink.hpp` | `Millis`, `SquareWave<Tag, Period>`, `BlinkState` — the cursor/text blink waves as strong types, not `(ms/530)%2` scattered inline |
| `poll_set.hpp` | a `Timeout` value + a `PollSet` builder over `poll(2)` — you `add()` named fds and ask `ready(fd)`, never juggle `pollfd[3]` + `nfds` |

Most of `main.cpp` is comments explaining the two or three genuinely clever bits:

- **Zero-latency local echo.** When you type, `hand` hands the byte to the child,
  then `poll()`s the PTY for a *whole 3 milliseconds* hoping the shell echoes it
  back in time to render in the *same* frame — because a keystroke that shows up
  one vsync late feels awful. Shells echo in microseconds, so this basically
  always wins and never actually waits.
- **It sleeps.** The main loop `poll()`s until the child has output, a window
  event lands, or the cursor-blink timer fires. No 100%-CPU busy-spin. Your fan
  will not know `hand` is running. (During a `yes`/`cat /dev/urandom` flood it
  skips the nap and keeps draining, so the UI stays alive while the screen
  melts.)
- **It only draws when something changed.** A `RenderKey{generation, blink}`
  folds the damage counter and both blink phases into one comparable value; if
  this frame's key equals the last drawn one, nothing is rendered. Idle terminal
  = zero wasted GPU frames, except the ~530 ms heartbeat to blink the cursor.
- **The window title follows the app** (OSC 0/2), **OSC 52 clipboard** requests
  are honored, and **inline-image animations** (kitty `a=f`) tick along at their
  intended framerate.

The window comes from `toe::platform`, the terminal from `toe::Terminal`, the
config from `vibe.h`. `hand` is the glue, and it's proud of being just glue.

## Dependencies

Whatever `libtoe` needs, which is: a C++23 compiler, CMake, EGL, Wayland
(`wayland-client`/`egl`, `xkbcommon`), X11 (`x11`, `xcb`, `xkbcommon-x11`),
FreeType, HarfBuzz, Fontconfig, and `epoxy`.

Notably **absent:** GTK, VTE, SDL, Electron, a browser engine, a JavaScript
runtime, or 1.2 GB of `node_modules`. It renders text in a box. It does not need
a village.

## The family

`hand` doesn't work alone — it's the frontend of a three-part stack that all
lives under `../`:

| Repo | Role |
|------|------|
| [`hand`](.) | **this** — the app: window, config, wiring (you are here) |
| [`toe`](../toe) | the engine: PTY, VT parser, grid, GPU renderer — pure Elm-style core |
| [`vibe`](../vibe) | the config format `hand` reads (`config.vibe`) |

## License

LGPL-2.0-or-later. Use it, ship it, fork it, give it a better pun.

---

*Runs vim. Runs tmux. Runs htop. Runs the risk of being the only terminal named
after a body part you can high-five with.* ✋
