#!/usr/bin/env python3
"""Revert check for the negations' bulk top-k in prospect/search.hh (docs/perf/p7-exclusion.md):
applies mutant N (see MUTANTS), rebuilds search_test in ./build and runs it. The header is
restored. Exit 1 iff the test ran and failed (the mutant is caught); 0 if it survived or did not
build.

  negation_mutants.py N   (N indexes MUTANTS; "list" prints them)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/search.hh")

# (name, old, new); each old text occurs once in the header.
MUTANTS = [
    ("probe: a hit that could enter is never probed", "if ((m > 0 && s <= m) || probe_->excluded(ord)) { return; }", "if (m > 0 && s <= m) { return; }"),
    ("probe: a step back in ordinals does not restart the excluded clauses", "		if (d < last_) {", "		if (false) {"),
    ("nested: only the outermost excluded clause is probed", "for (exclusion_scorer* e = this; e; e = dynamic_cast<exclusion_scorer*>(e->in_.get())) {", "for (exclusion_scorer* e = this; e; e = nullptr) {"),
    ("probe: the excluded clause is taken as matching every hit it reaches", "if (e.doc() == d) { return true; }", "if (e.doc() >= d) { return true; }"),
]


def main(arg: str) -> int:
    if arg == "list":
        for i, (name, _, _) in enumerate(MUTANTS):
            print(i, name)
        return 0
    name, old, new = MUTANTS[int(arg)]
    original = open(header).read()
    if original.count(old) != 1:
        print(f"ERROR {name}: the text to mutate occurs {original.count(old)} times")
        return 0
    backup = header + ".orig"
    shutil.copy(header, backup)
    try:
        open(header, "w").write(original.replace(old, new))
        built = subprocess.run(["cmake", "--build", "build", "--target", "search_test", "-j4"], cwd=root, capture_output=True, text=True)
        if built.returncode:
            print(f"ERROR {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
            return 0
        test = subprocess.run([os.path.join(root, "build/search_test")], capture_output=True, text=True)
    finally:
        shutil.copy(backup, header)
        os.remove(backup)
    first = (test.stderr.strip().splitlines() or [f"exit {test.returncode}"])[0][:120]
    print(f"{'caught' if test.returncode else 'SURVIVED'} {name}: {first}")
    return 1 if test.returncode else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
