# f13-gate: format 13's confirmation rows on the gate VM

- Task: the lead's confirmation of main `8110f1d` (format 13 with the query items) at 5M on the
  gate VM before the freeze: identity (C1), bytes (D15), merge times (C3), run-level queries
  (T4, against the baselines), and the owed rows (single-term `TOP_100`, peak memory).
- Branch / worktree: `perf/f13-gate` from main `8110f1d`, at
  `/Users/kronuz/code/Kronuz/strata-wt/f13-gate`. Record: `docs/perf/format13-gate-vm.md`.
- Gate VM layout: sources in `~/libs/f13g-{a,b,f12}` (A `026ee10`, B `8110f1d`, F12 `69a3f8c`),
  results and indexes in `~/bench/results/f13g/`.

## State

Plan written; builds next.

## Done

## Next

1. Builds of A, B, F12 and the review's `probe_search` for A and B.
2. Reordered indexes of A and B; rows 1 to 4 on them; then the merged layout; then row 5.

## Blocked on

Nothing.
