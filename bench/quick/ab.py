#!/usr/bin/env python3
"""Scores quick/ab.sh output: per command and query set, the geometric mean over queries
of B's best time over A's (best over rounds), with a 95% bootstrap interval over queries,
the per-round ratios for spread, and result mismatches (counts must agree).

  ab.py OUT
"""

from __future__ import annotations

import glob
import math
import os
import random
import sys
from collections import defaultdict


def main(out: str) -> None:
    best: dict[tuple[str, str, str], dict[str, float]] = defaultdict(dict)
    rounds: dict[tuple[str, str, str, str], dict[str, float]] = {}
    results: dict[tuple[str, str, str], dict[str, str]] = defaultdict(dict)
    for path in glob.glob(os.path.join(out, "*.tsv")):
        r, side, cmd, qset = os.path.basename(path)[:-4].split("-", 3)
        times: dict[str, float] = {}
        with open(path) as f:
            for line in f:
                t, res, q = line.rstrip("\n").split("\t", 2)
                times[q] = float(t)
                b = best[(side, cmd, qset)]
                b[q] = min(b.get(q, math.inf), float(t))
                results[(side, cmd, qset)][q] = res
        rounds[(r, side, cmd, qset)] = times
    rng = random.Random(7)
    print("| Command | Set | n | B/A | 95% | Rounds | Mismatches |")
    print("|---|---|---:|---:|---|---|---:|")
    for cmd in ["TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT", "COUNT"]:
        for qset in ["queries", "terms"]:
            a, b = best.get(("a", cmd, qset)), best.get(("b", cmd, qset))
            if not a or not b:
                continue
            qs = sorted(set(a) & set(b))
            logs = [math.log(max(b[q], 0.1) / max(a[q], 0.1)) for q in qs]
            gm = math.exp(sum(logs) / len(logs))
            boot = sorted(
                math.exp(sum(rng.choice(logs) for _ in logs) / len(logs))
                for _ in range(1000)
            )
            per_round = []
            for r in sorted({k[0] for k in rounds}, key=int):
                ra, rb = (
                    rounds.get((r, "a", cmd, qset)),
                    rounds.get((r, "b", cmd, qset)),
                )
                if ra and rb:
                    per_round.append(
                        math.exp(
                            sum(math.log(max(rb[q], 0.1) / max(ra[q], 0.1)) for q in qs)
                            / len(qs)
                        )
                    )
            ra_, rb_ = results[("a", cmd, qset)], results[("b", cmd, qset)]
            mism = sum(1 for q in qs if ra_[q] != rb_[q])
            print(
                f"| `{cmd}` | {qset} | {len(qs)} | {gm:.3f} | {boot[25]:.3f}–{boot[974]:.3f} | {' '.join(f'{x:.2f}' for x in per_round)} | {mism} |"
            )


if __name__ == "__main__":
    main(sys.argv[1])
