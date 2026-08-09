#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# bench.py — an APPLES-TO-APPLES terminal benchmark you can run in ANY terminal
# (hand, iTerm2, Terminal.app, kitty, alacritty, …) and compare the numbers.
#
#     ./build/hand -e python3 tools/bench.py     # measure hand
#     ./build/hand -e python3 tools/bench.py --reps 4   # 4x longer per workload
#     ./build/hand -e python3 tools/bench.py --rounds 3 # 3 rounds, keep the best
#     python3 tools/bench.py                      # measure whatever term you're in
#
# ── why this is a REAL measurement, not a lie ────────────────────────────────
# Naively timing os.write() measures how fast bytes leave into the PTY buffer —
# NOT how fast the terminal draws them. A terminal can look "infinitely fast" if
# it just buffers. So after each workload we force a SYNC: send a Device Status
# Report (ESC[6n) and BLOCK reading stdin until the terminal replies. That reply
# is only produced once the terminal has parsed everything queued before it — so
# the elapsed time is the true end-to-end "time to process + render N bytes".
# This is the same method vtebench / the alacritty benchmarks use.
#
# Every terminal runs the identical byte stream, so MB/s is directly comparable.

import os
import sys
import time
import termios
import tty
import select

OUT = sys.stdout.fileno()
IN = sys.stdin.fileno()
CSI = "\x1b["


def supports_sync():
    return os.isatty(IN) and os.isatty(OUT)


class RawTTY:
    """Put the tty in raw mode so the DSR reply arrives byte-for-byte, unbuffered."""
    def __enter__(self):
        self.saved = termios.tcgetattr(IN)
        tty.setraw(IN)
        return self
    def __exit__(self, *a):
        termios.tcsetattr(IN, termios.TCSADRAIN, self.saved)


def sync():
    """Round-trip a cursor-position report; returns only after the term processed
    everything written before it. Times out at 5s so a term that ignores DSR
    doesn't hang the bench (it just yields an inflated number, flagged below)."""
    os.write(OUT, (CSI + "6n").encode())
    buf = b""
    deadline = time.time() + 5.0
    while time.time() < deadline:
        r, _, _ = select.select([IN], [], [], deadline - time.time())
        if not r:
            return False
        buf += os.read(IN, 64)
        if buf.endswith(b"R"):  # ESC[<row>;<col>R
            return True
    return False


def write_all(data: bytes):
    mv = memoryview(data)
    off = 0
    while off < len(mv):
        off += os.write(OUT, mv[off:])


def measure(name, data: bytes, reps: int):
    """Write `data` `reps` times, sync, return (MB, seconds, MB/s)."""
    total = len(data) * reps
    # warm up (fault in pages, JIT the term's fast path) — not timed
    write_all(data)
    sync()
    t0 = time.perf_counter()
    for _ in range(reps):
        write_all(data)
    ok = sync()
    dt = time.perf_counter() - t0
    mb = total / 1e6
    rate = mb / dt if dt > 0 else 0.0
    return mb, dt, rate, ok


# ── workloads (identical bytes for every terminal) ───────────────────────────

def wl_ascii():
    line = (b"The quick brown fox jumps over the lazy dog 0123456789 " * 2)[:100] + b"\r\n"
    return line * 500, 400  # ~20 MB total

def wl_scroll():
    # dense scrolling: forces the terminal to shift the whole grid every line
    return (b"x" * 200 + b"\r\n") * 1000, 300  # ~60 MB

def wl_sgr():
    # a fresh truecolor SGR before every glyph — worst case for attribute handling
    cells = []
    for i in range(2000):
        r, g, b = (i * 7) & 255, (i * 13) & 255, (i * 29) & 255
        cells.append(f"\x1b[38;2;{r};{g};{b}m#".encode())
    return b"".join(cells) + b"\r\n", 300

def wl_cursor():
    # random-ish absolute cursor moves: stresses grid addressing, not scrolling
    parts = []
    for i in range(3000):
        y = (i * 7) % 24 + 1
        x = (i * 11) % 80 + 1
        parts.append(f"\x1b[{y};{x}H*".encode())
    return b"".join(parts), 300

