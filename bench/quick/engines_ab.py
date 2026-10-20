#!/usr/bin/env python3
"""Turns harness trials (src/client.py's results.json, one per trial and set) into ab.sh's
round files, so quick/ab.py --aa scores engines at the run level (docs/perf/ab-run-level.md).

Each trial is a run. Side a is the reference engine and side b prospect, so B/A is prospect's time
over the reference's; a query's time in a run is its best over the trial's iterations, and
its result the count. A query an engine does not answer (PISA's phrases) is left out.

  engines_ab.py TRIALS_DIR OUT A_ENGINE B_ENGINE

TRIALS_DIR holds q<trial>-<set>.json, as bench/query_trials.sh names them; the sets
"queries"/"queries-*" map to ab.py's "queries" and "terms*" to "terms". The query files are
copied to OUT/queries.txt and OUT/terms.txt from QUERIES and TERMS for the tags.
"""

from __future__ import annotations

import glob
import json
import os
import re
import shutil
import sys
from typing import Any

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "phase2")
)
from gate import canon


def set_name(name: str) -> str:
    return "terms" if name.startswith("terms") else "queries"


def convert(trials: str, out: str, a: str, b: str) -> int:
    os.makedirs(out, exist_ok=True)
    written = 0
    for path in sorted(glob.glob(os.path.join(trials, "q*-*.json"))):
        m = re.fullmatch(r"q(\d+)-(.+)\.json", os.path.basename(path))
        if not m:
            continue
        trial, qset = m.group(1), set_name(m.group(2))
        with open(path) as f:
            results: dict[str, dict[str, list[dict[str, Any]]]] = json.load(f)[
                "results"
            ]
        for cmd, engines in results.items():
            for side, engine in (("a", a), ("b", b)):
                rows = {canon(e): r for e, r in engines.items()}.get(canon(engine))
                if rows is None:
                    continue
                target = os.path.join(out, f"{trial}-{side}-{cmd}-{qset}.tsv")
                with open(target, "w") as f:
                    for r in rows:
                        d = r.get("duration") or []
                        if not d or r.get("count", -1) < 0:
                            continue
                        f.write(f"{min(d)}\t{r['count']}\t{r['query']}\n")
                written += 1
    for qset, env in (("queries", "QUERIES"), ("terms", "TERMS")):
        src = os.environ.get(env)
        if src:
            shutil.copy(os.path.expanduser(src), os.path.join(out, qset + ".txt"))
    return written


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    print(convert(*sys.argv[1:5]), "files")
