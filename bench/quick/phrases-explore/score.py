#!/usr/bin/env python3
"""score.py OUT A B [CMD...]: geometric mean over queries of B's best time over A's (best over
rounds), 95% bootstrap interval over queries, per-round ratios, sums, and result mismatches."""
import glob, math, os, random, sys
out, a, b = sys.argv[1:4]
cmds = sys.argv[4:] or ["TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT", "COUNT"]
def load(side, cmd):
    runs = {}
    for p in glob.glob(f"{out}/*-{side}-{cmd}.tsv"):
        r = int(os.path.basename(p).split("-")[0])
        runs[r] = {}
        for line in open(p):
            t, res, q = line.rstrip("\n").split("\t", 2)
            runs[r][q] = (float(t), res)
    return runs
rng = random.Random(7)
for cmd in cmds:
    A, B = load(a, cmd), load(b, cmd)
    if not A or not B: continue
    qs = sorted(set.intersection(*[set(x) for x in list(A.values()) + list(B.values())]))
    best = lambda R: {q: min(r[q][0] for r in R.values()) for q in qs}
    ba, bb = best(A), best(B)
    logs = [math.log(max(bb[q], .1) / max(ba[q], .1)) for q in qs]
    gm = math.exp(sum(logs) / len(logs))
    boot = sorted(math.exp(sum(rng.choice(logs) for _ in logs) / len(logs)) for _ in range(1000))
    pr = []
    for r in sorted(set(A) & set(B)):
        l = [math.log(max(B[r][q][0], .1) / max(A[r][q][0], .1)) for q in qs]
        pr.append(math.exp(sum(l) / len(l)))
    mism = sum(1 for q in qs if A[next(iter(A))][q][1] != B[next(iter(B))][q][1])
    print(f"{cmd:14} {b}/{a} {gm:.3f} [{boot[25]:.3f}, {boot[974]:.3f}]  rounds {' '.join(f'{x:.2f}' for x in pr)}  sums {sum(ba.values())/1e3:.1f} -> {sum(bb.values())/1e3:.1f} ms  mism {mism}  n={len(qs)}")
