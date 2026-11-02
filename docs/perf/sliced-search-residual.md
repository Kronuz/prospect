# Sliced search: the residual cost of a query that fits one slice

Branch `perf/sliced-residual` from main `prospect:f817398`. The sliced search (`prospect:c77d6eb`, `docs/status/phase7c-l4.md` S6) costs
1.4-2.2% on multi-term queries against the unsliced loops (`prospect:0d7c43d`), also on a scheduler that never yields;
single words are within noise. The 7c gate's T12 compares throughput against pre-7c.

## Method

- A = `prospect:0d7c43d` (the last commit before the merge) with main's library pins, so only the search code differs;
  B = the branch. Release build, Apple clang, Intel Mac, synthetic 300,000-document index
  (`bench/quick/synth_corpus.py`, one segment), the 280 multi-term and 21 single-word queries.
- `bench/quick/ab.sh` interleaved, sides alternating by round, best of RUNS per query and round, geometric mean of B/A
  over queries; under `quiet strict`. A/A is the same binary on both sides.
- Per class (from the query text): conjunction `+a +b`, required/optional `+a b`, exclusion `a -b`, union of 2 and of 3,
  phrase. Profiles: `sample` over one class at a time, self time by function.

## Baseline (A = pre-sliced, B = main)

See the table of E0.

## Experiments

(each: hypothesis, procedure, keep/remove rule, before/after table, verdict)
