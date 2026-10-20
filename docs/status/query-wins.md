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

Code, tests, revert checks, and mutants done (Q3); identity, parity, sanitizers, and the A/B
wait on the host (strict holds from 07:16).

## Done

- `5bcc81a` search: counted unions prune; required plus optional skips blocks (rows 1 and 3,
  NOTICE and the Apache License for the Lucene port, search_test's scoring tally).
- `c507f2a` strata_engine: verify checks the uncounted search too; `STRATA_DUMP_COUNT=0`.
- Q3: with row 1 reverted, search_test's tally check fails (4 of 4: two segments, k 10 and
  100); with row 3 reverted, likewise (4 of 4). Mutants caught: M1 (`count(*copy)` as
  `copy->cost()`), M1b (count also inline), M3 (window skip on the required bound alone),
  M3b (leapfrog past the window when the required side competes alone), M3c (`>=` in the
  optional bound's zero case).
- Full Release suite: 39 of 39 pass (macOS x86_64).
- Pushed; CI run 39438785455 (push of `c507f2a`).

## Next

1. Indexes (100k, 1M; merged and reordered) from main's binary (`.scratch/build_idx.sh`).
2. Identity (`.scratch/identity.sh`): dumps with and without a count, both presets; verify.
3. Parity against the oracle ranks (one segment, four, reordered).
4. ASan and UBSan locally (CI's sanitizer job too).
5. A/A then A/B at 1M reordered, both presets (`.scratch/session.sh`), light hold.

## Blocked on

Nothing.
