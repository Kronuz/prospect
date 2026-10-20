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

### Tests, revert checks, mutants

## Identity and parity

## Run-level A/B at 1M
