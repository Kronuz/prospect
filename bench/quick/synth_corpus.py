#!/usr/bin/env python3
"""A synthetic corpus and query sets for quick/ab.sh on a host without the benchmark game's data:
Zipfian words over a fixed seed, documents of 20 to 60 words, and queries over words of stratified
frequency (single words, OR, +AND, -NOT, phrases that occur).

  synth_corpus.py OUT_DIR [DOCS]    writes corpus.json, queries.txt and terms.txt (JSON lines, the harness's query format)
"""

import itertools
import os
import random
import sys


def main(out: str, docs: int) -> None:
    os.makedirs(out, exist_ok=True)
    r = random.Random(20261030)
    vocab = [f"w{i}" for i in range(30000)]
    weights = list(itertools.accumulate(1.0 / (i + 1) ** 1.05 for i in range(len(vocab))))
    seen_pairs: list[tuple[str, str]] = []
    with open(os.path.join(out, "corpus.json"), "w") as f:
        for n in range(docs):
            words = r.choices(vocab, cum_weights=weights, k=r.randint(20, 60))
            if len(seen_pairs) < 4000 and len(words) > 3:
                i = r.randrange(len(words) - 1)
                seen_pairs.append((words[i], words[i + 1]))
            f.write('{"id": "' + str(n) + '", "text": "' + " ".join(words) + '"}\n')
    stratum = [vocab[min(len(vocab) - 1, int(len(vocab) ** (k / 12)))] for k in range(1, 13)] + [vocab[i] for i in (0, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144)]
    pick = lambda: r.choice(vocab[: r.choice((30, 300, 3000, 30000))])
    queries = []
    for _ in range(60):
        queries.append(f"{pick()} {pick()}")
    for _ in range(40):
        queries.append(f"{pick()} {pick()} {pick()}")
    for _ in range(60):
        queries.append(f"+{pick()} +{pick()}")
    for _ in range(40):
        queries.append(f"+{pick()} {pick()}")
    for _ in range(40):
        queries.append(f"{pick()} -{pick()}")
    for a, b in r.sample(seen_pairs, 40):
        queries.append(f'"{a} {b}"')
    line = lambda q: '{"query": "' + q.replace('"', '\\"') + '", "tags": ["synthetic"]}'
    with open(os.path.join(out, "queries.txt"), "w") as f:
        f.write("\n".join(line(q) for q in queries) + "\n")
    with open(os.path.join(out, "terms.txt"), "w") as f:
        f.write("\n".join(line(q) for q in stratum) + "\n")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 300000)
