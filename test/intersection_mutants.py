#!/usr/bin/env python3
"""Revert check for the block compare and the bitset rank in prospect/reader.hh: applies mutant
N (see MUTANTS), rebuilds intersection_test in ./build and runs it. The header is restored.
Exit 1 iff the test ran and failed (the mutant is caught); 0 if it survived or did not build.

  intersection_mutants.py N   (N indexes MUTANTS; "list" prints them)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/reader.hh")

# (name, old, new); each old text occurs once in the header.
MUTANTS = [
    ("compare: a slot equal to the target counts as below", "n += std::uint32_t(buf_ords_[i] < target);", "n += std::uint32_t(buf_ords_[i] <= target);"),
    ("probe: a slot equal to the target is passed over", "if (buf_ords_[p] >= target) { return p; }", "if (buf_ords_[p] > target) { return p; }"),
    ("probe: the first slot after pos_ is not probed", "for (std::size_t p = pos_ + 1; p < probes; ++p)", "for (std::size_t p = pos_ + 2; p < probes; ++p)"),
    ("compare: only the first 16 slots are compared", "const std::size_t end = (buf_len_ + 15) & ~std::size_t(15);", "const std::size_t end = 16;"),
    ("compare: a block's slots past its end are not filled", "			std::fill(buf_ords_ + n, buf_ords_ + format::block, no_more_docs);\n", ""),
    ("rank: the word's own rank is not added", "		rank += bits_rank_[w];\n", ""),
    ("rank: a word's rank includes the word", "			bits_rank_[w] = std::uint8_t(n);\n			n += std::size_t(std::popcount(word));", "			n += std::size_t(std::popcount(word));\n			bits_rank_[w] = std::uint8_t(n);"),
    ("rank: the rank is that of the word before", "		rank += bits_rank_[w];\n", "		rank += w ? bits_rank_[w - 1] : 0;\n"),
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
        built = subprocess.run(["cmake", "--build", "build", "--target", "intersection_test", "-j4"], cwd=root, capture_output=True, text=True)
        if built.returncode:
            print(f"ERROR {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
            return 0
        test = subprocess.run([os.path.join(root, "build/intersection_test")], capture_output=True, text=True)
    finally:
        shutil.copy(backup, header)
        os.remove(backup)
    first = (test.stderr.strip().splitlines() or [f"exit {test.returncode}"])[0][:120]
    print(f"{'caught' if test.returncode else 'SURVIVED'} {name}: {first}")
    return 1 if test.returncode else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
