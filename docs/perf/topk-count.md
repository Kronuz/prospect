# TOP_100_COUNT unions of three or more terms: what is left after query-wins

Branch `explore/topk-count`, a prototype; not for landing as is.

## Finding 1: the loss was already fixed on main

Loss 2 of `format13-loss-attribution.md` (1.71 and 1.97 times slower than Tantivy) is the state
before `query-wins.md` row 1: counting kept every clause essential and scored every match (991,994
postings per four-term query). The single change that fixes most of it is the one that landed:
count from a fresh copy's window bits (`count()`), then run the MaxScore split from the collector's
threshold as uncounted top-k does. query-wins measured 0.461 and 0.345 of the old time (bm25),
0.390 and 0.291 (bm25_xapian), so about 0.78 and 0.68 of Tantivy's time by the 5M ratios, not
re-measured here. `include/prospect/search.hh` `disjunction_scorer::collect_top_k` is that code.

Main at cb36499, 1M reordered, one thread, average over the class of best of 5, loaded host
(two other light holds):

| Class | COUNT | TOP_100 | TOP_100_COUNT |
|---|---:|---:|---:|
| union >3 (20 queries) | 917 us | 2,590 us | 3,908 us |
| union 3 (83) | 385 us | 901 us | 1,353 us |

TOP_100_COUNT is about COUNT + TOP_100 + 10% (3,908 against 3,507 for >3): what remains is the
top-k scoring itself, plus a counting pass the scoring pass does not share.

## Experiment 1: keep the threshold seed while counting (win, small)

`top_k` zeroed the cross-segment/seed floor whenever it counted, because counting used to forbid
pruning. With counting apart from scoring (a fresh copy), the seed (F13, a strict lower bound on the
k-th score, `disjunction_scorer::seed`) can prune scoring from the first window. Change: a
`scorer::counts_apart()` (true for `disjunction_scorer`); `top_k` keeps `floor` for counted
queries when it holds, and the collector is seeded with it. 7 lines.

Identity: `dump` (counted and uncounted, 962 gate queries, k 10/100/1000, both presets, 5,772
lines) byte-identical to main at 100k and 1M, docid-merged and reordered; `verify` 0 mismatches
over the 962 at each. Multi-segment indexes were not run (that path is unchanged).

Timing: `bench/quick/ab.sh` (this tree's ab.py has no `--aa`), 1M reordered, TOP_100_COUNT only,
best of 3 per query, 10 interleaved rounds, class total of per-query bests, B/A (below 1 is B
faster). A/A is main against a copy of itself. Per-round spread in brackets.

| Class | bm25 A/A | bm25 A/B | bm25_xapian A/A | bm25_xapian A/B |
|---|---|---|---|---|
| union >3 | 0.991 [0.88-1.17] | 0.949 [0.89-1.01] | 1.000 [0.88-1.10] | 0.917 [0.91-1.06] |
| union 3 | 0.981 [0.90-1.13] | 0.931 [0.87-0.96] | 0.998 [0.91-1.13] | 0.847 [0.83-0.93] |

Union 3 is 7% and 15% faster in nearly every round; union >3 is 5% and 8% faster but its rounds
overlap the A/A band, so not claimed alone. An earlier 6-round bm25 session gave 0.856 and 0.943.
The host was loaded and noisy (A/A rounds up to 1.17), so the figures need a quiet-host run and
the run-level scorer (`ab.py --aa` on perf/query-exp) before landing. Other rows of the A/B moved
within the A/A band (phrase, intersection, intersection_union, union 2: 0.97 to 1.07 in single
sessions, both directions in A/A, and none of them runs the changed line).

## Not tried

- Fusing the counting pass into the scoring pass (one decode of each clause per window): the
  ~10% overhead above is the only upper bound; it breaks `count()`'s aligned bitset reuse.
- Block-max pruning of postings inside a window below the threshold: `collect_top_k` already
  drops a window whose bounds cannot compete and consults non-essential clauses only while a
  document can; per-posting skipping inside essential clauses is the remaining idea.

## Worth finishing?

Marginally: 7-line change, identical output, 5-15% on one class under a noisy host. Land it only
after the run-level A/B on a quiet host and a multi-segment identity run.
