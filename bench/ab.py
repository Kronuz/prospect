#!/usr/bin/env python3
"""Compares the two variants of bench/ab.sh over its trials.

  ab.py [--md] TRIAL [TRIAL...]      TRIAL: RESULTS_JSON[+RESULTS_JSON...]

Per trial, command, and class: the geometric mean over queries of strata-b / strata-a, a
query's time being its fastest run. Across trials: the median and the 95% bootstrap
interval of the median. Counts must match between the variants.
"""

import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "phase2"))
from gate import geo, interval, load


def main(argv):
    md = "--md" in argv
    ratios, mism = defaultdict(list), 0
    for path in (a for a in argv if a != "--md"):
        for cmd, per in load(path).items():
            a = {q["query"]: q for q in per["strata-a"]}
            b = {q["query"]: q for q in per["strata-b"]}
            mism += sum(1 for qs in a if a[qs]["count"] != b[qs]["count"])
            by = defaultdict(list)
            for qs, q in a.items():
                for tag in q["tags"]:
                    if ":" not in tag:
                        by[tag].append(qs)
            for tag, qs in by.items():
                ratios[(cmd, tag)].append(
                    geo([(min(b[x]["duration"]), min(a[x]["duration"])) for x in qs])
                )
    sep = " | " if md else "  "
    if md:
        print("| command | class | b / a | 95% interval |\n|---|---|---|---|")
    for (cmd, tag), rs in sorted(ratios.items()):
        med, lo, hi = interval(rs)
        cells = [cmd, tag, f"{med:.3f}", f"{lo:.3f}-{hi:.3f}"]
        print(
            ("| " + sep.join(cells) + " |")
            if md
            else sep.join(f"{c:<16}" for c in cells)
        )
    print(f"count mismatches: {mism}")


if __name__ == "__main__":
    main(sys.argv[1:])
