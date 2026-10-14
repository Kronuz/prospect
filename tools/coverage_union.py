#!/usr/bin/env python3
"""Line and branch coverage of strata's headers, unioned over every test binary.

`llvm-cov report` with several binaries keeps one binary's counters for an inline
function that more than one of them instantiates, so a check that only one test reaches
can read as unreached. This exports each binary's coverage and unions it: a line counts
as covered when any binary executed it, and a branch outcome when any binary took it.

  coverage_union.py LLVM_BIN PROFDATA BINARY...
"""

from __future__ import annotations

import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


def export(llvm: Path, profdata: str, binary: str) -> list[Any]:
    out = subprocess.run(
        [
            str(llvm / "llvm-cov"),
            "export",
            binary,
            f"-instr-profile={profdata}",
            "-skip-expansions",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    files: list[Any] = json.loads(out)["data"][0]["files"]  # decoded JSON
    return files


def main() -> int:
    llvm = Path(sys.argv[1])
    profdata = sys.argv[2]
    lines: dict[str, dict[int, bool]] = defaultdict(dict)
    branches: dict[str, dict[tuple[int, int, int], list[bool]]] = defaultdict(dict)
    for binary in sys.argv[3:]:
        for f in export(llvm, profdata, binary):
            name = str(f["filename"])
            if "/include/strata/" not in name:
                continue
            short = name.rsplit("/", 1)[-1]
            # Segments: [line, col, count, has_count, is_region_entry, is_gap].
            for seg in f["segments"]:
                line, _col, count, has_count, _entry, gap = seg
                if has_count and not gap:
                    lines[short][line] = lines[short].get(line, False) or count > 0
            # Branches: [line, col, end_line, end_col, true, false, file, expanded, kind].
            for b in f["branches"]:
                key = (int(b[0]), int(b[1]), int(b[3]))
                seen = branches[short].setdefault(key, [False, False])
                seen[0] |= b[4] > 0
                seen[1] |= b[5] > 0
    for short in sorted(set(lines) | set(branches)):
        covered = sum(lines[short].values())
        total = len(lines[short])
        taken = sum(t + f for t, f in branches[short].values())
        outcomes = 2 * len(branches[short])
        print(
            f"{short:<18} lines {100 * covered / max(total, 1):6.2f}%  branches {100 * taken / max(outcomes, 1):6.2f}%"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
