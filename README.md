# hand

A terminal. As plain as foot: open it and you get a shell, nothing in the
way. Tabs, search, hints, copy mode and SSH are there when you reach for them.

Built on [toe](external/toe) (the VT engine), [mayag](external/mayag) (the
GPU UI) and [vibe](external/vibe) (the config format).

## Keys

Every hand chord is `ctrl+shift+…`; plain `ctrl+…` goes to the program in the
terminal.

| keys | |
|---|---|
| `ctrl+shift+t` / `w` | new tab / close tab |
| `ctrl+tab`, `ctrl+pgup/pgdn`, `ctrl+shift+←/→` | previous / next tab |
| `alt+1` … `alt+9` | go to tab N |
| `ctrl+shift+pgup/pgdn` | move the tab |
| `ctrl+shift+s` | SSH hosts |
| `ctrl+shift+f` | search the scrollback |
| `ctrl+shift+space` | hints: pick a URL / path / hash on screen |
| `ctrl+shift+m` | copy mode (vim keys through the scrollback) |
| `ctrl+shift+z` / `x` | previous / next prompt |
| `ctrl+shift+g` | copy the last command's output |
| `ctrl+shift+c` / `v` | copy / paste |
| `ctrl+=` / `-` / `0` | font size |
| `shift+pgup/pgdn`, `shift+home/end` | scroll |
| `ctrl+,` | settings |

## SSH

`ctrl+shift+s` lists every host in `~/.ssh/config` (read the way ssh reads
it: first value wins, globs, `Include`), the machines in `known_hosts`, and
any `user@host[:port]` you typed once. Pinned and recent hosts come first.
Enter connects in a new tab; one connection per host is shared, so a second
tab opens instantly. A production host (by name, or `env prod` in
`hosts.vibe`) is marked red; a dropped connection keeps its tab and offers to
reconnect. `~/.ssh/config` is never written; hand's notes on your hosts live
in `hosts.vibe` beside `hand.vibe`.

## Settings

`ctrl+,` opens settings over the terminal. Every edit applies live and is saved to
`~/.config/hand/hand.vibe` (the file is generated from the settings table in
`src/config/settings.cpp`; every key is a setting). Colours come from all 913
[iTerm2-Color-Schemes](https://github.com/mbadolato/iTerm2-Color-Schemes);
regenerate the catalogue with `tools/gen_schemes.py`.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hand
./build/hand                              # the terminal
./build/hand --headless --last-resort     # the self-test (also: ctest)
./build/hand --png out.png --shot tabs    # a screenshot (tabs, settings, hosts, ssh, ...)
```

How the code is laid out (layers, the model and message trees, the type
rules) is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
