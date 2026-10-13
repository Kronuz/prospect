#!/usr/bin/env python3
"""Phase 2 query gate over harness trials (BENCHMARKS rule 3, Phase 2 review G5).

  gate.py [--md] TRIAL [TRIAL...]      TRIAL: RESULTS_JSON[+RESULTS_JSON...]

A trial is one or more harness runs (the gate queries and the stratified term set) of
strata and its baselines (Lucene, Tantivy, and Xapian
when present). A query's time is its fastest run. The pipe floor is the smallest time
over every trial, engine, and query, since one run's minimum can be a query's real work.

Per trial, command, and class: the geometric mean over queries of strata / engine, raw
and with the floor subtracted from both (at least 1 us each). The ratio against the
faster of Lucene and Tantivy is the larger of the two geometric means. Across trials:
the median and the 95% bootstrap interval of the median. A class is flagged when the
floor exceeds half the mean time of strata or the faster engine. Counts must equal
every baseline's; mismatches are reported per engine.
"""
import json
import math
import random
import statistics
import sys
from collections import defaultdict

FAST = ["lucene-10.4.0", "tantivy-0.26"]
XAPIAN = "xapian-2.1.0"
COUNTED = {"COUNT", "TOP_100_COUNT"}


def load(trial):
    merged = defaultdict(lambda: defaultdict(list))
    for path in trial.split("+"):
        for cmd, per in json.load(open(path))["results"].items():
            for e, qs in per.items():
                merged[cmd][e] += qs
    return merged


def run_floor(r):
    return min(min(q["duration"]) for per in r.values() for e in per.values() for q in e)


def geo(pairs):
    return math.exp(sum(math.log(a / b) for a, b in pairs) / len(pairs))


def trial(r, floor):
    """{(cmd, tag): row} for one file, and the count mismatches per command."""
    rows, mism = {}, {}
    for cmd, per in r.items():
        t = {e: {q["query"]: q for q in per[e]} for e in per}
        mism[cmd] = {e: sum(1 for qs, q in t["strata"].items() if q["count"] != t[e][qs]["count"]) for e in t if e != "strata"}
        by = defaultdict(list)
        for qs, q in t["strata"].items():
            for tag in q["tags"]:
                if ":" not in tag:
                    by[tag].append(qs)
        for tag, qs in by.items():
            mean = {e: statistics.fmean(min(t[e][x]["duration"]) for x in qs) for e in t}
            row = {"n": len(qs), "mean": mean}
            for e in t:
                if e != "strata":
                    times = [(min(t["strata"][x]["duration"]), min(t[e][x]["duration"])) for x in qs]
                    row[e] = (geo(times), geo([(max(s - floor, 1.0), max(o - floor, 1.0)) for s, o in times]))
            row["raw"] = max(row[e][0] for e in FAST)
            row["sub"] = max(row[e][1] for e in FAST)
            faster = min(FAST, key=lambda e: mean[e])
            row["flag"] = floor > 0.5 * min(mean["strata"], mean[faster])
            rows[(cmd, tag)] = row
    return rows, mism


def interval(values, reps=10000, seed=2026):
    rng = random.Random(seed)
    meds = sorted(statistics.median(rng.choices(values, k=len(values))) for _ in range(reps))
    return statistics.median(values), meds[int(0.025 * reps)], meds[int(0.975 * reps) - 1]


def main(argv):
    md = "--md" in argv
    paths = [a for a in argv if a != "--md"]
    results = [load(p) for p in paths]
    floors = [run_floor(r) for r in results]
    floor = min(floors)
    trials = [trial(r, floor) for r in results]
    print(f"pipe floor {floor:.0f} us, from {paths[floors.index(floor)]}; {len(paths)} trials")
    if md:
        print("\n| Command | Class | n | Trials | strata µs | Lucene µs | Tantivy µs | Xapian µs | Raw | Floor-subtracted | Against Xapian |")
        print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for cmd in dict.fromkeys(cmd for r in results for cmd in r):
        if not md:
            per = defaultdict(list)
            for _, m in trials:
                for e, n in m.get(cmd, {}).items():
                    per[e].append(n)
            print(f"{cmd}  count mismatches: " + ", ".join(f"{e} {v}" for e, v in per.items()))
        for tag in sorted({tag for rows, _ in trials for c, tag in rows if c == cmd}):
            rows = [rows[(cmd, tag)] for rows, _ in trials if (cmd, tag) in rows]
            raw = interval([r["raw"] for r in rows])
            sub = interval([r["sub"] for r in rows])
            xap = interval([r[XAPIAN][1] for r in rows]) if all(XAPIAN in r for r in rows) else None
            us = {e: statistics.median(r["mean"][e] for r in rows) for e in rows[0]["mean"]}
            # PLAN: counted classes within 1.5x; a TOP_K class above it needs a cause and a plan.
            verdict = "FAIL" if sub[0] > 2 or (cmd in COUNTED and sub[0] > 1.5) else "over" if sub[0] > 1.5 else ""
            flag = "floor>1/2" if any(r["flag"] for r in rows) else ""
            n = max(r["n"] for r in rows)
            if md:
                x = f"{xap[0]:.3f}× [{xap[1]:.3f}–{xap[2]:.3f}]" if xap else "–"
                xu = f"{us[XAPIAN]:,.0f}" if XAPIAN in us else "–"
                print(f"| `{cmd}` | {tag} | {n} | {len(rows)} | {us['strata']:,.0f} | {us['lucene-10.4.0']:,.0f} | {us['tantivy-0.26']:,.0f} | {xu} "
                      f"| {raw[0]:.2f}× [{raw[1]:.2f}–{raw[2]:.2f}] | {sub[0]:.2f}× [{sub[1]:.2f}–{sub[2]:.2f}] {flag} | {x} |")
            else:
                x = f"  Xapian {xap[0]:.3f}x [{xap[1]:.3f}-{xap[2]:.3f}]" if xap else ""
                print(f"  {tag:20} n={n:4} t={len(rows)}  raw {raw[0]:5.2f}x [{raw[1]:.2f}-{raw[2]:.2f}]  "
                      f"floor-sub {sub[0]:5.2f}x [{sub[1]:.2f}-{sub[2]:.2f}]{x} {flag} {verdict}")


if __name__ == "__main__":
    main(sys.argv[1:])
