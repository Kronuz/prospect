#!/usr/bin/env python3
"""The comparative soak (TESTING, "Comparative soak"): one churn workload, the same for
every engine, through each engine's churn mode, with throughput, disk, memory, and query
latency recorded after every cycle.

Every engine reads the same op stream on stdin and answers on stdout:
  P<TAB>id<TAB>text      upsert by id
  D<TAB>id               delete by id
  C                      commit; the engine answers "C<TAB>µs<TAB>segments"
  Q<TAB>COMMAND<TAB>q    query; the engine answers "Q<TAB>µs<TAB>result"

The workload loads LIVE documents, then each cycle replaces 5% of the live set, deletes
2%, and adds 2% new ones (the live set stays constant), commits, and runs the query
sample. Every tenth cycle it also adds HUGE documents (each the text of 200 articles) and
deletes the previous cycle's. Disk is the bytes of the files in the index directory;
memory is the engine process's resident set, both read after each commit.

  soak.py --corpus corpus.json --queries queries.txt --dir DIR [--live N] [--cycles N]
          [--seed N] -- ENGINE_COMMAND...   ("{dir}" in the command becomes DIR)
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import time
from collections.abc import Iterator


def corpus_texts(path: str) -> Iterator[str]:
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                yield json.loads(line)["text"].replace("\t", " ").replace("\n", " ")


def dir_bytes(path: str) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


def rss_bytes(pid: int) -> int:
    out = subprocess.run(
        ["ps", "-o", "rss=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return int(out) * 1024 if out else 0


class Engine:
    def __init__(self, command: list[str]) -> None:
        self.proc = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1
        )

    def send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")

    def answer(self, kind: str) -> list[str]:
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"engine exited (code {self.proc.wait()})")
            fields = line.rstrip("\n").split("\t")
            if fields[0] == kind:
                return fields

    def close(self) -> int:
        assert self.proc.stdin is not None
        self.proc.stdin.close()
        return self.proc.wait()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--live", type=int, default=100_000)
    ap.add_argument("--cycles", type=int, default=100)
    ap.add_argument("--query-sample", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    command = [c.replace("{dir}", args.dir) for c in args.command if c != "--"]
    rng = random.Random(args.seed)
    texts = corpus_texts(args.corpus)
    with open(
        args.queries, encoding="utf-8"
    ) as f:  # the harness's JSON lines, or one query a line
        queries = [
            json.loads(q)["query"] if q.startswith("{") else q.strip()
            for q in f
            if q.strip()
        ]
    sample = rng.sample(queries, min(args.query_sample, len(queries)))

    shutil.rmtree(args.dir, ignore_errors=True)
    os.makedirs(args.dir)
    engine = Engine(command)
    live: list[str] = []
    next_id = 0
    huge: list[str] = []

    def put(doc_id: str, text: str) -> None:
        engine.send(f"P\t{doc_id}\t{text}")

    start = time.monotonic()
    for _ in range(args.live):
        put(str(next_id), next(texts))
        live.append(str(next_id))
        next_id += 1
    engine.send("C")
    c = engine.answer("C")
    load_s = time.monotonic() - start
    print(
        json.dumps(
            {
                "cycle": 0,
                "seconds": round(load_s, 3),
                "ops": args.live,
                "commit_us": int(c[1]),
                "segments": int(c[2]),
                "disk": dir_bytes(args.dir),
                "rss": rss_bytes(engine.proc.pid),
            }
        ),
        flush=True,
    )

    for cycle in range(1, args.cycles + 1):
        ops = 0
        start = time.monotonic()
        for doc_id in rng.sample(live, len(live) * 5 // 100):
            put(doc_id, next(texts))
            ops += 1
        doomed = set(rng.sample(live, len(live) * 2 // 100))
        for doc_id in doomed:
            engine.send(f"D\t{doc_id}")
            ops += 1
        live = [d for d in live if d not in doomed]
        for _ in range(len(doomed)):
            put(str(next_id), next(texts))
            live.append(str(next_id))
            next_id += 1
            ops += 1
        if cycle % 10 == 0:
            for doc_id in huge:
                engine.send(f"D\t{doc_id}")
                ops += 1
            huge = []
            for _ in range(5):
                doc_id = f"huge{next_id}"
                next_id += 1
                put(doc_id, " ".join(next(texts) for _ in range(200)))
                huge.append(doc_id)
                ops += 1
        engine.send("C")
        c = engine.answer("C")
        write_s = time.monotonic() - start
        latencies: list[int] = []
        for q in sample:
            for cmd in ("COUNT", "TOP_10"):
                engine.send(f"Q\t{cmd}\t{q}")
                latencies.append(int(engine.answer("Q")[1]))
        latencies.sort()
        print(
            json.dumps(
                {
                    "cycle": cycle,
                    "seconds": round(write_s, 3),
                    "ops": ops,
                    "commit_us": int(c[1]),
                    "segments": int(c[2]),
                    "disk": dir_bytes(args.dir),
                    "rss": rss_bytes(engine.proc.pid),
                    "query_p50_us": latencies[len(latencies) // 2],
                    "query_p99_us": latencies[len(latencies) * 99 // 100],
                    "query_mean_us": round(statistics.fmean(latencies), 1),
                }
            ),
            flush=True,
        )
    return engine.close()


if __name__ == "__main__":
    sys.exit(main())
