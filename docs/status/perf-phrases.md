# perf/phrases ledger

## State
Round 2 of the phrase-query drill. Five experiments run, four kept (E1, E2, E3, E5), one removed
(E4). The target (at most 1.0 of Tantivy on `COUNT` and `TOP_10`) is not met: against Tantivy 0.26
on the 300 gate phrases, 1M reordered index, `COUNT` 1.425 -> 1.348 and `TOP_10` 1.357 -> 1.116.
No format change, no disk change. Measured on the Intel Mac only.

## Done
- Baseline and short-query profile; rebuilt Tantivy index and runner (7dc3131).
- E1 recycle per-query objects (d2e22c9), E2 two-slot match (95dfa26), E3 TOP_k bound from counts
  (e653fa8), E5 position-index sum (ab933b1); E4 weight tables removed as a loser.
- docs/perf/phrases-explore.md sections 4 to 7 (numbers, profile, pair-posting cost estimate).

## Next
- Decide on the pair posting (about +12% to +21% disk for parity on `COUNT`; section 7).
- Measure the landing version on Linux, FreeBSD, arm64.
- Optional build and `verify` diet, 1 to 3% each.

## Blocked on
Landing: the other platforms' measurements (the lead's hosts) and a code review of the
thread-local block stack (`recycled` in include/prospect/reader.hh) and of `can_compete`
(include/prospect/search.hh), which skips candidates in top-k searches before their positions are read.
