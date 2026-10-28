#!/usr/bin/env python3
"""A search-benchmark-game corpus (JSON lines: id, text, sort_field) as a terms dump for T1.

  wiki_terms.py CORPUS [LIMIT] > wiki.terms

Terms as the design's probe (terms.py) names them: `text.S` and a word; `sort_field.N` and the
number's order-preserving form, and one field per accuracy level (`sort_field.<64>.N` ...). The
dump has the lines xapiand_terms.py writes: "doc<TAB>wdf<TAB>hex(term)"; wdf counts a word's
occurrences. The id is the document key, not a term.
"""
import json
import re
import struct
import sys
from collections import Counter

WORD = re.compile(r"[a-z0-9]+")
ACC = [100, 1000, 10000, 100000, 1000000, 100000000]


def num(v):
    b = struct.unpack(">Q", struct.pack(">d", float(v)))[0]
    b = b ^ 0xFFFFFFFFFFFFFFFF if b >> 63 else b | (1 << 63)
    return struct.pack(">Q", b)


def main():
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else None
    out = sys.stdout
    with open(sys.argv[1]) as f:
        for n, line in enumerate(f):
            if limit is not None and n >= limit:
                break
            d = json.loads(line)
            doc = f"0:{n}"
            for w, c in sorted(Counter(WORD.findall(d["text"].lower())).items()):
                out.write(f"{doc}\t{c}\t{(b'text.S' + w.encode()).hex()}\n")
            v = d["sort_field"]
            out.write(f"{doc}\t1\t{(b'sort_field.N' + num(v)).hex()}\n")
            for a in ACC:
                out.write(f"{doc}\t1\t{(f'sort_field.<{a:x}>.N'.encode() + num(int(v) - int(v) % a)).hex()}\n")


main()
