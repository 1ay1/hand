#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# torture.py — a BRUTAL, visible stress harness for `hand`. Run it INSIDE the
# terminal:
#
#     ./build/hand -e python3 tools/torture.py            # interactive menu
#     ./build/hand -e python3 tools/torture.py all        # run every scenario
#     ./build/hand -e python3 tools/torture.py flood sgr   # named scenarios
#     ./build/hand -e python3 tools/torture.py all --time 20   # 20s per scenario
#     ./build/hand -e python3 tools/torture.py all --brutal    # crank everything
#     ./build/hand -e python3 tools/torture.py all --latency   # + measure latency
#
# Each scenario hammers a different part of the VT/screen/renderer pipeline and
# prints a live THROUGHPUT + LATENCY readout so you can WATCH the terminal cope
# (or not). Latency is real end-to-end: after a workload we round-trip a Device
# Status Report (ESC[6n) and time how long until the terminal replies — that
# only happens once it has parsed + rendered everything queued before it (the
# vtebench / alacritty method). Nothing here is hand-specific — it's pure ANSI,
# so you can point it at Terminal.app / iTerm / kitty for a side-by-side.

import os
import sys
import time
import random
import shutil
import select
import termios
import tty

W = lambda s: (sys.stdout.write(s), sys.stdout.flush())
CSI = "\x1b["
OUT = sys.stdout.fileno()
IN = sys.stdin.fileno()

# Global duration scale, set from --time / --brutal. Every scenario multiplies
# its base seconds / iterations by this so "run longer" is one knob.
DUR = 8.0          # default seconds per timed scenario (was 2-3s)
SCALE = 1.0        # iteration multiplier for count-based scenarios
BRUTAL = False     # extra-nasty variants
LATENCY = False    # measure end-to-end latency via DSR (opt-in: --latency)


def size():
    try:
        c = shutil.get_terminal_size((80, 24))
        return c.columns, c.lines
    except Exception:
        return 80, 24

def reset():
    W(CSI + "0m" + CSI + "?25h" + CSI + "?1049l" + CSI + "?7h" + CSI + "?2026l")

def banner(title):
    cols, _ = size()
    line = "─" * max(0, cols)
    W(CSI + "0m\n" + CSI + "1;38;5;39m" + line + "\n" + f" ▶ {title}\n" + line + CSI + "0m\n")


# ── latency probe (DSR round-trip) ───────────────────────────────────────────
# Only works on a real tty. Measures true "time until the terminal has processed
# everything written so far". We sample it repeatedly to get a distribution.

def _tty_ok():
    return os.isatty(IN) and os.isatty(OUT)

