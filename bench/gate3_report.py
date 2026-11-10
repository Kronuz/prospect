#!/usr/bin/env python3
"""Phase 3's gate rows on all four axes from gate3.sh's job files (strata BENCHMARKS.md):
indexing speed and memory (unit-*.txt), disk after a full merge (sections.txt beside
keyed-bp.sizes), write amplification (*.result), serving memory (servemem.md), and query
speed through bench/phase2/gate.py over the q*.json trials.

  gate3_report.py [--ref ENGINE] [--budget-mb N] DIR...

DIR is a job's files (its out/STEP directory or any parent). Indexing trials are the units
sharing a tag; per tag, prospect's wall time over the reference engine's (default: the
fastest other engine in that tag), then the median over tags with its 95% bootstrap
interval. Peaks are reported against the node budget plus 10% and against each engine.
"""

from __future__ import annotations

import os
import random
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

MIB = 1 << 20


def files(dirs: list[str], pattern: str) -> list[Path]:
    return sorted({p for d in dirs for p in Path(d).rglob(pattern)})


def kv(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for word in path.read_text().split():
        k, _, v = word.partition("=")
        out[k] = v
    return out


def interval(xs: list[float], n: int = 10000) -> tuple[float, float]:
    rnd = random.Random(1)
    meds = sorted(statistics.median(rnd.choices(xs, k=len(xs))) for _ in range(n))
    return meds[int(0.025 * n)], meds[int(0.975 * n) - 1]


def indexing(dirs: list[str], ref: str | None, budget_mb: float | None) -> None:
    units = [kv(p) for p in files(dirs, "unit-*.txt")]
    if not units:
        return
    print("## Indexing\n")
    print(
        "| Tag | Engine | Docs | Wall s | CPU s | Peak RSS MiB | Anon MiB | Segments | Bytes | Budget MiB |"
    )
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    by_tag: dict[str, dict[str, dict[str, str]]] = defaultdict(dict)
    for u in units:
        by_tag[u["tag"]][
            u["engine"]
            + (
                "-" + u["threads"] + "t"
                if u["engine"] == "prospect" and u["threads"] != "1"
                else ""
            )
        ] = u
        print(
            f"| {u['tag']} | {u['engine']} | {u['docs']} | {float(u['wall_s']):.1f} | {float(u['user_s']) + float(u['sys_s']):.1f} | "
            f"{int(u['max_rss_kb']) / 1024:,.0f} | {int(u['cg_anon_peak']) / MIB:,.0f} | {u['segments']} | {int(u['bytes']):,} | {u['budget_mb']} |"
        )
    ratios: list[float] = []
    for tag, per in sorted(by_tag.items()):
        p = per.get("prospect")
        others = {e: u for e, u in per.items() if not e.startswith("prospect")}
        if ref:
            others = {e: u for e, u in others.items() if e == ref}
        if not p or not others:
            continue
        e, u = min(others.items(), key=lambda eu: float(eu[1]["wall_s"]))
        r = float(p["wall_s"]) / float(u["wall_s"])
        ratios.append(r)
        rss = int(p["max_rss_kb"]) / int(u["max_rss_kb"])
        print(
            f"\n{tag}: prospect {r:.3f}x {e}'s wall time ({1 / r:.2f}x faster), peak RSS {rss:.2f}x",
            end="",
        )
        b = budget_mb or (float(p["budget_mb"]) if p.get("budget_mb") else None)
        if b:
            over = int(p["max_rss_kb"]) / 1024 / b
            print(
                f", {over:.2f}x the {b:,.0f} MiB budget ({'within' if over <= 1.1 else 'past'} budget + 10%)",
                end="",
            )
    print()
    if len(ratios) >= 2:
        lo, hi = interval(ratios)
        print(
            f"\nover {len(ratios)} trials: median {statistics.median(ratios):.3f}x [{lo:.3f}-{hi:.3f}] (below 1: prospect faster)"
        )
    print()


def sizes_of(path: Path) -> dict[str, int]:
    out: dict[str, int] = defaultdict(int)
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].isdigit():
            out[parts[0].rsplit("/", 1)[-1]] += int(parts[1])
    return out


SECTIONS = (  # row, prospect sections, Lucene extensions
    ("Whole index", ("file",), ("total",)),
    ("Postings", ("postings",), ("doc",)),
    ("Positions", ("positions",), ("pos",)),
    (
        "Terms with keys",
        ("terms", "term_index", "keys", "key_index"),
        ("tim", "tip", "tmd"),
    ),
    ("Stored", ("stored",), ("fdt", "fdx", "fdm")),
    ("Norms", ("norms",), ("nvd", "nvm")),
)


def disk(dirs: list[str]) -> None:
    for sec in files(dirs, "sections.txt"):
        bp = sec.parent / "keyed-bp.sizes"
        if not bp.exists():
            continue
        s, lu = sizes_of(sec), sizes_of(bp)
        print(f"## Disk after a full merge ({sec.parent})\n")
        print("| Section | prospect bytes | Lucene -bp keyed bytes | Ratio |")
        print("|---|---:|---:|---:|")
        for row, ps, ls in SECTIONS:
            a, b = sum(s[k] for k in ps), sum(lu[k] for k in ls)
            print(
                f"| {row} | {a:,} | {b:,} | {a / b:.3f}x |"
                if b
                else f"| {row} | {a:,} | - | |"
            )
        rest = sorted(k for k in s if k not in {x for _, ps, _ in SECTIONS for x in ps})
        print("\nprospect only: " + ", ".join(f"{k} {s[k]:,}" for k in rest) + "\n")


def wamp(dirs: list[str]) -> None:
    rs = files(dirs, "*.result")
    if rs:
        print("## Write amplification\n")
        for r in rs:
            print(f"- {r.parent.name}: {r.read_text().strip()}")
        print()


def servemem(dirs: list[str]) -> None:
    for m in files(dirs, "servemem.md"):
        print(f"## Serving memory ({m.parent})\n")
        print(m.read_text())


def queries(dirs: list[str]) -> None:
    trials: dict[str, list[str]] = defaultdict(list)
    for q in files(dirs, "q*.json"):
        t = q.stem.split("-", 1)[0]
        trials[t].append(str(q))
    if not trials:
        return
    gate = Path(__file__).resolve().parent / "phase2" / "gate.py"
    args = ["+".join(trials[t]) for t in sorted(trials)]
    print(
        f"## Queries: `gate.py --md --target 1.0 {os.environ.get('GATE_ARGS', '')}` over {len(args)} trials\n",
        flush=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(gate),
            "--md",
            "--target",
            "1.0",
            *os.environ.get("GATE_ARGS", "").split(),
            *args,
        ],
        check=False,
    )


def main(argv: list[str]) -> int:
    ref = budget = None
    while argv and argv[0].startswith("--"):
        flag, val, argv = argv[0], argv[1], argv[2:]
        if flag == "--ref":
            ref = val
        elif flag == "--budget-mb":
            budget = float(val)
    indexing(argv, ref, budget)
    disk(argv)
    wamp(argv)
    servemem(argv)
    queries(argv)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
