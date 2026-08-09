#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# torture.py — a visible stress harness for `hand`. Run it INSIDE the terminal:
#
#     ./build/hand -e python3 tools/torture.py            # interactive menu
#     ./build/hand -e python3 tools/torture.py all        # run every scenario
#     ./build/hand -e python3 tools/torture.py flood sgr   # run named scenarios
#
# Each scenario hammers a different part of the VT/screen/renderer pipeline and
# prints a live throughput/timing readout so you can WATCH the terminal cope (or
# not). Nothing here is hand-specific — it's pure ANSI, so you can point it at
# Terminal.app / iTerm / kitty for a side-by-side.

import os
import sys
import time
import random
import shutil

W = lambda s: (sys.stdout.write(s), sys.stdout.flush())
CSI = "\x1b["

def size():
    try:
        c = shutil.get_terminal_size((80, 24))
        return c.columns, c.lines
    except Exception:
        return 80, 24

def reset():
    W(CSI + "0m" + CSI + "?25h" + CSI + "?1049l" + CSI + "?7h")  # sgr, cursor, main screen, wrap

def banner(title):
    cols, _ = size()
    line = "─" * max(0, cols)
    W(CSI + "0m\n" + CSI + "1;38;5;39m" + line + "\n" + f" ▶ {title}\n" + line + CSI + "0m\n")

def rate_line(label, nbytes, secs, extra=""):
    mb = nbytes / 1e6
    rate = mb / secs if secs > 0 else 0.0
    W(CSI + "0m" + f"  {label}: {mb:8.1f} MB in {secs:6.2f}s  =  "
      + CSI + "1;38;5;46m" + f"{rate:8.1f} MB/s" + CSI + "0m"
      + (f"   {extra}" if extra else "") + "\n")

# ── scenarios ────────────────────────────────────────────────────────────────

def flood(seconds=3.0):
    """Raw print flood: how many bytes/sec can the parser+screen+renderer eat."""
    banner(f"FLOOD — raw text at max rate for {seconds:.0f}s")
    chunk = ("The quick brown fox jumps over the lazy dog. " * 20 + "\n") * 40
    data = chunk.encode()
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        os.write(1, data)
        n += len(data)
    rate_line("flood", n, time.time() - t0)

def sgr(seconds=3.0):
    """SGR color storm: every cell a different truecolor fg/bg — kills naive attr caches."""
    banner(f"SGR STORM — full 24-bit color churn for {seconds:.0f}s")
    cols, rows = size()
    buf = []
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        buf.clear()
        buf.append(CSI + "H")  # home
        for _ in range(rows - 1):
            for _ in range(cols):
                r = random.randint(0, 255); g = random.randint(0, 255); b = random.randint(0, 255)
                buf.append(f"\x1b[38;2;{r};{g};{b};48;2;{255-r};{255-g};{255-b}m█")
        s = "".join(buf).encode()
        os.write(1, s)
        n += len(s)
    rate_line("sgr", n, time.time() - t0)

def cursor(seconds=3.0):
    """Cursor-warp storm: random absolute moves + writes — stresses grid addressing."""
    banner(f"CURSOR WARP — random addressing for {seconds:.0f}s")
    cols, rows = size()
    glyphs = "@#%&*█▓▒░◆●▲"
    buf = []
    n = 0
    t0 = time.time()
    W(CSI + "2J")
    while time.time() - t0 < seconds:
        buf.clear()
        for _ in range(4000):
            y = random.randint(1, max(1, rows - 1)); x = random.randint(1, cols)
            col = random.randint(16, 231)
            buf.append(f"\x1b[{y};{x}H\x1b[38;5;{col}m{random.choice(glyphs)}")
        s = "".join(buf).encode()
        os.write(1, s)
        n += len(s)
    rate_line("cursor", n, time.time() - t0)

def unicode_test(seconds=3.0):
    """Unicode / emoji / CJK / combining marks / box-drawing — fallback-chain + width."""
    banner(f"UNICODE STORM — emoji · CJK · combining · box for {seconds:.0f}s")
    pools = [
        "🔥💥🚀🎉😀🌈🦄🍕🎸⚡️🌟💀👾🤖🧠",              # emoji (wide + color)
        "漢字日本語中文한국어テスト표시",                    # CJK / Hangul (wide)
        "ĂÂÊÔƠƯ áàảãạ é̀ë̈õ̃",                             # combining marks
        "─│┌┐└┘├┤┬┴┼╔╗╚╝║═╠╣╦╩╬▏▎▍▌▋▊▉█",           # box + block
        "ᚠᚢᚦᚨᚱᚲ ελληνικά кириллица العربية עברית",     # scripts
    ]
    n = 0
    t0 = time.time()
    W(CSI + "2J" + CSI + "H")
    while time.time() - t0 < seconds:
        line = "".join(random.choice(random.choice(pools)) for _ in range(200)) + "\n"
        b = line.encode()
        os.write(1, b)
        n += len(b)
    rate_line("unicode", n, time.time() - t0)

