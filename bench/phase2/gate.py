#!/usr/bin/env python3
"""Phase 2 query gate over harness trials.

  gate.py RESULTS_JSON [RESULTS_JSON...]

Each file is one harness run of strata, Lucene, and Tantivy. A query's time is its
fastest run. The pipe floor of a run is its smallest time over all engines and queries.
Per command and class: the geometric mean over queries of (strata - floor) / (engine -
floor) for each engine, the ratio against the faster engine (the smaller geometric mean
of engine over strata), and the arithmetic-mean ratio; across trials, the mean and the
range. Counts must equal Tantivy's.
"""
import json
import math
import sys
from collections import defaultdict

ENGINES = ["lucene-10.4.0", "tantivy-0.26"]


def trial(path):
    r = json.load(open(path))["results"]
    floor = min(min(q["duration"]) for cmd in r.values() for e in cmd.values() for q in e)
    out = {}
    for cmd, per in r.items():
        t = {e: {q["query"]: q for q in per[e]} for e in per}
        mism = sum(1 for qs, q in t["strata"].items() if q["count"] != t["tantivy-0.26"][qs]["count"])
        by = defaultdict(lambda: defaultdict(list))
        for qs, q in t["strata"].items():
            s = min(q["duration"])
            for tag in q["tags"]:
                if ":" in tag:
                    continue
                for e in ENGINES:
                    o = min(t[e][qs]["duration"])
                    by[tag][e].append((max(s - floor, 1.0), max(o - floor, 1.0), s, o))
        rows = {}
        for tag, v in by.items():
            geo = {e: math.exp(sum(math.log(a / b) for a, b, _, _ in v[e]) / len(v[e])) for e in ENGINES}
            mean = {e: sum(s for _, _, s, _ in v[e]) / sum(o for _, _, _, o in v[e]) for e in ENGINES}
            rows[tag] = (max(geo.values()), max(mean.values()), len(v[ENGINES[0]]))
        out[cmd] = (rows, mism)
    return floor, out


trials = [trial(p) for p in sys.argv[1:]]
print("floors (us):", ", ".join(f"{f:.0f}" for f, _ in trials))
for cmd in trials[0][1]:
    print(f"{cmd}  count mismatches: {[t[1][cmd][1] for t in trials]}")
    for tag in sorted(trials[0][1][cmd][0]):
        geos = [t[1][cmd][0][tag][0] for t in trials]
        means = [t[1][cmd][0][tag][1] for t in trials]
        n = trials[0][1][cmd][0][tag][2]
        g = sum(geos) / len(geos)
        flag = "FAIL" if g > 2 else "over" if g > 1.5 else ""
        print(f"  {tag:20} n={n:4}  geomean {g:5.2f}x [{min(geos):.2f}-{max(geos):.2f}]  mean {sum(means)/len(means):5.2f}x {flag}")
