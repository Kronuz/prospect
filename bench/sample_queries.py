#!/usr/bin/env python3
"""The quick tier's query sample (BENCHMARKS.md, "Tiers"): up to PER queries of each class
(a query's first tag without a colon), drawn with a fixed seed so every run of the tier
measures the same queries. Classes with fewer queries keep them all.

  sample_queries.py IN OUT [PER]
"""

from __future__ import annotations

import json
import random
import sys
from collections import defaultdict


def main() -> None:
    source, target = sys.argv[1], sys.argv[2]
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    by_class: dict[str, list[str]] = defaultdict(list)
    with open(source) as f:
        for line in f:
            if not line.strip():
                continue
            tags = [t for t in json.loads(line)["tags"] if ":" not in t]
            by_class[tags[0] if tags else "untagged"].append(line)
    rng = random.Random(7)
    out: list[str] = []
    for name in sorted(by_class):
        lines = by_class[name]
        out.extend(lines if len(lines) <= per else rng.sample(lines, per))
    with open(target, "w") as f:
        f.writelines(out)
    counts = {name: min(len(v), per) for name, v in sorted(by_class.items())}
    print(f"{len(out)} queries: {counts}")


if __name__ == "__main__":
    main()
