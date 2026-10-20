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

Item 1 (seeds): built, byte-identical at 100k and 1M, and at A's time on the Mac at 1M (13
trials pooled: builds 1.002, docid merges 1.005 of `026ee10`'s CPU; main 1.10 and 1.09). The 5M
gate VM confirmation is running. Item 2 (attribution): plan written, a counting build of main
with the counters ported (`.scratch/wt-cnt`), a profile build, and the level-1 positional
offset's scratch build (`.scratch/wt-pos`) ready.

## Done

- Plans: eaccf2a (seeds), 22568bf (attribution).
- 7fc0054: `strata_engine merge` honors `STRATA_IDENTITY_SEED`, so merged segments compare byte
  for byte.
- 5e7a426 (C1): `seeds_of` from exact and per-16 length counts over levels, a branchless split
  of wdf 1 from the rest; the same pairs. postings_test pins the pairs exactly, lengths past the
  65,536 cap and wdfs past 65,536; four mutants fail it, two equivalent ones survive.
- 59d9d08 (C2): one length gather per skip-block term, shared by the whole-list pairs, the
  blocks' pairs, and the seeds.
- K1: segments identical to main's (cmp, identity seed 1) at 100k (51 flushed segments, docid and
  reordering merges) and 1M (6 flushed, both merges), for C1 and C2; postings_test passes
  (macOS x86_64, Apple clang, Release).
- K2 on the Mac: 1M, one thread, CPU from `/usr/bin/time -l`, two sessions (5 and 8 trials,
  rotated): B/A 1.103 [1.080-1.127] build, 1.085 [1.063-1.107] merge; C2 against the mean of A
  and A' 1.002 [0.961-1.045] and 1.005 [0.953-1.059]; A'/A 0.995 and 0.988. In situ the new
  `seeds_of` takes 15 cycles per posting (2.3% of the build, 2.7% of the merge) against 59.
  One session (22:14) was aborted at host load 157 to 195 and discarded.

## Next

1. The 5M confirmation's numbers (gate VM, running).
2. Counters and profiles per loss class at 1M; the format-free A/Bs; FQ3's offset A/B.

## Blocked on

Nothing.
