# perf/phrases ledger

## State
Round 2 started from explore/phrases (0555d10). Baseline re-measured against a rebuilt Tantivy
0.26 index: TOP_10 1.369, COUNT 1.438 (see docs/perf/phrases-explore.md, section 4).

## Done
- Baseline, short-query profile, build-phase timers (docs/perf/phrases-explore.md section 4).

## Next
- E1 build diet, E2 two-term path, E3 TOP_10 per-candidate bound, E4 pair posting prototype.

## Blocked on
Nothing.
