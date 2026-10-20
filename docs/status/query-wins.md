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

Done; not merged. Q1 to Q5 met (`docs/perf/query-wins.md`). Branch pushed; CI dispatched on
the final head.

## Done

- `5bcc81a` search: counted unions prune; required plus optional skips blocks (rows 1 and 3,
  NOTICE and the Apache License for the Lucene port, search_test's scoring tally).
- `c507f2a` strata_engine: verify checks the uncounted search too; `STRATA_DUMP_COUNT=0`.
- `3b5a311` search: required plus optional windows are the required side's blocks. Lucene's
  finer windows (either side's block end) measured slower: intersection_union `TOP_1000`
  1.063 [1.020-1.107], `TOP_10` 0.904 against the prototype's 0.820.
- Q1: dumps identical to main with and without a count (both presets, k 10, 100, 1,000) at
  100k and 1M, merged and reordered, and on nine 100k segments; verify 0 mismatches on each.
- Q2: parity 419 of 419 in one segment, four, and reordered; worst error 3.87e-16.
- Q3: each row reverted fails search_test's tally check (4 of 4); five mutants caught
  (M1, M1b, M3, M3b, M3c), rerun on `3b5a311`.
- Q4: ASan and UBSan (Homebrew LLVM, `-O1`): search, exact, multisegment, engine_query, and
  verify over the gate queries at 100k (one and nine segments) clean; CI's linux-asan-ubsan
  green on `c507f2a` and `3b5a311`.
- Q5: 1M reordered, 16 rounds, A/A first, both presets: no row lost. `TOP_100_COUNT` union of
  3 terms 0.461 and 0.390 (bm25, bm25_xapian), more than 3 0.345 and 0.291; intersection_union
  `TOP_10` 0.859 and 0.764, `TOP_100` 0.890 and 0.848, `TOP_100_COUNT` 0.878 and 0.837,
  `TOP_1000` 0.997 and 1.000.
- CI: run 39438785455 (`c507f2a`) all eight jobs green; run 39448276304 (`3b5a311`) seven of
  eight green when last read, linux-tsan running.

## Next

1. The lead: review and merge `perf/query-wins`; confirm on the gate VM at 5M if wanted.
2. Follow-ups the attribution names, not in scope here: row 2 (fused FOR decode into window
   bits), row 5's counting rule for conjunctions, row 7 (exclude by window bits).

## Blocked on

Nothing.
