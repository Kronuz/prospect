# f13-preflight: the seeds' writer cost and the loss attribution before the freeze

- Task (the lead's, 2026-10-19): two pre-freeze items for format 13 (main `939ee70` or later).
  1. Indexing: M20's `seeds_of` cheaper with byte-identical seed pairs; builds and docid merges
     within T5's 1.03 of `026ee10` at 1M (Mac), then a short 5M confirmation on the gate VM.
     Record: `docs/perf/format13-seeds-cost.md`.
  2. Attribution: each 5M loss class against Tantivy, PISA, Lucene and Lucene `-bp` classified
     as format-dependent (its place in format 13 named) or format-free (the algorithm named),
     with FQ3's phrase cost measured. Record: `docs/perf/format13-loss-attribution.md`.
- Branch / worktree: `perf/f13-preflight` from main `06a730b`, at
  `/Users/kronuz/code/Kronuz/strata-wt/f13-preflight`. Scratch: `.scratch/` (A = `026ee10` in
  `.scratch/wt-a`, B = main in `.scratch/wt-b`; phase times in `.scratch/phases.log`).

## Done criteria

- K1. Segment bytes identical to main's at 100k and 1M (flushed, docid merge, reordering merge);
  digest and fixtures unchanged; the test suite passes.
- K2. Builds and docid merges within 1.03 of `026ee10` at 1M on the Mac at the run level, A/A
  first; then two alternating 5M build pairs and one docid merge pair on the gate VM.
- K3. Every loss class named in the task attributed with counters and a profile, classified, and
  a final table: class, cause, format-dependent or not, the place it needs, expected gain.
- K4. FQ3's phrase cost with a measured estimate of a level-1 positional offset's saving.

## State

Started: plan written; waiting on a strict hold to build.

## Done

## Next

1. Instrumented count of `seeds_of`'s phases at 1M.

## Blocked on

Nothing.
