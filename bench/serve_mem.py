#!/usr/bin/env python3
"""Serving memory per engine (PLAN, memory ratchet): runs each engine's `serve` through the
harness queries for TOP_10, COUNT, and TOP_1000 in turn, after dropping the page cache, and
records the peak anonymous memory and the peak mapped file pages (the index pages the
queries made resident) over the engine's processes. Untimed; Linux only.

  serve_mem.py ENGINE...   (from ~/bench/search-benchmark-game)
"""

from __future__ import annotations

import json
import subprocess
import sys

COMMANDS = ("TOP_10", "COUNT", "TOP_1000")


def tree(pid: int) -> list[int]:
    """pid and every descendant."""
    out = [pid]
    for p in out:
        try:
            with open(f"/proc/{p}/task/{p}/children") as f:
                out.extend(int(c) for c in f.read().split())
        except OSError:
            pass
    return out


def rss(pids: list[int]) -> tuple[int, int]:
    """Summed RssAnon and RssFile in kB."""
    anon = file = 0
    for p in pids:
        try:
            with open(f"/proc/{p}/status") as f:
                for line in f:
                    if line.startswith("RssAnon:"):
                        anon += int(line.split()[1])
                    elif line.startswith("RssFile:"):
                        file += int(line.split()[1])
        except OSError:
            pass
    return anon, file


def measure(engine: str, queries: list[str]) -> tuple[int, int, int]:
    subprocess.run(
        "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null",
        shell=True,
        check=True,
    )
    p = subprocess.Popen(
        ["make", "-s", "-C", f"engines/{engine}", "serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    assert p.stdin and p.stdout
    peak_anon = peak_file = peak = 0
    n = 0
    for command in COMMANDS:
        for q in queries:
            p.stdin.write(f"{command}\t{q}\n")
            p.stdin.flush()
            p.stdout.readline()
            n += 1
            if n % 50 == 0:
                anon, file = rss(tree(p.pid))
                peak_anon, peak_file, peak = (
                    max(peak_anon, anon),
                    max(peak_file, file),
                    max(peak, anon + file),
                )
    anon, file = rss(tree(p.pid))
    peak_anon, peak_file, peak = (
        max(peak_anon, anon),
        max(peak_file, file),
        max(peak, anon + file),
    )
    p.stdin.close()
    p.wait()
    return peak_anon, peak_file, peak


def main() -> None:
    with open("queries.txt") as f:
        queries = [json.loads(line)["query"] for line in f if line.strip()]
    print("| Engine | Anonymous | Mapped index pages | Peak |")
    print("|---|---:|---:|---:|")
    for engine in sys.argv[1:]:
        anon, file, peak = measure(engine, queries)
        print(
            f"| {engine} | {anon / 1024:,.0f} MB | {file / 1024:,.0f} MB | {peak / 1024:,.0f} MB |",
            flush=True,
        )


if __name__ == "__main__":
    main()
