# query-wins: the loss attribution's two measured format-free wins, in production code

- Task (the lead's, 2026-10-20): rows 1 and 3 of `docs/perf/format13-loss-attribution.md`
  as production code, from the prototypes in `format13-loss-attribution-ff.patch`.
  1. Row 1: counted unions (`TOP_100_COUNT`, three or more clauses) count from window bits and
     prune the scoring as top-k does (prototype 2.09 and 2.75 times faster).
  2. Row 3: intersection_union prunes as Lucene's `ReqOptSumScorer` does: required/optional
     block skipping and the optional side turned required (prototype 1.14 to 1.22 times
     faster at `TOP_10`, `TOP_100`, `TOP_100_COUNT`).
- Branch / worktree: `perf/query-wins` from main `57a3091`, at
  `/Users/kronuz/code/Kronuz/strata-wt/query-wins`. Scratch: `.scratch/`.

## Done criteria

- Q1. Results identical to main: `dump` (top 10, 100, 1,000 with counts, both presets) over the
  962 gate queries on 100k and 1M indexes, docid-merged and reordered; `verify` 0 mismatches.
- Q2. Parity against the reference scorer (`parity`, recorded oracle ranks).
- Q3. Per fix, a test that fails with the fix reverted, and one mutant a test catches.
- Q4. ASan and UBSan over the suite.
- Q5. Run-level A/B against main at 1M, A/A first, both presets, every row: no row lost beyond
  noise; the two target rows at or near the prototype's gains.

## State

Implementing.

## Done

## Next

1. Row 1 in `disjunction_scorer::collect_top_k`; row 3 in `required_optional_scorer`.
2. Tests, revert checks, mutants, sanitizers.
3. Indexes (100k, 1M; merged and reordered) from main's binary; dump and verify identity.
4. A/A then A/B at 1M under a light hold.

## Blocked on

Nothing.
