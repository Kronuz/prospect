#!/usr/bin/env python3
"""Queries for T3b from a terms dump (xapiand_terms.py): lines "KIND<TAB>hex hex ...".

  terms_queries.py TERMS [N] > queries.txt

Single terms, ORs of terms within one field and across fields, and ANDs of terms of one
document within and across fields (so they match), seeded and deterministic. Q (the key) and
V (the version) are not queried. The field of a term is split as fieldsize.py does.
"""
import random
import sys
from collections import defaultdict


def field_of(t):
    i = 1 if t[:1] == b"Z" else 0
    while i < len(t):
        if 65 <= t[i] <= 90:
            return t[: i + 1]
        i = t.index(b".", i) + 1
    raise ValueError(t)


def main():
    path = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 240
    docs, df = defaultdict(list), defaultdict(int)
    for line in open(path):
        doc, _, h = line.rstrip("\n").split("\t")
        t = bytes.fromhex(h)
        f = field_of(t)
        if f in (b"Q", b"V"):
            continue
        docs[doc].append((f, h))
        df[h] += 1
    names = sorted(docs)
    by_field = defaultdict(list)
    for h, c in sorted(df.items()):
        by_field[field_of(bytes.fromhex(h))].append(h)
    fields = sorted(by_field)
    rng = random.Random(28)
    out = []
    while len(out) < n:
        kind = len(out) % 6
        if kind == 0:  # a term
            f = rng.choice(fields)
            out.append("TERM\t" + rng.choice(by_field[f]))
        elif kind == 1:  # OR within a field
            f = rng.choice(fields)
            k = min(len(by_field[f]), rng.randint(2, 3))
            out.append("OR\t" + " ".join(rng.sample(by_field[f], k)))
        elif kind == 2:  # OR across fields
            fs = rng.sample(fields, rng.randint(2, 3))
            out.append("OR\t" + " ".join(rng.choice(by_field[f]) for f in fs))
        else:  # AND of a document's terms: kind 3 and 5 across fields, 4 within a field
            d = docs[rng.choice(names)]
            if kind == 4:
                g = defaultdict(list)
                for f, h in d:
                    g[f].append(h)
                big = [v for v in g.values() if len(v) >= 2]
                if not big:
                    continue
                out.append("AND\t" + " ".join(rng.sample(rng.choice(big), 2)))
            else:
                g = {}
                for f, h in d:
                    g.setdefault(f, h)
                if len(g) < 2:
                    continue
                out.append("AND\t" + " ".join(rng.sample(sorted(g.values()), min(len(g), rng.randint(2, 3)))))
    print("\n".join(out))


main()
