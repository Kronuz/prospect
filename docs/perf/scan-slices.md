# Exhaustive scan: 256-ordinal windows when the host works per match

`exhaustive_search::scan` (`include/prospect/shards.hh`) checks its slice once per window of ordinals. The host's
visitor (aggregations) and key (doc-value sorts) run per match inside the window, so with a 4,096-ordinal window a
Xapiand loop stayed in one turn for about 2.3 ms at 0.56 us of aggregation per match, whatever the 200 us slice said
(Xapiand `docs/perf/x7-loop-stalls.md`, cause 2, job 261111-221539-18ce81). With a visitor, or a key that runs
(k > 0), the window is now 256 ordinals (`host_window_ordinals`): at most 256 host calls per turn, about 0.14 ms at
that cost. A scan without host work (a descending docid order alone) keeps 4,096.

## Proof of the bound

`sliced_test` `host_windows()` runs an exhaustive scan on a 0 ns slice over 60,000 documents in two shards with
deletions, for a visitor feeding aggregations, a key, both, and neither. It counts the host calls of every resume:
at most 256 per resume and at least one yield per 256 matches, with hits, counts and aggregates equal to the
unsliced search; with no host work, no more yields than 4,096-ordinal windows. The test fails with the window set
back to 4,096 for host work: job 261112-003708-c3d2d6 (`most() <= 256` and `yields() * 256 >= matches`). Suite:
build-test 261112-003707-96568a at prospect:ca65df9.

## Cost: before and after

- B = prospect:ca65df9; A = base prospect:0aa68f4's `shards.hh` compiled into the same `sliced_test` source (via
  `-iquote`), so only the header under test differs. Release, the host's Apple clang, x86_64 binaries (`file`).
- Host: macOS, Intel Core i9-9980HK. One strict job, 261112-003722-18be3b (build 261112-003718-87dd09).
- `sliced_test bench 150000 21`: TERM `a` over one segment of 150,000 documents, 62,440 matches, k = 10. Each run
  prints, per arm, the median ns per match of 21 timed scans after a warm-up. Arms: plain (descending docids, no
  host work), count (a visitor that adds the ordinal), agg (the visitor feeding the test's terms, stats, histogram
  and avg aggregations); each unsliced and on a 200 us slice whose resumes run at once.
- 8 rounds of A, B and A again (A2), order alternating by round. Ratios are medians over rounds; A2/A is the
  same binary against itself.

| Arm | A ns/match | B ns/match | B/A | A2/A |
|---|---:|---:|---:|---:|
| plain, unsliced | 21.00 | 21.16 | 1.008 | 0.985 |
| plain, sliced | 20.38 | 20.68 | 1.015 | 1.006 |
| count, unsliced | 21.46 | 22.65 | 1.056 | 0.999 |
| count, sliced | 21.74 | 22.00 | 1.012 | 0.994 |
| agg, unsliced | 181.01 | 184.56 | 1.020 | 1.006 |
| agg, sliced | 183.92 | 181.66 | 0.988 | 0.990 |

Per-round ratios spread widely on this host: A2/A rounds run 0.80-1.19 on the count arm, 0.90-1.04 on plain.

**No host work (the criterion): no measurable change.** Plain is 0.8% (unsliced) and 1.5% (sliced) slower; the
same binary against itself is off by 1.5% and 0.6%, and its per-round ranges (0.95-1.02, 0.90-1.04) hold both. The
plain scan's code differs only in the window's end being computed from a variable chosen once per segment.

**Host work: 1-2% where the visitor's work is real, 5.6% on the cheapest visitor, unsliced.** The change adds 16
times the window exits: 150,000 ordinals make about 586 windows of 256 against 37 of 4,096, about 550 more exits per
scan. An exit returns from the window, tests the cursor and awaits the checkpoint (a null slice is ready at once; a
bounded one reads the TSC). At a generous 10 ns each that is about 5.5 us on a 1.34 ms scan, 0.4%. So the window
count does not explain count's unsliced 5.6%: the sliced arm of the same visitor pays the same exits plus a TSC read
each and shows 1.2%. The 5.6% is the code's placement or the host's noise, not work, and per the lead's ruling it is
not rerun. At aggregation cost (181 ns per match here, 0.56 us in Xapiand's mix) the change is within 2% either way.

## What it leaves open

- The turn bound in Xapiand (about 2.3 ms to about 0.14 ms per aggregation window) is derived from the per-match cost,
  not measured; Xapiand's x7-loop-stalls measures loop lag once its pin includes this commit.
- A scan where every ordinal matches and the host's work is slower than 0.56 us per match still holds a turn for
  256 times that work; the window counts ordinals, not time.
