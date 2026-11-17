#!/usr/bin/env python3
"""Per-class interleaved A/Bs: prospect builds in process, or any harness engines over serve.

  classes_ab.py split QUERIES DIR [PREFIX ...]
      One JSON-lines file per class tag (e.g. intersection:num_tokens_2 -> DIR/intersection_2.txt);
      PREFIX limits it to tags starting so (default: intersection).
  classes_ab.py bench OUT ROUNDS RUNS DIR COMMANDS SIDE=BIN:INDEX ...
      `prospect_engine bench` per class file in DIR, sides alternating each round (rotated for
      more than two), best of RUNS per query and round.
  classes_ab.py serve OUT ROUNDS RUNS DIR COMMANDS SIDE='serve command' ...
      The same over each engine's serve ("COMMAND\\tquery" in, one line out), one process per side
      for the session, timed around the round trip as the harness's client does; each query runs
      WARMUP times (default 1) per command first.
  classes_ab.py score OUT A B
      Per class and command: the run-level ratio B/A (each round's geometric mean over queries,
      then the mean over rounds with a t interval), the per-query best ratio, and mismatches.

Rounds write OUT/<round>-<side>-<command>-<class>.tsv as "µs\\tresult\\tquery" (ab.sh's layout), and
OUT/<round>.time with each round's start, warm-up, timed part and end.
"""

from __future__ import annotations

import glob
import json
import math
import os
import shlex
import subprocess
import sys
import time
from collections import defaultdict

# Student's t, 97.5th percentile, by degrees of freedom: 1 to 10 exact, then rounded up.
T975 = [12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23]


def t975(df: int) -> float:
    return T975[df - 1] if df <= len(T975) else 2.13 if df < 29 else 2.05


def class_name(tag: str) -> str:
    return tag.replace(":num_tokens_", "_").replace(">", "gt")


def split(queries: str, out: str, prefixes: list[str]) -> None:
    os.makedirs(out, exist_ok=True)
    by: dict[str, list[str]] = defaultdict(list)
    with open(queries) as f:
        for line in f:
            for tag in json.loads(line)["tags"]:
                if ":num_tokens_" in tag and any(tag.startswith(p) for p in prefixes):
                    by[class_name(tag)].append(line)
    for name, lines in sorted(by.items()):
        with open(os.path.join(out, name + ".txt"), "w") as f:
            f.writelines(lines)
        print(f"{name}\t{len(lines)}")


def classes(d: str) -> list[tuple[str, str]]:
    return [
        (os.path.basename(p)[:-4], p)
        for p in sorted(glob.glob(os.path.join(d, "*.txt")))
    ]


def sides_of(args: list[str]) -> list[tuple[str, str]]:
    out = []
    for a in args:
        name, spec = a.split("=", 1)
        if "-" in name:
            sys.exit(f"side name {name!r} must not contain '-'")
        out.append((name, spec))
    return out


def rotated(xs: list, k: int) -> list:
    k %= len(xs)
    return xs[k:] + xs[:k]


def bench(
    out: str,
    rounds: int,
    runs: int,
    d: str,
    commands: str,
    sides: list[tuple[str, str]],
) -> None:
    os.makedirs(out, exist_ok=True)
    cls = classes(d)
    for r in range(1, rounds + 1):
        t0 = time.time()
        for cmd in commands.split():
            for name, path in cls:
                for side, spec in rotated(sides, r - 1):
                    binary, index = spec.split(":", 1)
                    with (
                        open(path) as q,
                        open(
                            os.path.join(out, f"{r}-{side}-{cmd}-{name}.tsv"), "w"
                        ) as o,
                    ):
                        subprocess.run(
                            [binary, "bench", index, cmd, str(runs)],
                            stdin=q,
                            stdout=o,
                            check=True,
                        )
        t1 = time.time()
        with open(os.path.join(out, f"{r}.time"), "w") as f:
            f.write(f"start {t0:.3f}\nwarmup 0\ntimed {t1 - t0:.3f}\nend {t1:.3f}\n")
        print(f"{time.strftime('%T')} round {r} {t1 - t0:.1f}s", flush=True)


