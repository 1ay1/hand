#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# compare.py — merge bench.py results from several terminals into one table.
#
# Workflow (run bench in EACH terminal you want to compare; they all append to
# the same TSV via BENCH_OUT):
#
#     rm -f /tmp/termbench.tsv
#     BENCH_OUT=/tmp/termbench.tsv ./build/hand -e python3 tools/bench.py
#     # then, INSIDE iTerm2:      BENCH_OUT=/tmp/termbench.tsv python3 tools/bench.py
#     # then, INSIDE Terminal.app: BENCH_OUT=/tmp/termbench.tsv python3 tools/bench.py
#     python3 tools/compare.py /tmp/termbench.tsv
#
# Prints MB/s per workload per terminal, side by side, with the fastest bolded.

import sys
from collections import defaultdict

WORKLOAD_ORDER = ["ascii", "scroll", "sgr-color", "cursor", "unicode", "clear", "TOTAL"]
BOLD = "\x1b[1;38;5;46m"
DIM = "\x1b[2m"
RST = "\x1b[0m"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/termbench.tsv"
    # rate[workload][term] = MB/s   (last run wins if a term ran twice)
    rate = defaultdict(dict)
    terms = []
    try:
        for line in open(path):
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 5:
                continue
            term, workload, mb, secs, mbps = parts
            rate[workload][term] = float(mbps)
            if term not in terms:
                terms.append(term)
    except FileNotFoundError:
        print(f"no results file at {path}. Run bench.py with BENCH_OUT set first.")
        return 1

    if not terms:
        print("no results parsed."); return 1

    # header
    tw = max(9, max(len(t) for t in terms))
    print()
    print(f"  terminal throughput comparison  (MB/s, higher = faster)\n")
    hdr = f"  {'workload':10}" + "".join(f"  {t:>{tw}}" for t in terms)
    print(hdr)
    print("  " + "─" * (len(hdr) - 2))

    for wl in WORKLOAD_ORDER:
        if wl not in rate:
            continue
        row = rate[wl]
        best = max(row.values()) if row else 0
        cells = ""
        for t in terms:
            v = row.get(t)
            if v is None:
                cells += f"  {'-':>{tw}}"
            elif v == best and len(row) > 1:
                cells += f"  {BOLD}{v:>{tw}.1f}{RST}"
            else:
                cells += f"  {v:>{tw}.1f}"
        sep = "  " + "─" * (len(hdr) - 2) if wl == "TOTAL" else ""
        if wl == "TOTAL":
            print(sep)
        label = f"{DIM}{wl:10}{RST}" if wl == "TOTAL" else f"{wl:10}"
        print(f"  {label}{cells}")

    # verdict on TOTAL
    if "TOTAL" in rate and len(rate["TOTAL"]) > 1:
        ranked = sorted(rate["TOTAL"].items(), key=lambda kv: kv[1], reverse=True)
        winner, wv = ranked[0]
        runner, rv = ranked[1]
        ratio = wv / rv if rv else 0
        print(f"\n  → fastest overall: {BOLD}{winner}{RST} "
              f"({wv:.1f} MB/s, {ratio:.2f}x vs {runner})\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
