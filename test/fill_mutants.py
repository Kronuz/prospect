#!/usr/bin/env python3
"""Revert check for postings_iterator::fill: applies one mutant at a time to include/prospect/reader.hh,
rebuilds fill_test (or fill_slow_test for the fallback's), and expects the test to fail. The header is
restored after each. A survivor, or a mutant that does not build, is an error.

  fill_mutants.py BUILD_DIR            (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/reader.hh")

OLD_SCATTER = """		std::uint64_t sums[25] = {};
		const std::uint32_t w0 = o[0] >> 5;
		std::uint64_t c = 0;
		for (std::size_t i = 0; i < n; ++i) {
			const std::uint32_t d = o[i];
			c += bit_masks[d & 31];
			sums[(d >> 5) - w0] = c;
		}
		std::uint64_t prev = 0;
		for (std::uint32_t k = 0, hi = (o[n - 1] >> 5) - w0; k <= hi; ++k) {
			const std::uint64_t x = sums[k], delta = (x - prev) & -std::uint64_t(x != 0);
			bits[(w0 + k) >> 1] |= delta << (32 * ((w0 + k) & 1));
			prev += delta;
		}
"""
FIRST_SCATTER = """		std::uint64_t sums[25] = {};
		const std::uint32_t w0 = o[0] >> 6;
		std::uint64_t c = 0;
		for (std::size_t i = 0; i < n; ++i) {
			const std::uint32_t d = o[i];
			c += bit_masks[d & 63];
			sums[(d >> 6) - w0] = c;
		}
		std::uint64_t prev = 0;
		for (std::uint32_t k = 0, hi = (o[n - 1] >> 6) - w0; k <= hi; ++k) {
			const std::uint64_t x = sums[k], delta = (x - prev) & -std::uint64_t(x != 0);
			bits[w0 + k] |= delta;
			prev += delta;
		}
"""

# (name, path, old, new, target)
MUTANTS = [
    ("walk: a block ending at the window's end is taken", "entries", "if (next.last >= end) { return; }", "if (next.last > end) { return; }", "fill_test"),
    ("walk: the cached entry is filed under the wrong block", "entries", "checked_b_ = next_block_;\n				if (next.last", "checked_b_ = next_block_ - 1;\n				if (next.last", "fill_test"),
    ("walk: an entry's last need not rise", "entries", "last > gr.last || last <= r.last || start >= stop", "last > gr.last || start >= stop", "fill_test"),
    ("fused unpack: widths up to 32 bits", "wrap", "|| width > 25 ||", "|| width > 32 ||", "fill_test"),
    ("step-1 run: last bit missing", "range", "set_range(r.base + 1 - base, r.last - base, bits);", "set_range(r.base + 1 - base, r.last - base - 1, bits);", "fill_test"),
    ("step-1 run: last word overwrites", "range", "bits[wh] |= mh;", "bits[wh] = mh;", "fill_test"),
    ("scatter: the first 64-bit sum with a zero for untouched", "wrap", OLD_SCATTER, FIRST_SCATTER, "fill_test"),
    ("scatter: a word's total is not taken from the sum before it", "sum", "			prev += delta;\n", "", "fill_test"),
    ("scatter: the sums go back into the iterator", "size", "	ordinal_t bits_first_ = 0, bits_doc0_ = 0, bits_last_ = 0;\n", "	std::uint64_t word_sum_[128] = {};\n	ordinal_t bits_first_ = 0, bits_doc0_ = 0, bits_last_ = 0;\n", "fill_test"),
    ("scatter: sparse bits shifted", "sparse", "bits[o[i] >> 6] |= bit_masks[o[i] & 63];", "bits[o[i] >> 6] |= bit_masks[(o[i] + 1) & 63];", "fill_test"),
    ("scatter: a consecutive run loses its last bit", "consecutive", "set_range(o[0], o[n - 1], bits);", "set_range(o[0], o[n - 1] - (n > 1), bits);", "fill_test"),
    ("bitset: the shifted-out bits are dropped", "bitset", "carry = shift ? word >> (64 - shift) : 0;", "carry = 0;", "fill_test"),
    ("bitset: the last word is not masked to the span", "bitset", "if (w + 1 == words && span % 64) { word &= (std::uint64_t(1) << (span % 64)) - 1; }", "", "fill_test"),
    ("crossing block: the window's base rounded down to a word", "drain", "o[k - i] = buf_ords_[k] - base;", "o[k - i] = buf_ords_[k] - (base & ~ordinal_t(63));", "fill_test"),
    ("fused unpack: the ramp is one short", "fused", "o[i] += std::uint32_t(i + 1);", "o[i] += std::uint32_t(i);", "fill_test"),
    ("fallback: ordinals are not made relative to the window", "fallback", "for (std::size_t i = 0; i < format::block; ++i) { o[i] -= base; }", "", "fill_slow_test"),
]


def run(cmd, **kw):
    prefix = [] if os.environ.get("NICE") == "0" else ["nice", "-n", "19"]
    return subprocess.run(prefix + cmd, capture_output=True, text=True, **kw)


def main(build: str) -> int:
    jobs = os.environ.get("JOBS", "1")
    original = open(header).read()
    backup = header + ".orig"
    shutil.copy(header, backup)
    bad = 0
    try:
        for name, path, old, new, target in MUTANTS:
            if original.count(old) != 1:
                print(f"ERROR   {name}: the text to mutate occurs {original.count(old)} times")
                bad += 1
                continue
            open(header, "w").write(original.replace(old, new))
            built = run(["cmake", "--build", build, "--target", target, "-j", jobs])
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                bad += 1
                continue
            test = run([os.path.join(build, target)])
            if test.returncode:
                print(f"caught  {name} ({path}): {(test.stderr.strip().splitlines() or ['exit ' + str(test.returncode)])[0][:110]}")
            else:
                print(f"SURVIVED {name} ({path})")
                bad += 1
    finally:
        shutil.copy(backup, header)
        os.remove(backup)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
