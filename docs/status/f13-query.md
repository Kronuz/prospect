# f13-query: format 13's query freeze items (M16, M20, M22, M23, M3t)

- Design: docs/design/format-13.md (F10 to F13, "Choosing F10 to F13 together" F13-R8, F13-R44,
  F13-R45) on main 026ee10; the decisions are perf/query-exp's freeze table
  (`docs/perf/query-gap-format13.md` at fac1e8d) with `query-gap-skip-entries.md` (E6),
  `query-gap-seeds.md` (E4), `query-gap-m3.md` (E10), and `ab-run-level.md`; D14 and D15 in
  design/query-gap's `docs/design/query-gap.md`.
- Branch / worktree: phase4/f13-query from origin/main 026ee10, at
  /Users/kronuz/code/Kronuz/strata-wt/f13-query.
- Items, one commit or more each, each format change bumping `format::pre_freeze` (C17):
  1. M16: advance entries at A3@128, and the joint selection's record (F13-R8).
  2. M23: ordinal gaps stored as gap - 1, checked by the header width (25 bits) or a 64-bit sum.
  3. M22: Elias-Fano as a block ordinal codec, chosen per block by size.
  4. M20: two dominance pairs per k (10, 100, 1,000) per term with skip blocks; seeds off in
     segments with deletes.
  5. M3t: bound-ordered traversal for single terms in reordered segments (format-free).

## State

Starting: baseline built and tested on 026ee10 (build 112 s, ctest 36/36 in 86 s).

## Done

## Next

1. M16 (F10's split at A3@128).

## Blocked on

Nothing.

## Last verified

2026-10-19: ctest 36/36 at 026ee10 (macOS x86_64, Apple clang, RelWithDebInfo, quiet exit 0).
