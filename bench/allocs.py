#!/usr/bin/env python3
"""Allocations per query by command and class (PLAN, "Ratchets", Memory: allocations per
query do not grow with the number of matches), from the allocation-counting build
(bench/alloc_count.c preloaded into the release binary).

  allocs.py SHIM ENGINE INDEX QUERIES...

Runs `ENGINE serve INDEX` with the shim preloaded and sends each query alone, waiting for its
answer. COUNT runs first and gives every query its number of matches; then, per command, one
pass over every query warms and one more is measured. Per command and class: the queries, the
median and largest allocations per query, the mean over the quarter with the fewest matches
and over the quarter with the most, and Spearman's rho between matches and allocations. A
class is flagged GROWS when rho exceeds 0.5 and the high quarter's mean exceeds 1.5 times the
low quarter's plus one.
"""

from __future__ import annotations

import json
import os
import statistics
import subprocess
import sys
from collections import defaultdict

COMMANDS = ("COUNT", "TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT")


def ranks(xs: list[float]) -> list[float]:
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    r = [0.0] * len(xs)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2
        i = j + 1
    return r


def spearman(xs: list[float], ys: list[float]) -> float:
    if len(xs) < 3:
        return 0.0
    rx, ry = ranks(xs), ranks(ys)
    mx, my = statistics.fmean(rx), statistics.fmean(ry)
    sxy = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    sx = sum((a - mx) ** 2 for a in rx) ** 0.5
    sy = sum((b - my) ** 2 for b in ry) ** 0.5
    return sxy / (sx * sy) if sx and sy else 0.0


def main(argv: list[str]) -> int:
    shim, engine, index, *sets = argv
    queries: list[tuple[str, list[str]]] = []
    for path in sets:
        with open(path) as f:
            for line in f:
                if line.strip():
                    q = json.loads(line)
                    queries.append((q["query"], [t for t in q["tags"] if ":" not in t]))
    log = os.path.join(os.environ.get("OUT", "."), "allocs.log")
    env = dict(os.environ, LD_PRELOAD=os.path.abspath(shim), ALLOC_LOG=log)
    proc = subprocess.Popen([engine, "serve", index], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
    assert proc.stdin and proc.stdout
    sent: list[tuple[str, int, bool]] = []  # command, query index, measured
    answers: dict[tuple[str, int], str] = {}

    def ask(cmd: str, i: int, measured: bool) -> str:
        proc.stdin.write(f"{cmd}\t{queries[i][0]}\n".encode())
        proc.stdin.flush()
        a = proc.stdout.readline().decode().strip()
        sent.append((cmd, i, measured))
        return a

    for cmd in COMMANDS:
        for measured in (False, True):
            for i in range(len(queries)):
                a = ask(cmd, i, measured)
                if measured:
                    answers[cmd, i] = a
    proc.stdin.close()
    proc.wait()
    with open(log) as f:
        marks = [tuple(map(int, line.split())) for line in f if line.strip()]
    if len(marks) < len(sent) + 1:
        sys.exit(f"allocs: {len(marks)} log lines for {len(sent)} queries: the engine did not read one query a line")
    per: dict[tuple[str, int], tuple[int, int]] = {}
    for k, (cmd, i, measured) in enumerate(sent):
        if measured:
            per[cmd, i] = (marks[k + 1][0] - marks[k][0], marks[k + 1][1] - marks[k][1])
    matches = [int(answers["COUNT", i]) for i in range(len(queries))]
    print(f"{len(queries)} queries; allocations per query by command and class (measured pass after a warm one)\n")
    print("| Command | Class | n | Allocations, median | Largest | Bytes, median | Fewest-matches quarter | Most-matches quarter | Spearman rho | |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---|")
    grows = 0
    for cmd in COMMANDS:
        by_class: dict[str, list[int]] = defaultdict(list)
        for i, (_, tags) in enumerate(queries):
            for t in tags:
                by_class[t].append(i)
        for cls in sorted(by_class):
            ids = sorted(by_class[cls], key=lambda i: matches[i])
            allocs = [per[cmd, i][0] for i in ids]
            q = max(1, len(ids) // 4)
            low, high = statistics.fmean(allocs[:q]), statistics.fmean(allocs[-q:])
            rho = spearman([matches[i] for i in ids], [float(a) for a in allocs])
            flag = rho > 0.5 and high > 1.5 * low + 1
            grows += flag
            print(
                f"| `{cmd}` | {cls} | {len(ids)} | {statistics.median(allocs):g} | {max(allocs)} | "
                f"{statistics.median(per[cmd, i][1] for i in ids):g} | {low:.1f} | {high:.1f} | {rho:.2f} | {'GROWS' if flag else ''} |"
            )
    print(f"\n{grows} command and class rows grow with the number of matches")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
