#!/usr/bin/env python3
"""Line and branch coverage of strata's headers, unioned over every test binary.

`llvm-cov report` with several binaries keeps one binary's counters for an inline
function that more than one of them instantiates, so a check that only one test reaches
can read as unreached. This exports each binary's coverage as lcov, llvm-cov's own line
(`DA`) and branch (`BRDA`) records, and unions them: a line counts as covered when any
binary executed it, and a branch outcome when any binary took it.

  coverage_union.py LLVM_BIN PROFDATA BINARY...
"""

from __future__ import annotations

import subprocess
import sys
from collections import defaultdict
from pathlib import Path


def export(llvm: Path, profdata: str, binary: str) -> str:
    return subprocess.run(
        [
            str(llvm / "llvm-cov"),
            "export",
            binary,
            f"-instr-profile={profdata}",
            "-format=lcov",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def main() -> int:
    llvm = Path(sys.argv[1])
    profdata = sys.argv[2]
    lines: dict[str, dict[int, bool]] = defaultdict(dict)
    branches: dict[str, dict[tuple[str, str, str], bool]] = defaultdict(dict)
    for binary in sys.argv[3:]:
        short = ""
        for record in export(llvm, profdata, binary).splitlines():
            if record.startswith("SF:"):
                name = record[3:]
                short = name.rsplit("/", 1)[-1] if "/include/strata/" in name else ""
            elif not short:
                continue
            elif record.startswith("DA:"):
                line, count = record[3:].split(",")[:2]
                seen = lines[short]
                seen[int(line)] = seen.get(int(line), False) or int(count) > 0
            elif record.startswith("BRDA:"):
                line, block, branch, taken = record[5:].split(",")
                key = (line, block, branch)
                seen_branch = branches[short]
                seen_branch[key] = seen_branch.get(key, False) or taken not in (
                    "-",
                    "0",
                )
    for short in sorted(set(lines) | set(branches)):
        covered, total = sum(lines[short].values()), len(lines[short])
        taken, outcomes = sum(branches[short].values()), len(branches[short])
        print(
            f"{short:<18} lines {100 * covered / max(total, 1):6.2f}%  "
            f"branches {100 * taken / max(outcomes, 1):6.2f}%"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
