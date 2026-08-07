# hand

A terminal — a pun on [foot](https://codeberg.org/dnkl/foot).

`hand` is a native, keyboard-driven terminal emulator built on
[`libgvte`](../gvte) — the from-scratch GPU terminal engine. It has **no GTK,
no VTE, and no SDL**: the window is a direct Wayland or X11 surface (EGL), and
the terminal core (PTY, VT parser, grid, GPU renderer) is `libgvte`.

It's the successor to the original GTK/VTE termite (still at the repo root),
reborn on the GPU stack.

## Build

```sh
cmake -S . -B build
cmake --build build -j
./build/hand
```

The build pulls in `libgvte` from the sibling `../gvte` directory.

## Configuration

`hand` reads an INI config from the first of:

1. `-c PATH` / `--config PATH`
2. `$XDG_CONFIG_HOME/hand/config` (or `~/.config/hand/config`)
3. `$XDG_CONFIG_HOME/termite/config` — for drop-in compatibility with an
   existing termite config

Recognised keys:

```ini
[options]
font = Monospace 11      # family + point size (scaled to px at 96 DPI)

[colors]
foreground = #dcdccc
background = #171720
```

## Dependencies

Just what `libgvte` needs: a C++23 compiler, CMake, EGL, Wayland
(`wayland-client/egl`, `xkbcommon`), X11 (`x11`, `xcb`, `xkbcommon-x11`),
FreeType, HarfBuzz, Fontconfig. No GTK, VTE or SDL.