def altscreen(iterations=2000):
    """Alt-screen thrash: enter/leave the alternate buffer as fast as possible."""
    banner(f"ALT-SCREEN THRASH — {iterations} enter/leave cycles")
    n = 0
    t0 = time.time()
    for i in range(iterations):
        seq = CSI + "?1049h" + CSI + "2J" + CSI + "H" + f"alt buffer frame {i}" + CSI + "?1049l"
        b = seq.encode()
        os.write(1, b); n += len(b)
    rate_line("altscreen", n, time.time() - t0, extra=f"{iterations} cycles")

def scrollback(lines=200000):
    """Scrollback stress: emit a huge number of lines fast (ledger/trim path)."""
    banner(f"SCROLLBACK — {lines} lines as fast as possible")
    n = 0
    t0 = time.time()
    block = "".join(f"\x1b[38;5;{16 + (i % 216)}mline {i:>8} " + "·" * 40 + "\n"
                    for i in range(1000))
    reps = max(1, lines // 1000)
    for _ in range(reps):
        b = block.encode()
        os.write(1, b); n += len(b)
    rate_line("scrollback", n, time.time() - t0, extra=f"{reps*1000} lines")

def malformed(seconds=2.0):
    """Adversarial garbage: malformed/pathological escapes + random bytes. Survive!"""
    banner(f"MALFORMED ESCAPES — adversarial garbage for {seconds:.0f}s (must not crash)")
    evil = [
        CSI + "999999999999999999m",         # absurd param
        CSI + "1;2;3;4;5;6;7;8;9;10;11;12;13;14;15m",  # many params
        CSI + "?" * 500 + "h",               # intermediate flood
        "\x1b]0;" + "A" * 5000,              # unterminated OSC title
        "\x1b]8;;http://" + "x" * 4000 + "\x1b\\",     # giant OSC 8 link
        CSI + "38;2;",                       # truncated truecolor
        "\x1bP" + "q" * 2000,               # unterminated DCS
        CSI + "r" + CSI + "999;999r",       # weird scroll region
        "\x1b" * 200,                        # ESC storm
    ]
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        parts = [random.choice(evil) for _ in range(50)]
        parts.append(bytes(random.randint(0, 255) for _ in range(2000)).decode("latin1"))
        s = "".join(parts).encode("latin1", "replace")
        os.write(1, s); n += len(s)
    # a clean reset so the terminal recovers visibly
    os.write(1, (CSI + "!p" + CSI + "0m" + CSI + "2J" + CSI + "H").encode())
    rate_line("malformed", n, time.time() - t0, extra="survived ✓")

def paint(seconds=3.0):
    """Full-frame repaint: clear + redraw the whole screen every frame (render load)."""
    banner(f"FULL REPAINT — clear+fill every frame for {seconds:.0f}s")
    cols, rows = size()
    frames = 0
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        c = 16 + (frames % 216)
        s = CSI + "H" + "".join((f"\x1b[48;5;{c}m" + " " * cols) for _ in range(rows - 1))
        b = s.encode()
        os.write(1, b); n += len(b); frames += 1
    fps = frames / (time.time() - t0)
    rate_line("paint", n, time.time() - t0, extra=f"{fps:.0f} frames/s")

SCENARIOS = {
    "flood": flood,
    "sgr": sgr,
    "cursor": cursor,
    "unicode": unicode_test,
    "altscreen": altscreen,
    "scrollback": scrollback,
    "malformed": malformed,
    "paint": paint,
}
ORDER = ["flood", "sgr", "paint", "cursor", "unicode", "scrollback", "altscreen", "malformed"]

def run_named(names):
    for name in names:
        fn = SCENARIOS.get(name)
        if not fn:
            W(f"unknown scenario: {name}\n"); continue
        try:
            fn()
        except Exception as e:  # a scenario must never take the harness down
            W(CSI + "0m" + CSI + "1;31m" + f"  scenario '{name}' raised: {e}\n" + CSI + "0m")
        time.sleep(0.4)
    W(CSI + "0m\n" + CSI + "1;38;5;46m" + "  ✓ torture complete\n" + CSI + "0m")

def menu():
    while True:
        reset()
        W(CSI + "2J" + CSI + "H")
        W(CSI + "1;38;5;213m" + "  hand torture harness\n\n" + CSI + "0m")
        for i, name in enumerate(ORDER, 1):
            W(f"   {i}) {name}\n")
        W("   a) ALL\n   q) quit\n\n  choose > ")
        try:
            choice = sys.stdin.readline().strip().lower()
        except KeyboardInterrupt:
            break
        if choice in ("q", "quit", ""):
            break
        elif choice in ("a", "all"):
            run_named(ORDER)
            W("\n  press Enter to continue…"); sys.stdin.readline()
        elif choice.isdigit() and 1 <= int(choice) <= len(ORDER):
            run_named([ORDER[int(choice) - 1]])
            W("\n  press Enter to continue…"); sys.stdin.readline()
    reset()

def main():
    argv = sys.argv[1:]
    try:
        if not argv:
            menu()
        elif argv[0] in ("all", "-a", "--all"):
            run_named(ORDER)
        else:
            run_named(argv)
    finally:
        reset()

if __name__ == "__main__":
    main()