def wl_unicode():
    # mixed wide/CJK/emoji: exercises width tables + glyph fallback
    s = ("漢字カナ한글 café ĝĥ ▓▒░ →★ 🚀🔥 " * 30 + "\r\n") * 60
    return s.encode(), 200

def wl_clear():
    # clear + repaint churn
    return (CSI + "2J" + CSI + "H" + ("y" * 80 + "\r\n") * 24).encode(), 3000


WORKLOADS = [
    ("ascii    ", wl_ascii),
    ("scroll   ", wl_scroll),
    ("sgr-color", wl_sgr),
    ("cursor   ", wl_cursor),
    ("unicode  ", wl_unicode),
    ("clear    ", wl_clear),
]


def term_name():
    for k in ("TERM_PROGRAM", "TERM"):
        v = os.environ.get(k)
        if v:
            return v
    return "unknown"


def run():
    if not supports_sync():
        sys.stderr.write("bench: needs an interactive terminal (a real tty).\n"
                         "Run it inside the terminal you want to measure.\n")
        return 1

    # Scale the workload for a LONGER, steadier run:
    #   --reps N / BENCH_REPS=N  multiplies every workload's rep count (default 1)
    #   --rounds R / BENCH_ROUNDS=R  runs the whole suite R times, keeping the
    #                                best (fastest) throughput per workload
    mult = float(os.environ.get("BENCH_REPS", "1") or "1")
    rounds = int(os.environ.get("BENCH_ROUNDS", "1") or "1")
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] in ("--reps", "-r") and i + 1 < len(argv):
            mult = float(argv[i + 1]); i += 2; continue
        if argv[i] in ("--rounds", "-R") and i + 1 < len(argv):
            rounds = int(argv[i + 1]); i += 2; continue
        i += 1
    mult = max(0.1, mult)
    rounds = max(1, rounds)

    name = term_name()
    # best[label] = (mb, dt, rate, ok) keeping the fastest round.
    best = {}
    with RawTTY():
        for _round in range(rounds):
            os.write(OUT, (CSI + "0m" + CSI + "2J" + CSI + "H").encode())
            for label, make in WORKLOADS:
                data, reps = make()
                reps = max(1, int(reps * mult))
                mb, dt, rate, ok = measure(label, data, reps)
                if label not in best or rate > best[label][2]:
                    best[label] = (mb, dt, rate, ok)
        os.write(OUT, (CSI + "0m" + CSI + "2J" + CSI + "H").encode())
    results = [(label, *best[label]) for label, _ in WORKLOADS]

    # results table (raw mode is off now, normal printing)
    print(f"\n  ── terminal benchmark ──  ({name})")
    print(f"  {'workload':10}  {'data':>9}  {'time':>8}  {'throughput':>13}")
    print("  " + "─" * 46)
    total_mb = 0.0
    total_s = 0.0
    for label, mb, dt, rate, ok in results:
        flag = "" if ok else "  ⚠ no DSR (inflated)"
        print(f"  {label:10}  {mb:7.1f}MB  {dt:6.2f}s  {rate:9.1f} MB/s{flag}")
        total_mb += mb
        total_s += dt
    agg = total_mb / total_s if total_s > 0 else 0.0
    print("  " + "─" * 46)
    print(f"  {'TOTAL':10}  {total_mb:7.1f}MB  {total_s:6.2f}s  {agg:9.1f} MB/s")
    print("\n  (run the same in iTerm2 / Terminal.app / kitty and compare)\n")

    # Optional: also append a machine-readable line to a file, so a GUI terminal
    # (whose window we can't scrape) can report its numbers to a script.
    out_path = os.environ.get("BENCH_OUT")
    if out_path:
        try:
            with open(out_path, "a") as f:
                f.write(f"{name}\tTOTAL\t{total_mb:.1f}\t{total_s:.3f}\t{agg:.1f}\n")
                for label, mb, dt, rate, ok in results:
                    f.write(f"{name}\t{label.strip()}\t{mb:.1f}\t{dt:.3f}\t{rate:.1f}\n")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(run())
    except KeyboardInterrupt:
        os.write(OUT, (CSI + "0m").encode())
        sys.exit(130)