def _dsr_once(timeout=1.0):
    """Send ESC[6n, block until the reply; return round-trip seconds or None.
    Bounded by `timeout` so a non-responsive terminal can NEVER hang us."""
    # drain any pending input first so a stale reply doesn't skew us
    while select.select([IN], [], [], 0)[0]:
        try: os.read(IN, 4096)
        except OSError: break
    t0 = time.perf_counter()
    os.write(OUT, (CSI + "6n").encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([IN], [], [], deadline - time.time())
        if not r:
            return None
        try: buf += os.read(IN, 64)
        except OSError: return None
        if b"R" in buf:
            return time.perf_counter() - t0
    return None

def latency_after(write_fn, samples=40):
    """Run write_fn() then probe latency `samples` times; return (p50,p99,max) ms
    or None if not on a tty. write_fn is the per-sample load whose processing
    latency we're measuring. Bails early if the terminal stops replying so it
    can never wedge the harness."""
    if not LATENCY or not _tty_ok():
        return None
    # Probe ONCE up front: if the terminal doesn't answer DSR at all (or this
    # isn't a real interactive session), skip latency entirely rather than
    # eating `samples` timeouts.
    if _dsr_once() is None:
        return None
    xs = []
    misses = 0
    for _ in range(samples):
        write_fn()
        dt = _dsr_once()
        if dt is not None:
            xs.append(dt * 1000.0)
        else:
            misses += 1
            if misses >= 2:  # terminal stopped answering; stop probing
                break
    if not xs:
        return None
    xs.sort()
    p = lambda q: xs[min(len(xs) - 1, int(q * len(xs)))]
    return (p(0.50), p(0.99), xs[-1])


def rate_line(label, nbytes, secs, extra="", lat=None):
    mb = nbytes / 1e6
    rate = mb / secs if secs > 0 else 0.0
    s = (CSI + "0m" + f"  {label}: {mb:8.1f} MB in {secs:6.2f}s  =  "
         + CSI + "1;38;5;46m" + f"{rate:8.1f} MB/s" + CSI + "0m")
    if extra:
        s += f"   {extra}"
    if lat:
        p50, p99, mx = lat
        s += (CSI + "0m" + "   lat "
              + CSI + "1;38;5;220m" + f"p50 {p50:.1f}ms p99 {p99:.1f}ms max {mx:.1f}ms" + CSI + "0m")
    W(s + "\n")


def _t(base):
    """A timed scenario's duration in seconds (scaled by --time)."""
    return base * (DUR / 8.0)

def _n(base):
    """A count-based scenario's iterations (scaled by --brutal/--time)."""
    return int(base * SCALE)


# ── scenarios ────────────────────────────────────────────────────────────────

def flood(seconds=None):
    """Raw print flood: how many bytes/sec can the parser+screen+renderer eat."""
    seconds = seconds or _t(8.0)
    banner(f"FLOOD — raw text at max rate for {seconds:.0f}s")
    chunk = ("The quick brown fox jumps over the lazy dog. " * 20 + "\n") * 40
    data = chunk.encode()
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        os.write(1, data)
        n += len(data)
    lat = latency_after(lambda: os.write(1, data))
    rate_line("flood", n, time.time() - t0, lat=lat)

def sgr(seconds=None):
    """SGR color storm: every cell a different truecolor fg/bg — kills naive attr caches."""
    seconds = seconds or _t(8.0)
    banner(f"SGR STORM — full 24-bit color churn for {seconds:.0f}s")
    cols, rows = size()
    def frame():
        buf = [CSI + "H"]
        for _ in range(rows - 1):
            for _ in range(cols):
                r = random.randint(0, 255); g = random.randint(0, 255); b = random.randint(0, 255)
                buf.append(f"\x1b[38;2;{r};{g};{b};48;2;{255-r};{255-g};{255-b}m█")
        return "".join(buf).encode()
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        s = frame(); os.write(1, s); n += len(s)
    lat = latency_after(lambda: os.write(1, frame()), samples=20)
    rate_line("sgr", n, time.time() - t0, lat=lat)

def cursor(seconds=None):
    """Cursor-warp storm: random absolute moves + writes — stresses grid addressing."""
    seconds = seconds or _t(8.0)
    banner(f"CURSOR WARP — random addressing for {seconds:.0f}s")
    cols, rows = size()
    glyphs = "@#%&*█▓▒░◆●▲"
    hits = _n(8000) if BRUTAL else _n(4000)
    def frame():
        buf = []
        for _ in range(hits):
            y = random.randint(1, max(1, rows - 1)); x = random.randint(1, cols)
            col = random.randint(16, 231)
            buf.append(f"\x1b[{y};{x}H\x1b[38;5;{col}m{random.choice(glyphs)}")
        return "".join(buf).encode()
    n = 0
    t0 = time.time()
    W(CSI + "2J")
    while time.time() - t0 < seconds:
        s = frame(); os.write(1, s); n += len(s)
    lat = latency_after(lambda: os.write(1, frame()), samples=20)
    rate_line("cursor", n, time.time() - t0, lat=lat)

def unicode_test(seconds=None):
    """Unicode / emoji / CJK / combining marks / box-drawing — fallback-chain + width."""
    seconds = seconds or _t(8.0)
    banner(f"UNICODE STORM — emoji · CJK · combining · box for {seconds:.0f}s")
    pools = [
        "🔥💥🚀🎉😀🌈🦄🍕🎸⚡️🌟💀👾🤖🧠🫠🥶👺🎃💩",         # emoji (wide + color)
        "漢字日本語中文한국어テスト表示濫觴驫麤龘",              # CJK / Hangul (wide)
        "ĂÂÊÔƠƯ áàảãạ é̀ë̈õ̃ ǫ̷̢̛̳a͖̽",                       # combining marks (zalgo)
        "─│┌┐└┘├┤┬┴┼╔╗╚╝║═╠╣╦╩╬▏▎▍▌▋▊▉█",           # box + block
        "ᚠᚢᚦᚨᚱᚲ ελληνικά кириллица العربية עברית",     # scripts (incl. RTL)
        "\u200b\u200d\u2060\ufe0f\u0301",              # ZWSP/ZWJ/word-joiner/VS16
    ]
    def line():
        return ("".join(random.choice(random.choice(pools)) for _ in range(200)) + "\n").encode()
    n = 0
    t0 = time.time()
    W(CSI + "2J" + CSI + "H")
    while time.time() - t0 < seconds:
        b = line(); os.write(1, b); n += len(b)
    lat = latency_after(lambda: os.write(1, line()))
    rate_line("unicode", n, time.time() - t0, lat=lat)

def altscreen(iterations=None):
    """Alt-screen thrash: enter/leave the alternate buffer as fast as possible."""
    iterations = iterations or _n(8000)
    banner(f"ALT-SCREEN THRASH — up to {iterations} enter/leave cycles")
    n = 0
    i = 0
    t0 = time.time()
    budget = _t(6.0)
    while i < iterations and time.time() - t0 < budget:
        seq = CSI + "?1049h" + CSI + "2J" + CSI + "H" + f"alt buffer frame {i}" + CSI + "?1049l"
        b = seq.encode()
        os.write(1, b); n += len(b); i += 1
    lat = latency_after(lambda: os.write(1, (CSI+"?1049h"+CSI+"?1049l").encode()), samples=30)
    rate_line("altscreen", n, time.time() - t0, extra=f"{i} cycles", lat=lat)

def scrollback(lines=None):
    """Scrollback stress: emit a huge number of lines fast (ledger/trim path)."""
    lines = lines or _n(1000000)
    banner(f"SCROLLBACK — up to {lines} lines as fast as possible")
    n = 0
    t0 = time.time()
    budget = _t(6.0)
    block = "".join(f"\x1b[38;5;{16 + (i % 216)}mline {i:>8} " + "·" * 40 + "\n"
                    for i in range(1000))
    b = block.encode()
    reps = max(1, lines // 1000)
    done = 0
    for _ in range(reps):
        os.write(1, b); n += len(b); done += 1000
        if time.time() - t0 >= budget:
            break
    rate_line("scrollback", n, time.time() - t0, extra=f"{done} lines")

def malformed(seconds=None):
    """Adversarial garbage: malformed/pathological escapes + random bytes. Survive!"""
    seconds = seconds or _t(6.0)
    banner(f"MALFORMED ESCAPES — adversarial garbage for {seconds:.0f}s (must not crash)")
    evil = [
        CSI + "999999999999999999m",         # absurd param
        CSI + "1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20m",  # many params
        CSI + "?" * 2000 + "h",              # intermediate flood
        "\x1b]0;" + "A" * 20000,             # unterminated OSC title
        "\x1b]8;;http://" + "x" * 16000 + "\x1b\\",     # giant OSC 8 link
        CSI + "38;2;",                       # truncated truecolor
        "\x1bP" + "q" * 8000,               # unterminated DCS
        CSI + "r" + CSI + "999;999r",       # weird scroll region
        "\x1b" * 1000,                       # ESC storm
        CSI + ";;;;;;;;;;;;;;;;;;;;;;m",     # empty params
        "\x1b[" + "9" * 400 + "H",          # gigantic cursor address
        "\x1b_G" + "z" * 8000,             # unterminated APC (kitty graphics-ish)
        "\xc0\x80\xed\xa0\x80\xf4\x90\x80\x80",  # invalid UTF-8 sequences
    ]
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        parts = [random.choice(evil) for _ in range(80)]
        parts.append(bytes(random.randint(0, 255) for _ in range(4000)).decode("latin1"))
        s = "".join(parts).encode("latin1", "replace")
        os.write(1, s); n += len(s)
    os.write(1, (CSI + "!p" + CSI + "0m" + CSI + "2J" + CSI + "H").encode())
    lat = latency_after(lambda: os.write(1, (CSI+"0m").encode()), samples=20)
    rate_line("malformed", n, time.time() - t0, extra="survived ✓", lat=lat)

def paint(seconds=None):
    """Full-frame repaint: clear + redraw the whole screen every frame (render load)."""
    seconds = seconds or _t(8.0)
    banner(f"FULL REPAINT — clear+fill every frame for {seconds:.0f}s")
    cols, rows = size()
    def frame(fr):
        c = 16 + (fr % 216)
        return (CSI + "H" + "".join((f"\x1b[48;5;{c}m" + " " * cols) for _ in range(rows - 1))).encode()
    frames = 0
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        b = frame(frames); os.write(1, b); n += len(b); frames += 1
    fps = frames / (time.time() - t0)
    lat = latency_after(lambda: os.write(1, frame(random.randint(0, 215))), samples=20)
    rate_line("paint", n, time.time() - t0, extra=f"{fps:.0f} frames/s", lat=lat)


# ── NEW brutal scenarios ─────────────────────────────────────────────────────

def mixed(seconds=None):
    """EVERYTHING AT ONCE: interleave colour, cursor warps, unicode, scroll, and
    synchronized-output frames — the realistic worst case, all pipelines hot."""
    seconds = seconds or _t(10.0)
    banner(f"MIXED STORM — all pipelines interleaved for {seconds:.0f}s")
    cols, rows = size()
    emoji = "🔥🚀💥⚡🌈"
    def frame():
        buf = [CSI + "?2026h"]  # begin synchronized update
        for _ in range(300):
            y = random.randint(1, max(1, rows - 1)); x = random.randint(1, cols)
            r = random.randint(0, 255); g = random.randint(0, 255); b = random.randint(0, 255)
            ch = random.choice(emoji) if random.random() < 0.15 else chr(random.randint(33, 126))
            buf.append(f"\x1b[{y};{x}H\x1b[38;2;{r};{g};{b}m{ch}")
        buf.append(CSI + "?2026l")  # end synchronized update (atomic present)
        return "".join(buf).encode()
    n = 0
    t0 = time.time()
    W(CSI + "2J")
    while time.time() - t0 < seconds:
        s = frame(); os.write(1, s); n += len(s)
    lat = latency_after(lambda: os.write(1, frame()), samples=25)
    rate_line("mixed", n, time.time() - t0, lat=lat)

def bigpaste(seconds=None):
    """One COLOSSAL write per iteration (10-40 MB): stresses the PTY read loop,
    chunk coalescing, and back-pressure. Simulates pasting a huge log."""
    seconds = seconds or _t(6.0)
    banner(f"BIG PASTE — multi-MB single writes for {seconds:.0f}s")
    mb = 40 if BRUTAL else 16
    blob = (("paste line " + "=" * 100 + "\n") * (mb * 10000 // 112)).encode()
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        os.write(1, blob); n += len(blob)
    lat = latency_after(lambda: os.write(1, blob), samples=8)
    rate_line("bigpaste", n, time.time() - t0, extra=f"{len(blob)//1_000_000}MB/write", lat=lat)

def widechar(seconds=None):
    """Wide-char boundary torture: CJK/emoji straddling the right margin, ZWJ
    families, VS16 selectors, and combining marks at column edges — width + wrap."""
    seconds = seconds or _t(6.0)
    banner(f"WIDECHAR BOUNDARY — wrap + width edge cases for {seconds:.0f}s")
    cols, _ = size()
    fams = ["👨‍👩‍👧‍👦", "👩🏾‍💻", "🧑‍🚀", "🏳️‍🌈", "❤️", "1️⃣", "🇯🇵", "가"]
    def line():
        # place wide glyphs so some land exactly on the last column
        parts = [CSI + "H"]
        for i in range(cols):
            if i % 3 == 0:
                parts.append(random.choice(fams))
            else:
                parts.append("漢")
        parts.append("\n")
        return "".join(parts).encode()
    n = 0
    t0 = time.time()
    W(CSI + "2J")
    while time.time() - t0 < seconds:
        b = line(); os.write(1, b); n += len(b)
    lat = latency_after(lambda: os.write(1, line()))
    rate_line("widechar", n, time.time() - t0, lat=lat)

def regions(seconds=None):
    """Scroll-region ping-pong: set tiny DECSTBM regions all over the screen and
    scroll them, thrashing the scroll/margin machinery."""
    seconds = seconds or _t(6.0)
    banner(f"SCROLL REGIONS — margin ping-pong for {seconds:.0f}s")
    cols, rows = size()
    def frame():
        buf = []
        for _ in range(200):
            top = random.randint(1, max(1, rows - 3))
            bot = min(rows, top + random.randint(1, 6))
            buf.append(f"\x1b[{top};{bot}r")            # DECSTBM
            buf.append(f"\x1b[{bot};1H")
            buf.append("\n" * random.randint(1, 5))     # scroll within region
            buf.append("scroll " + "-" * random.randint(4, cols - 8))
        buf.append(CSI + "r")  # reset region
        return "".join(buf).encode()
    n = 0
    t0 = time.time()
    W(CSI + "2J")
    while time.time() - t0 < seconds:
        s = frame(); os.write(1, s); n += len(s)
    os.write(1, (CSI + "r").encode())
    lat = latency_after(lambda: os.write(1, frame()), samples=20)
    rate_line("regions", n, time.time() - t0, lat=lat)

def links(seconds=None):
    """OSC 8 hyperlink flood: every cell its own link id — stresses the link
    table + per-cell attribute storage."""
    seconds = seconds or _t(6.0)
    banner(f"HYPERLINK FLOOD — OSC 8 per cell for {seconds:.0f}s")
    def line(i):
        parts = [CSI + "H"]
        for j in range(200):
            uri = f"https://example.com/{i}/{j}/" + "p" * random.randint(0, 40)
            parts.append(f"\x1b]8;id={i}-{j};{uri}\x1b\\link\x1b]8;;\x1b\\ ")
        parts.append("\n")
        return "".join(parts).encode()
    n = 0
    i = 0
    t0 = time.time()
    W(CSI + "2J")
    while time.time() - t0 < seconds:
        b = line(i); os.write(1, b); n += len(b); i += 1
    lat = latency_after(lambda: os.write(1, line(random.randint(0, 9999))))
    rate_line("links", n, time.time() - t0, lat=lat)

def damage(seconds=None):
    """Damage worst-case: flip ONE random cell per frame across the whole grid,
    forcing a full-grid diff every frame for a single-cell change."""
    seconds = seconds or _t(6.0)
    banner(f"DAMAGE WORST-CASE — 1 cell/frame, full grid for {seconds:.0f}s")
    cols, rows = size()
    # paint a full grid first
    W(CSI + "2J" + CSI + "H")
    W(("".join("x" * cols + "\n" for _ in range(rows - 1))))
    def frame():
        y = random.randint(1, max(1, rows - 1)); x = random.randint(1, cols)
        c = random.randint(16, 231)
        return f"\x1b[{y};{x}H\x1b[38;5;{c}m@".encode()
    n = 0
    frames = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        b = frame(); os.write(1, b); n += len(b); frames += 1
    fps = frames / (time.time() - t0)
    lat = latency_after(frame, samples=60)
    rate_line("damage", n, time.time() - t0, extra=f"{fps:.0f} frames/s", lat=lat)

def keylatency(seconds=None):
    """Pure input-to-photon latency: write ONE char, sync, repeat. This is the
    number that decides whether typing feels instant. Reported as a distribution."""
    seconds = seconds or _t(4.0)
    banner(f"KEY LATENCY — single-char echo round-trip")
    if not LATENCY:
        W("  (latency probing is opt-in — re-run with --latency)\n"); return
    if not _tty_ok():
        W("  (needs a real tty — skipped)\n"); return
    W(CSI + "2J" + CSI + "H")
    xs = []
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds and len(xs) < 400:
        dt = _dsr_once()
        os.write(1, b"x"); n += 1
        d2 = _dsr_once()
        if d2 is not None:
            xs.append(d2 * 1000.0)
    if xs:
        xs.sort()
        p = lambda q: xs[min(len(xs) - 1, int(q * len(xs)))]
        rate_line("keylatency", n, time.time() - t0,
                  extra=f"{len(xs)} samples", lat=(p(0.50), p(0.99), xs[-1]))
    else:
        W("  no samples\n")


SCENARIOS = {
    "flood": flood,
    "sgr": sgr,
    "cursor": cursor,
    "unicode": unicode_test,
    "altscreen": altscreen,
    "scrollback": scrollback,
    "malformed": malformed,
    "paint": paint,
    "mixed": mixed,
    "bigpaste": bigpaste,
    "widechar": widechar,
    "regions": regions,
    "links": links,
    "damage": damage,
    "keylatency": keylatency,
}
ORDER = ["flood", "bigpaste", "sgr", "paint", "mixed", "cursor", "damage",
         "unicode", "widechar", "regions", "links", "scrollback", "altscreen",
         "keylatency", "malformed"]

def run_named(names):
    grand = time.time()
    for name in names:
        fn = SCENARIOS.get(name)
        if not fn:
            W(f"unknown scenario: {name}\n"); continue
        try:
            fn()
        except Exception as e:  # a scenario must never take the harness down
            W(CSI + "0m" + CSI + "1;31m" + f"  scenario '{name}' raised: {e}\n" + CSI + "0m")
        time.sleep(0.3)
    W(CSI + "0m\n" + CSI + "1;38;5;46m"
      + f"  ✓ torture complete ({time.time()-grand:.0f}s total)\n" + CSI + "0m")

def menu():
    while True:
        reset()
        W(CSI + "2J" + CSI + "H")
        W(CSI + "1;38;5;213m" + f"  hand torture harness   (DUR={DUR:.0f}s"
          + (" BRUTAL" if BRUTAL else "") + ")\n\n" + CSI + "0m")
        for i, name in enumerate(ORDER, 1):
            doc = (SCENARIOS[name].__doc__ or "").strip().split("\n")[0]
            W(f"   {i:>2}) {name:11} {CSI}38;5;244m{doc[:48]}{CSI}0m\n")
        W("    a) ALL\n    q) quit\n\n  choose > ")
        try:
            choice = sys.stdin.readline()
        except KeyboardInterrupt:
            break
        if choice == "":  # EOF (piped / non-interactive) — don't spin forever
            break
        choice = choice.strip().lower()
        if choice in ("q", "quit"):
            break
        elif choice in ("a", "all"):
            run_named(ORDER)
            W("\n  press Enter to continue…"); sys.stdin.readline()
        elif choice.isdigit() and 1 <= int(choice) <= len(ORDER):
            run_named([ORDER[int(choice) - 1]])
            W("\n  press Enter to continue…"); sys.stdin.readline()
    reset()

def main():
    global DUR, SCALE, BRUTAL, LATENCY
    argv = sys.argv[1:]
    # parse flags: --time N / -t N, --brutal / -b, --latency / -l
    names = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--time", "-t") and i + 1 < len(argv):
            DUR = float(argv[i + 1]); i += 2; continue
        if a in ("--brutal", "-b"):
            BRUTAL = True; i += 1; continue
        if a in ("--latency", "-l"):
            LATENCY = True; i += 1; continue
        names.append(a); i += 1
    if BRUTAL:
        DUR = max(DUR, 20.0)
        SCALE = 3.0
    else:
        # scale count-based scenarios with the time knob too
        SCALE = max(1.0, DUR / 8.0)

    try:
        if not names:
            menu()
        elif names[0] in ("all", "-a", "--all"):
            run_named(ORDER)
        else:
            run_named(names)
    finally:
        reset()

if __name__ == "__main__":
    main()
