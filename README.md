# hand

A terminal. As plain as foot: open it, you get a shell, nothing in the way.

It also carries two things you'd otherwise leave the terminal for:

- **the folder you're in**, beside the panes (`ctrl+shift+e`). It follows the
  focused terminal's working directory; it is not a project, there is nothing
  to open or configure.
- **a real editor**, as a pane (`enter` on a file, or `ctrl+shift+f` to find
  one). Multi-cursor, undo, folding, bracket matching, find, syntax colours —
  next to the terminal you opened it from, in the same split tree, closed the
  same way.

An editor with a terminal is an IDE. This is a terminal with an editor.

## Keys

Every hand chord is `ctrl+shift+…`; plain `ctrl+…` always goes to the program
in the terminal (or, in an editor pane, to the editor).

| keys | |
|---|---|
| `ctrl+shift+\` / `-` | split right / down |
| `ctrl+shift+w` | close pane |
| `ctrl+shift+z` | zoom pane |
| `ctrl+shift+[` / `]` | previous / next pane |
| `ctrl+shift+t` | new tab |
| `ctrl+shift+e` | folder panel |
| `ctrl+shift+f` | find a file in this folder |
| `ctrl+shift+p` | command palette |
| `ctrl+,` | appearance |
| `ctrl+s` (editor) | save |

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build
./build/hand               # window
./build/hand --headless    # scripted self-test
```

Built on [mayag](external/mayag) (UI) and, next, [toe](external/toe) (the VT engine).