def serve(
    out: str,
    rounds: int,
    runs: int,
    d: str,
    commands: str,
    sides: list[tuple[str, str]],
) -> None:
    os.makedirs(out, exist_ok=True)
    cls = []
    for name, path in classes(d):
        with open(path) as f:
            cls.append((name, [json.loads(line)["query"] for line in f]))
    procs = {
        side: subprocess.Popen(
            shlex.split(spec),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        for side, spec in sides
    }

    def ask(side: str, cmd: str, q: str) -> tuple[float, str]:
        p = procs[side]
        assert p.stdin and p.stdout
        t = time.perf_counter_ns()
        p.stdin.write(f"{cmd}\t{q}\n")
        p.stdin.flush()
        res = p.stdout.readline().strip()
        return (time.perf_counter_ns() - t) / 1000.0, res

    try:
        tw = time.time()
        for (
            side,
            _,
        ) in sides:  # warm-up: every query WARMUP times (default 1) per command
            for cmd in commands.split() * int(os.environ.get("WARMUP", "1")):
                for _, qs in cls:
                    for q in qs:
                        ask(side, cmd, q)
        warm = time.time() - tw
        for r in range(1, rounds + 1):
            t0 = time.time()
            for cmd in commands.split():
                for name, qs in cls:
                    for side, _ in rotated(sides, r - 1):
                        with open(
                            os.path.join(out, f"{r}-{side}-{cmd}-{name}.tsv"), "w"
                        ) as o:
                            for q in qs:
                                best, res = math.inf, ""
                                for _ in range(runs):
                                    us, res = ask(side, cmd, q)
                                    best = min(best, us)
                                o.write(f"{best:.1f}\t{res}\t{q}\n")
            t1 = time.time()
            with open(os.path.join(out, f"{r}.time"), "w") as f:
                f.write(
                    f"start {t0:.3f}\nwarmup {warm if r == 1 else 0:.3f}\ntimed {t1 - t0:.3f}\nend {t1:.3f}\n"
                )
            print(f"{time.strftime('%T')} round {r} {t1 - t0:.1f}s", flush=True)
    finally:
        for p in procs.values():
            if p.stdin:
                p.stdin.close()
            p.wait(timeout=60)


def score(out: str, a: str, b: str) -> None:
    data: dict[tuple[str, str, str, str], dict[str, tuple[float, str]]] = {}
    for path in glob.glob(os.path.join(out, "*.tsv")):
        r, side, cmd, cls = os.path.basename(path)[:-4].split("-", 3)
        rows = {}
        with open(path) as f:
            for line in f:
                t, res, q = line.rstrip("\n").split("\t", 2)
                rows[q] = (float(t), res)
        data[(r, side, cmd, cls)] = rows
    rounds = sorted({k[0] for k in data}, key=int)
    keys = sorted({(k[2], k[3]) for k in data})
    order = {
        c: i
        for i, c in enumerate(
            ["TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT", "COUNT"]
        )
    }
    keys.sort(key=lambda k: (k[1], order.get(k[0], 9)))
    print(
        f"| Class | Command | n | {b}/{a} run level [95%] | rounds | per-query best | mismatches |"
    )
    print("|---|---|---:|---|---|---:|---:|")
    for cmd, cls in keys:
        per_round, best_a, best_b, mism, qs = [], {}, {}, 0, None
        for r in rounds:
            ra, rb = data.get((r, a, cmd, cls)), data.get((r, b, cmd, cls))
            if not ra or not rb:
                continue
            qs = sorted(set(ra) & set(rb))
            per_round.append(
                math.exp(
                    sum(math.log(max(rb[q][0], 0.1) / max(ra[q][0], 0.1)) for q in qs)
                    / len(qs)
                )
            )
            for q in qs:
                best_a[q] = min(best_a.get(q, math.inf), ra[q][0])
                best_b[q] = min(best_b.get(q, math.inf), rb[q][0])
                mism += ra[q][1] != rb[q][1]
        if not per_round or not qs:
            continue
        n = len(per_round)
        logs = [math.log(x) for x in per_round]
        m = sum(logs) / n
        sd = math.sqrt(sum((x - m) ** 2 for x in logs) / (n - 1)) if n > 1 else 0.0
        h = t975(n - 1) * sd / math.sqrt(n) if n > 1 else 0.0
        pq = math.exp(
            sum(math.log(max(best_b[q], 0.1) / max(best_a[q], 0.1)) for q in best_a)
            / len(best_a)
        )
        spread = " ".join(f"{x:.2f}" for x in per_round)
        print(
            f"| {cls} | `{cmd}` | {len(qs)} | {math.exp(m):.3f} [{math.exp(m - h):.3f}, {math.exp(m + h):.3f}] | {spread} | {pq:.3f} | {mism} |"
        )


def main(argv: list[str]) -> None:
    if len(argv) < 2:
        sys.exit(__doc__)
    mode = argv[1]
    if mode == "split":
        split(argv[2], argv[3], argv[4:] or ["intersection"])
    elif mode in ("bench", "serve"):
        out, rounds, runs, d, commands = (
            argv[2],
            int(argv[3]),
            int(argv[4]),
            argv[5],
            argv[6],
        )
        (bench if mode == "bench" else serve)(
            out, rounds, runs, d, commands, sides_of(argv[7:])
        )
    elif mode == "score":
        score(argv[2], argv[3], argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
