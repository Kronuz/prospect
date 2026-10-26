#!/usr/bin/env python3
"""buckets.py OUT REF A B CMD [N]: geometric mean B/A within N buckets of queries ordered by REF's
best time (REF is a variant name in OUT, e.g. tv), to show where a change lands."""
import glob, math, os, sys
out, ref, a, b, cmd = sys.argv[1:6]
n = int(sys.argv[6]) if len(sys.argv) > 6 else 5
def best(side):
    d = {}
    for p in glob.glob(f"{out}/*-{side}-{cmd}.tsv"):
        for line in open(p):
            t, _, q = line.rstrip("\n").split("\t", 2)
            d[q] = min(d.get(q, 1e18), float(t))
    return d
R, A, B = best(ref), best(a), best(b)
qs = sorted(set(R) & set(A) & set(B), key=R.get)
for i in range(n):
    s = qs[i * len(qs) // n:(i + 1) * len(qs) // n]
    gm = math.exp(sum(math.log(B[q] / A[q]) for q in s) / len(s))
    print(f"bucket {i + 1}/{n}: {ref} mean {sum(R[q] for q in s) / len(s):8.1f} us  {b}/{a} geomean {gm:.3f}  sums {sum(B[q] for q in s) / sum(A[q] for q in s):.3f}")
