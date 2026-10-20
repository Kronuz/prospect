# f13-gate: format 13's confirmation rows on the gate VM

- Task: the lead's confirmation of main `8110f1d` (format 13 with the query items) at 5M on the
  gate VM before the freeze: identity (C1), bytes (D15), merge times (C3), run-level queries
  (T4, against the baselines), and the owed rows (single-term `TOP_100`, peak memory).
- Branch / worktree: `perf/f13-gate` from main `8110f1d`, at
  `/Users/kronuz/code/Kronuz/strata-wt/f13-gate`. Record: `docs/perf/format13-gate-vm.md`.
- Gate VM layout: sources in `~/libs/f13g-{a,b,f12}` (A `026ee10`, B `8110f1d`, F12 `69a3f8c`),
  results and indexes in `~/bench/results/f13g/`.

## State

Reordered layout in progress: A's reordering merge done, B's running (strict hold).

## Done

- Plan, ddaa0c9. Builds on the VM (Release, clang 18): A 58 s, B 64 s, F12 57 s; the review's
  `probe_search` built against A and B. Dry run at 20k: dumps identical, verify clean, the
  harness with all six engines, `bench/quick/engines_ab.py` scoring through `ab.py --aa`.
- 5M flushed indexes (16 segments, 256 MB buffer): A 132.6 s, 572 MiB peak; B 143.7 s, 573 MiB.
- A's reordering merge: 1,063 s, 4.76 GiB peak RSS; postings 845,553,044 bytes.

## Next

1. Builds of A, B, F12 and the review's `probe_search` for A and B.
2. Reordered indexes of A and B; rows 1 to 4 on them; then the merged layout; then row 5.

## Blocked on

Nothing.
