#!/usr/bin/env python3
"""Line and branch coverage of prospect's headers, unioned over every test binary.

`llvm-cov report` with several binaries keeps one binary's counters for an inline
function that more than one of them instantiates, so a check that only one test reaches
can read as unreached. This exports each binary's coverage and unions it: lines from
llvm-cov's lcov records (`DA`), covered when any binary executed them; branches from its
JSON export, each outcome taken when any binary took it.

Counts are taken on evidence. llvm-cov derives branch counts from counter expressions,
and an exception unwinding past an increment can leave an expression negative, read as a
count near 2^63. A count at or above 2^62 is no evidence. Such an outcome is taken only
when a build with single-byte coverage (`--confirm`, a flag per region and no
expressions) took it; otherwise it is listed as unconfirmed and counts as uncovered.

  coverage_union.py LLVM_BIN PROFDATA BINARY... [--confirm PROFDATA BINARY...] [--list]
      [--headers /include/prospect/]

`--headers` selects the headers measured, by path fragment (`/include/strata/core/` for
the core's, from the fetched strata). `--list` prints, per header, the uncovered lines and the branch outcomes not taken.
"""

from __future__ import annotations

import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

SATURATED = 1 << 62
HEADERS = "/include/prospect/"

Key = tuple[int, int, int]


def export(llvm: Path, profdata: str, binary: str, form: str) -> str:
    extra = ["-skip-expansions"] if form == "text" else []
    return subprocess.run(
        [
            str(llvm / "llvm-cov"),
            "export",
            binary,
            f"-instr-profile={profdata}",
            f"-format={form}",
            *extra,
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def measured_header(name: str) -> str:
    return name.rsplit("/", 1)[-1] if HEADERS in name else ""


class Build:
    """Per header: each line's and each branch outcome's counts over the binaries."""

    def __init__(self, llvm: Path, profdata: str, binaries: list[str]) -> None:
        self.lines: dict[str, dict[int, list[int]]] = defaultdict(
            lambda: defaultdict(list)
        )
        self.branches: dict[str, dict[Key, list[list[int]]]] = defaultdict(dict)
        for binary in binaries:
            short = ""
            for record in export(llvm, profdata, binary, "lcov").splitlines():
                if record.startswith("SF:"):
                    short = measured_header(record[3:])
                elif short and record.startswith("DA:"):
                    line, count = record[3:].split(",")[:2]
                    self.lines[short][int(line)].append(int(count))
            files: list[Any] = json.loads(export(llvm, profdata, binary, "text"))[
                "data"
            ][0]["files"]  # decoded JSON
            for f in files:
                short = measured_header(str(f["filename"]))
                if not short:
                    continue
                # Branches: [line, col, end_line, end_col, true, false, file, expanded, kind].
                for b in f["branches"]:
                    key = (int(b[0]), int(b[1]), int(b[3]))
                    outcome = self.branches[short].setdefault(key, [[], []])
                    outcome[0].append(int(b[4]))
                    outcome[1].append(int(b[5]))


def plausible(counts: list[int]) -> bool:
    return any(0 < c < SATURATED for c in counts)


def saturated(counts: list[int]) -> bool:
    return any(c >= SATURATED for c in counts)


def main() -> int:
    global HEADERS
    args = sys.argv[1:]
    if "--headers" in args:
        at = args.index("--headers")
        HEADERS = args[at + 1]
        del args[at : at + 2]
    listing = "--list" in args
    args = [a for a in args if a != "--list"]
    confirm_args: list[str] = []
    if "--confirm" in args:
        at = args.index("--confirm")
        args, confirm_args = args[:at], args[at + 1 :]
    llvm = Path(args[0])
    build = Build(llvm, args[1], args[2:])
    confirm = Build(llvm, confirm_args[0], confirm_args[1:]) if confirm_args else None

    for short in sorted(set(build.lines) | set(build.branches)):
        lines = build.lines[short]
        uncovered = []
        for line, counts in sorted(lines.items()):
            confirmed = confirm is not None and any(
                c > 0 for c in confirm.lines[short].get(line, [])
            )
            if not (plausible(counts) or confirmed):
                uncovered.append(line)
        taken = 0
        missing: list[str] = []
        unconfirmed = 0
        for key, (true, false) in sorted(build.branches[short].items()):
            other = confirm.branches[short].get(key) if confirm is not None else None
            for side, counts, name in ((0, true, "true"), (1, false, "false")):
                confirmed = other is not None and any(c > 0 for c in other[side])
                if plausible(counts) or confirmed:
                    taken += 1
                else:
                    unconfirmed += saturated(counts)
                    missing.append(
                        f"{key[0]}:{key[1]} {name}"
                        + (" (saturated)" if saturated(counts) else "")
                    )
        outcomes = 2 * len(build.branches[short])
        total = len(lines)
        print(
            f"{short:<18} lines {100 * (total - len(uncovered)) / max(total, 1):6.2f}%  "
            f"branches {100 * taken / max(outcomes, 1):6.2f}%  "
            f"({taken}/{outcomes}; {unconfirmed} saturated, unconfirmed)"
        )
        if listing:
            if uncovered:
                print(f"  uncovered lines: {', '.join(map(str, uncovered))}")
            if missing:
                print(f"  outcomes not taken: {'; '.join(missing)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
