#!/usr/bin/env python3
"""Line and branch coverage of strata's headers, unioned over every test binary.

`llvm-cov report` with several binaries keeps one binary's counters for an inline
function that more than one of them instantiates, so a check that only one test reaches
can read as unreached. This exports each binary's coverage and unions it: lines from
llvm-cov's lcov records (`DA`), covered when any binary executed them; branches from its
JSON export, each outcome taken when any binary took it.

  coverage_union.py LLVM_BIN PROFDATA BINARY...
"""

from __future__ import annotations

import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


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


def strata_header(name: str) -> str:
    return name.rsplit("/", 1)[-1] if "/include/strata/" in name else ""


def main() -> int:
    llvm = Path(sys.argv[1])
    profdata = sys.argv[2]
    lines: dict[str, dict[int, bool]] = defaultdict(dict)
    branches: dict[str, dict[tuple[int, int, int], list[bool]]] = defaultdict(dict)
    for binary in sys.argv[3:]:
        short = ""
        for record in export(llvm, profdata, binary, "lcov").splitlines():
            if record.startswith("SF:"):
                short = strata_header(record[3:])
            elif short and record.startswith("DA:"):
                line, count = record[3:].split(",")[:2]
                seen = lines[short]
                seen[int(line)] = seen.get(int(line), False) or int(count) > 0
        files: list[Any] = json.loads(export(llvm, profdata, binary, "text"))["data"][
            0
        ]["files"]  # decoded JSON
        for f in files:
            short = strata_header(str(f["filename"]))
            if not short:
                continue
            # Branches: [line, col, end_line, end_col, true, false, file, expanded, kind].
            for b in f["branches"]:
                key = (int(b[0]), int(b[1]), int(b[3]))
                outcome = branches[short].setdefault(key, [False, False])
                outcome[0] |= b[4] > 0
                outcome[1] |= b[5] > 0
    for short in sorted(set(lines) | set(branches)):
        covered, total = sum(lines[short].values()), len(lines[short])
        taken = sum(t + f for t, f in branches[short].values())
        outcomes = 2 * len(branches[short])
        print(
            f"{short:<18} lines {100 * covered / max(total, 1):6.2f}%  "
            f"branches {100 * taken / max(outcomes, 1):6.2f}%"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
