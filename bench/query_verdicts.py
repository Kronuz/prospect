#!/usr/bin/env python3
"""Phase 3's query verdicts per command and class over gate.py trials, with the binding engine.

  query_verdicts.py [--prospect NAME] [--as ENGINE=NAME]... TRIAL [TRIAL...]

TRIAL is RESULTS_JSON[+RESULTS_JSON...], as for bench/phase2/gate.py, whose raw ratio and
interval it uses. pass: the interval's upper bound is at most 1.0x; miss: its lower bound is
above 1.0x; straddle: otherwise. A pass whose binding engine (the most common slowest-ratio
engine over trials) is a JVM one and whose median is above 0.90x is "caveat": it beats Lucene
by less than the warm-up allowance (docs/perf/format13-gate-vm.md, "The warm-up").
"""

from __future__ import annotations

import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "phase2"))
import gate

ORDER = ["TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT", "COUNT"]


def main(argv: list[str]) -> int:
    prospect, alias, trials = "prospect", {}, []
    while argv:
        a, argv = argv[0], argv[1:]
        if a == "--prospect":
            prospect, argv = argv[0], argv[1:]
        elif a == "--as":
            e, _, n = argv[0].partition("=")
            alias[e], argv = n, argv[1:]
        else:
            trials.append(a)
    rs = [gate.load(t, prospect, alias) for t in trials]
    floor = min(gate.run_floor(r) for r in rs)
    per: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for r in rs:
        rows, mism = gate.trial(r, floor)
        bad = {c: m for c, m in mism.items() if any(m.values())}
        if bad:
            print(f"count mismatches: {bad}")
        for k, row in rows.items():
            per[k].append(row)
    tally: Counter[str] = Counter()
    for cmd, tag in sorted(per, key=lambda k: (ORDER.index(k[0]), k[1])):
        rows = per[(cmd, tag)]
        med, lo, hi = gate.interval([row["raw"] for row in rows])
        fast = [e for e in gate.FAST if e in rows[0]]
        bind = Counter(max(fast, key=lambda e: row[e][0]) for row in rows).most_common(
            1
        )[0][0]
        if hi <= 1.0:
            v = "caveat" if bind.startswith("lucene") and med > 0.90 else "pass"
        elif lo > 1.0:
            v = "miss"
        else:
            v = "straddle"
        tally[v] += 1
        print(f"{cmd:14} {tag:19} {med:.3f} [{lo:.3f}-{hi:.3f}] {bind:18} {v}")
    print(f"{dict(tally)} over {len(rs)} trials, floor {floor} us")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
