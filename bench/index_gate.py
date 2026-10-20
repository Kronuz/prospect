#!/usr/bin/env python3
"""Indexing ratchet rows over bench/index_trials.sh results (PLAN, "Indexing speed").

  index_gate.py DIR [PROSPECT_BUDGET_MB]

DIR holds i{t}-{engine}.time (GNU time -v) and .bytes per trial. Per engine: the wall
times, the median, the peak RSS (largest over trials), and the index bytes. Per trial,
prospect's throughput over the reference's is the reference's time over prospect's; the
reference is the engine with the highest median throughput among the others. Across
trials: the median ratio and the 95% bootstrap interval of the median. With a budget,
the memory row checks prospect's peak RSS against the budget plus 10%.
"""

import os
import random
import re
import statistics
import sys

# canon reads i{t}-strata.time, recorded before the rename, as prospect.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "phase2"))
from gate import canon


def seconds(text):
    parts = [float(p) for p in text.split(":")]
    total = 0.0
    for p in parts:
        total = total * 60 + p
    return total


def load(path):
    out = {}
    with open(path) as f:
        lines = f.readlines()
    for line in lines:
        if "Elapsed (wall clock)" in line:
            out["time"] = seconds(line.rsplit(" ", 1)[1].strip())
        elif "Maximum resident set size" in line:
            out["rss"] = int(line.rsplit(" ", 1)[1]) * 1024
    return out


def interval(values, reps=10000, seed=2026):
    rng = random.Random(seed)
    meds = sorted(
        statistics.median(rng.choices(values, k=len(values))) for _ in range(reps)
    )
    return (
        statistics.median(values),
        meds[int(0.025 * reps)],
        meds[int(0.975 * reps) - 1],
    )


def main(argv):
    root = argv[1]
    budget = int(argv[2]) << 20 if len(argv) > 2 else None
    runs = {}
    for name in os.listdir(root):
        m = re.fullmatch(r"i(\d+)-(.+)\.time", name)
        if m:
            row = load(os.path.join(root, name))
            size = os.path.join(root, name[:-5] + ".bytes")
            if os.path.exists(size):
                with open(size) as f:
                    row["bytes"] = int(f.read().split()[0])
            if "time" in row:
                runs.setdefault(canon(m.group(2)), {})[int(m.group(1))] = row
    if "prospect" not in runs:
        sys.exit("no prospect trials")
    print("| Engine | Wall time (s, per trial) | Median | Peak RSS | Index bytes |")
    print("|---|---|---|---|---|")
    medians = {}
    for e, trials in sorted(
        runs.items(),
        key=lambda kv: statistics.median(r["time"] for r in kv[1].values()),
    ):
        times = [trials[t]["time"] for t in sorted(trials)]
        medians[e] = statistics.median(times)
        rss = max(r.get("rss", 0) for r in trials.values())
        size = next((r["bytes"] for r in trials.values() if "bytes" in r), 0)
        print(
            f"| {e} | {', '.join(f'{x:.1f}' for x in times)} | {medians[e]:.1f} | {rss / 2**30:.2f} GiB | {size / 1e6:.1f} MB |"
        )
    others = [e for e in runs if e != "prospect"]
    if others:
        ref = min(others, key=lambda e: medians[e])
        common = sorted(set(runs["prospect"]) & set(runs[ref]))
        ratios = [runs[ref][t]["time"] / runs["prospect"][t]["time"] for t in common]
        med, lo, hi = interval(ratios)
        verdict = "passes" if lo >= 1.0 else "misses"
        print(
            f"\nThroughput over {ref} (the fastest other engine): median {med:.3f}x, "
            f"95% interval [{lo:.3f}, {hi:.3f}] over {len(ratios)} trials; the 1.0x target {verdict}."
        )
    if budget:
        rss = max(r.get("rss", 0) for r in runs["prospect"].values())
        limit = budget * 1.1
        print(
            f"Memory: prospect's peak RSS {rss / 2**30:.2f} GiB against the budget plus 10%, "
            f"{limit / 2**30:.2f} GiB: {'within' if rss <= limit else 'over'}."
        )


if __name__ == "__main__":
    main(sys.argv)
