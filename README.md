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

`hand` is configured with a [VIBE](https://github.com/1ay1/vibe) file — a small,
no-nonsense config format (`key value`, objects in `{ }`, `#` comments, no `=`
or `:`). The header is vendored at `vendor/vibe.h`.

It is read from the first of:

1. `-c PATH` / `--config PATH`
2. `$XDG_CONFIG_HOME/hand/config.vibe` (or `~/.config/hand/config.vibe`)

See [`config.vibe`](config.vibe) for a sample:

```vibe
font {
    family "monospace"
    size   11            # points, scaled to px at 96 DPI
}

colors {
    foreground "#dcdccc"
    background "#171720"
}
```

A parse error is reported to stderr and `hand` falls back to built-in defaults.

## Dependencies

Just what `libgvte` needs: a C++23 compiler, CMake, EGL, Wayland
(`wayland-client/egl`, `xkbcommon`), X11 (`x11`, `xcb`, `xkbcommon-x11`),
FreeType, HarfBuzz, Fontconfig. No GTK, VTE or SDL.
