#!/usr/bin/env python3
"""A cached baseline from full harness runs, for nights that run only prospect and an anchor.

  baseline.py OUT.json TRIAL [TRIAL...]        TRIAL: RESULTS_JSON[+RESULTS_JSON...]

Per command, engine, and query: the median over trials of the query's fastest run, and its
count, from runs that include every baseline engine; a query an engine answers UNSUPPORTED
keeps its null count and no timings. The file also records the result files it came from
with their SHA-256, the engines, and the date. gate.py --baseline reads it: engines a night
did not run come from here, and the anchor engine (ANCHOR, run every night) must stay within
DRIFT of its cached times, per command and class, or the night's rows are marked DRIFT and
pass nothing.
"""

import datetime
import hashlib
import json
import os
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate import canon

ANCHOR = "tantivy-0.26"
DRIFT = 0.03


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    out, trials = argv[0], argv[1:]
    times = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))  # cmd, engine, query
    meta = {}
    sources = []
    for trial in trials:
        seen = defaultdict(lambda: defaultdict(dict))
        for path in trial.split("+"):
            sources.append({"path": path, "sha256": sha256(path)})
            with open(path) as f:
                results = json.load(f)["results"]
            for cmd, per in results.items():
                for e, qs in per.items():
                    e = canon(e)
                    for q in qs:
                        meta[(cmd, q["query"])] = {"tags": q["tags"]}
                        meta[(cmd, q["query"], e)] = q["count"]
                        timed = q["count"] is not None and q["duration"]  # not UNSUPPORTED
                        seen[cmd][e][q["query"]] = min(q["duration"]) if timed else None
        for cmd, per in seen.items():
            for e, qs in per.items():
                for query, t in qs.items():
                    times[cmd][e].setdefault(query, [])
                    if t is not None:
                        times[cmd][e][query].append(t)
    if not any(ANCHOR in per for per in times.values()):
        sys.exit(f"baseline: the anchor {ANCHOR} is in none of the runs")
    results = {}
    for cmd, per in times.items():
        results[cmd] = {}
        for e, qs in per.items():
            results[cmd][e] = [
                {
                    "query": query,
                    "tags": meta[(cmd, query)]["tags"],
                    "count": meta[(cmd, query, e)],
                    "duration": [statistics.median(ts)] if ts else [],
                    "trials": len(ts),
                }
                for query, ts in sorted(qs.items())
            ]
    doc = {
        "baseline": {
            "made": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "anchor": ANCHOR,
            "drift": DRIFT,
            "trials": len(trials),
            "sources": sources,
            "engines": sorted({e for per in times.values() for e in per}),
        },
        "results": results,
    }
    with open(out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"baseline: {out}, {len(trials)} trials, engines {', '.join(doc['baseline']['engines'])}")


if __name__ == "__main__":
    main(sys.argv[1:])
