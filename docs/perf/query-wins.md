# The loss attribution's two format-free wins, landed as code

Rows 1 and 3 of `format13-loss-attribution.md`, measured there as prototypes
(`format13-loss-attribution-ff.patch`), written as production code on `perf/query-wins`.

## The changes

1. **Counted unions prune** (row 1). `disjunction_scorer::collect_top_k` kept every clause
   essential and scored every match whenever the collector counted (`TOP_100_COUNT` with three
   or more clauses; two clauses take the exact count by inclusion and exclusion). It now counts
   from a fresh copy's window bits with `count()`, the same windows `COUNT` fills, and then
   collects as an uncounted top-k does: the MaxScore split from the collector's threshold, and
   non-essential clauses consulted only while a document can compete. A union whose clauses
   cannot make a fresh copy counts inline as before. The prototype interleaved the counting
   copies window by window inside the scoring loop; counting first through `count()` reuses
   its 4,096-aligned windows (where bitset blocks are OR'd whole) and keeps the scoring loop
   as top-k has it.
2. **Required plus optional skips blocks** (row 3), ported from Lucene 10.4's
   `ReqOptSumScorer` (Apache License 2.0; `NOTICE`, `LICENSES/Apache-2.0.txt`). A window ends at
   the required side's block or the optional side's, or just before the optional side's next
   match, where the optional side adds nothing (`advanceShallow`). A window whose two bounds
   cannot compete is skipped; where the required bound alone cannot, the optional side becomes
   required and the two leapfrog, across windows when the required side can never compete
   alone (`upperBound`). Inside a window the per-document rule stays: a required match whose
   score plus the optional bound cannot compete is skipped before the optional side moves.
   Rounding follows the scorer's: every comparison bounds a two-term sum with `sum_upper`.

## Proof

Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, Release unless noted. A is main
`57a3091`, B this branch.

### Tests, revert checks, mutants

`search_test` gains a scoring tally (a scorer that forwards and counts `score()` calls; window
scoring goes through it, so every posting scored counts). On 100,000 documents, docid-ordered
and reordered, at k 10 and 100: a counted union of four clauses (one rare, three at 40%)
returns the exhaustive scan's best k exactly (ordinals and scores, ties by docid) and its exact
count, scoring under half of its matches; a required term at 50% with an optional one at 2%
returns the scan's best k scoring under half of the required documents.

| Check | Result |
|---|---|
| Row 1 reverted | the tally check fails, 4 of 4 (two segments, k 10 and 100) |
| Row 3 reverted | the tally check fails, 4 of 4 |
| M1: count as `copy->cost()` | caught: counted top-k's count (random corpus), 21 failures |
| M1b: count also inline | caught: counted top-k's count, 21 failures |
| M3: skip a window on the required bound alone | caught: pruned top-k against brute force |
| M3b: leapfrog past the window when the required side competes alone | caught: counted and pruned top-k |
| M3c: the optional bound zero at `>=` its next match | caught: pruned and floored top-k |
| Release suite | 39 of 39 |
| ASan + UBSan (Homebrew LLVM, `-O1`) | `search`, `exact`, `multisegment`, `engine_query` pass; `strata_engine verify` over the 962 gate queries at 100k (reordered; 9 flushed segments) 0 mismatches, no reports |
| CI | run 39438785455 (`c507f2a`): all eight jobs green, `linux-asan-ubsan` and `linux-tsan` included |

### Identity and parity

`strata_engine verify` now checks the uncounted search as well as the counted one, and
`STRATA_DUMP_COUNT=0` dumps the uncounted path (`c507f2a`). Indexes built once with A's binary
from the first N corpus documents (identity seed 1): 100k (4 flushed segments) and 1M (32),
each docid-merged and reordered (4 threads); 100k also flushed, 9 segments.

| Index | dump, counted | dump, uncounted | verify (B) |
|---|---|---|---|
| 100k merged | identical (5,772 lines) | identical | 962 queries, 0 mismatches |
| 100k reordered | identical | identical | 0 mismatches |
| 100k flushed, 9 segments | identical | identical | 0 mismatches |
| 1M merged | identical | identical | 0 mismatches |
| 1M reordered | identical | identical | 0 mismatches |

Each dump is every gate query at k 10, 100, and 1,000 under both presets (docids and exact
scores), and B's counted and uncounted hit lists agree line for line. Parity against the
oracle's rank vectors (419 queries over 10,000 documents, counts and top 10, counted and
pruned): 419 of 419 in one segment, in four, and reordered (`PARITY_REORDER=2`), worst
relative error 3.87e-16 (the format-13 merge review recorded the same).

## Run-level A/B at 1M
