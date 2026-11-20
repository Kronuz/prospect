# P9: a cost-based planner for conjunctions, per clause and per window

Track P item P9 (strata `docs/PLAN.md`): a cost-based planner choosing leapfrog, windows, probes
or bitmap AND per clause from measured unit costs, generalizing the conjunction model in
`prefer_windows`. Quest `track-p`, criterion A7; gap rows for A14.

## Why P9 next (job `261117-075402-484d70`, exclusive)

Prospect main `0aad777` (P2 bits 1+2 landed) against Lucene 10.4 and Tantivy 0.26 over `serve`,
1M reordered, every gate class, seven rounds rotated, best of three, run level prospect/engine
(below 1.0: prospect faster). Lucene is slower than prospect on every row; Tantivy is the fastest
baseline where prospect loses:

| Class (queries) | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---:|---:|---:|---:|---:|
| terms (200) | 0.945 | 0.822 | 0.896 | 0.688 | 0.912 |
| union 2 / 3 / > 3 | 0.45-0.85 | 0.46-0.76 | 0.45-0.71 | 0.44-0.78 | 0.37-0.81 |
| intersection 2 (198) | 0.984 | **1.080** | **1.145** | **1.175** | 0.973 |
| intersection 3 (83) | 0.895 | 1.011 | **1.078** | **1.132** | 0.935 |
| intersection > 3 (19) | 0.904 | 0.945 | 0.962 | 0.997 | 0.849 |
| negated 2 (4) | 0.976 | **1.183** | **1.155** | **1.637** | **1.147** |
| negated 3 (15) | 0.990 | **1.177** | **1.176** | **1.199** | **1.279** |
| intersection_union 2 / 3 / > 3 | 0.62-0.74 | 0.77-1.00 | 0.90-**1.175** | 0.76-0.94 | 0.45-0.56 |
| phrase 2 / 3 / > 3 | 1.00-1.13 | 1.16-1.23 | 1.18-1.28 | 1.22-1.25 | 1.19-1.22 |

P3's rows (single terms, unions; block-max pruning on long lists) all win against Tantivy, so P3 has
no losing row to close at 1M. P9's rows (intersections, negations) lose at the top-k commands by
1.08-1.18 and negations by up to 1.64 (four queries). Phrases lose too, but they are P8 and P5.

## Where the intersection loss is (in process, `bench`, main)

Geometric mean µs per query, seven runs: intersection 2 `COUNT` 79.5, `TOP_100` 117.5,
`TOP_100_COUNT` 132.8, `TOP_1000` 133.8; intersection 3 `COUNT` 157.4, `TOP_100` 220.5. On the
queries that lose most to Tantivy, prospect's pruned `TOP_100` costs more than its own exact
`COUNT` (`+robert +reed` 155 against 103 µs, `+color +combinations` 116 against 59, `+anderson
+hospital` 181 against 99, over `serve`): finding the matches is cheaper on the counting path
(window bits ANDed, or probes, by `prefer_windows`) than on the top-k path, which always leapfrogs
from the lead and scores the lead's document before the other terms agree. A profile of
intersection 2 `TOP_100_COUNT` puts `postings_iterator::advance` first (498 samples), then
`term_scorer::score` (214) and `conjunction_scorer::align` (158).

## Plan (written before the runs)

- **Experiment 1, windowed top-k (bit 1).** When `prefer_windows()` holds for a conjunction of
  terms, top-k finds each 4,096-ordinal window's matches as counting does (a fresh copy's
  `fill`: window bits ANDed, probes for few candidates), skips a window whose summed bounds cannot
  compete, and scores only the window's matches, the lead first, the rest only while they can
  compete. Otherwise today's leapfrog.
- **Experiment 2, match first (bit 2).** In today's leapfrog block loop, the other terms agree
  before the lead's score is computed, so a document that does not match costs no score.
- Prototype behind `PROSPECT_EXPERIMENT_P9` (0 is main), reader-side only: no format change.
- **Instrument.** `classes_ab.py bench` (in process): intersection 2, 3, > 3 and the guards
  (negated 3, terms, intersection_union 2), `TOP_10`, `TOP_100`, `TOP_1000`, `TOP_100_COUNT`,
  `COUNT`; screens under `quiet strict`, five rounds; the deciding A/B under `quiet exclusive`,
  seven rounds rotated, best of three, A/A from a copy of main's binary. The winner against Tantivy
  over `serve`.
- **Rule.** Per class and command, the run-level ratio B/A with its 95% interval. A row wins when
  its upper bound is under 1.0, its ratio under the A/A interval's lower bound and at most 0.97; it
  loses when its lower bound is over 1.0, its ratio over the A/A interval's upper bound and at least
  1.03. Keep a bit when an intersection row wins and no row loses. Results identical: `verify`
  clean over the 962 gate queries and the run's per-query results equal to main's.
- Budget: about five experiments; stop at the third loser in a row.

## Screens (in process, `quiet strict`, five rounds rotated, best of three; run level B/A)

Parity of each variant: `verify` 0 mismatches over the 962 queries, `dump` counted and uncounted
byte-identical to main's (5,772 lines).

| Class | Command | 1: windowed top-k | 2: match first |
|---|---|---|---|
| intersection 2 | `TOP_10` | 1.342 [1.310, 1.374] | 1.067 [1.021, 1.116] |
| intersection 2 | `TOP_100` | 1.212 [1.169, 1.257] | **0.947** [0.920, 0.975] |
| intersection 2 | `TOP_1000` | 1.172 [1.152, 1.192] | **0.927** [0.915, 0.939] |
| intersection 2 | `TOP_100_COUNT` / `COUNT` | 1.043 / 0.973 | 1.031 / 0.987 |
| intersection 3 | `TOP_10` | 1.240 [1.216, 1.263] | 1.106 [1.065, 1.149] |
| intersection 3 | `TOP_100` | 1.139 [1.114, 1.165] | **0.947** [0.917, 0.979] |
| intersection 3 | `TOP_1000` | 1.118 [1.107, 1.129] | **0.930** [0.911, 0.949] |
| intersection > 3 | `TOP_10` / `TOP_100` / `TOP_1000` | 1.20 / 1.14 / 1.13 | 1.035 / 0.939 / 0.928 |
| guards (negated 3, terms, intersection_union 2) | all | 0.98-1.02 | 0.98-1.04 (one noisy `COUNT` 1.18 [0.92, 1.53]) |

1. **Windowed top-k loses** (1.12-1.34 at every top-k row): finding matches from window bits is
   cheap, but scoring them needs the original iterators moved to each match anyway, and the window
   (4,096 ordinals) prunes far coarser than the lead's 128-posting blocks. Removed.
2. **Match first splits by k**: 0.93-0.95 at `TOP_100` and `TOP_1000`, 1.04-1.11 at `TOP_10`. With
   a high threshold (small k) the lead's score prunes most documents before the other terms move;
   with a low one it rarely prunes and only costs a score on every lead document that does not
   match. That is a per-block cost choice, which experiment 3 makes.
3. **Adaptive (bit 4)**: per document, the lead scores first only while its score can prune, that
   is while the threshold exceeds the other terms' block bounds by ALPHA of the lead's block bound
   (ALPHA 0 and 0.5); else match first.

Screen of bit 4 (same setup, a loaded host: unchanged rows such as terms `COUNT` spread 0.72-1.14):
ALPHA 0 intersection rows 0.91-1.01 at the top-k commands, ALPHA 0.5 0.86-1.05; `TOP_10` no longer
loses; `TOP_100_COUNT` and `COUNT` (counted paths, untouched) 0.95-1.08; parity clean for both. The
deciding A/B takes both ALPHAs with an A/A, under `quiet exclusive`.

Prototype: `docs/perf/p9-planner.patch` (`include/prospect/search.hh`, bits 1, 2 and 4, ALPHA by
`PROSPECT_P9_ALPHA`).

## Result: bit 4 against main (job `261117-092109-383ae4`, exclusive)

In process, seven rounds rotated, best of three; B is bit 4 at ALPHA 0, D at ALPHA 0.5, C a copy of
main's binary. The job exited 1 after its tables: its file-count check compared macOS `wc -l`'s
padded output with a number (840 files were there). The rounds took 10.4-11.1 s each, against
14.5 s for three sides in the loaded screen; the numbers stand.

| Class | Command | B/A (ALPHA 0) | D/A (ALPHA 0.5) | A/A |
|---|---|---|---|---|
| intersection 2 | `TOP_10` | 1.029 [0.925, 1.146] | 1.020 [0.968, 1.074] | 0.940 [0.853, 1.037] |
| intersection 2 | `TOP_100` | 0.933 [0.841, 1.035] | 0.883 [0.764, 1.020] | 0.957 [0.824, 1.112] |
| intersection 2 | `TOP_1000` | 1.029 [0.938, 1.129] | 0.945 [0.899, 0.995] | 0.966 [0.915, 1.020] |
| intersection 3 | `TOP_10` | 0.957 [0.925, 0.991] | 0.951 [0.862, 1.049] | 0.934 [0.852, 1.023] |
| intersection 3 | `TOP_100` | 0.950 [0.875, 1.032] | 0.971 [0.925, 1.019] | 0.977 [0.865, 1.104] |
| intersection 3 | `TOP_1000` | 0.905 [0.812, 1.008] | 0.897 [0.813, 0.990] | 0.953 [0.850, 1.068] |
| intersection > 3 | top-k | 0.94-0.97 | 0.93-0.98 | 0.93-1.05 |
| intersections | `TOP_100_COUNT`, `COUNT` (untouched paths) | 0.91-1.04 | 0.97-1.06 | 0.93-1.08 |
| intersection_union 2 | `TOP_1000` | 1.035 [0.950, 1.128] | 1.100 [1.020, 1.186] | 1.032 [0.974, 1.094] |
| guards | other rows | 0.98-1.12 | 0.93-1.14 | 0.91-1.09 |

Per-query best ratios on the intersection top-k rows: B 0.947-0.989, D 0.947-0.982, A/A
0.984-1.037. Over `serve` (job `261117-092115-ad33ee`, five rounds; it exited 1 on the same check
after complete tables, 375 files), bit 4 against main: 0.90-1.04 at ALPHA 0, 0.92-1.08 at ALPHA 0.5,
every interval across 1.0. Main against Tantivy in that run: intersection 2 `TOP_100` 1.041,
`TOP_1000` 1.080, `TOP_100_COUNT` 1.234.

By the rule no row wins: the best (D, intersection 2 `TOP_1000`, 0.945; intersection 3 `TOP_1000`,
0.897) sit inside the A/A's interval, whose spread on this host is ±5-15% per row. D loses one guard
row (intersection_union 2 `TOP_1000`, 1.100, above the A/A's 1.094), a path bit 4 does not touch.
The per-query bests point at 2-5% on intersection top-k, below what seven rounds resolve here.
Full `ctest` with the patch at bit 4: passed (job `261117-092124-2768e6`).

## Rerun at prospect:2ce63ac (jobs at the quest's head)

The same builds (main `0aad777`, SHA-1 `614e4d1e`; bit 4 at ALPHA 0, `383a79c0`, and at 0.5,
`df3c453b`) and scripts, under `quiet exclusive`. In process (job `261119-165438-2cf08a`, seven
rounds rotated, best of three, A/A from a copy of main's binary; 0 mismatches, 840 files):

| Class | Command | B/A (ALPHA 0) | D/A (ALPHA 0.5) | A/A |
|---|---|---|---|---|
| intersection 2 | `TOP_10` | 0.993 [0.972, 1.014] | 1.035 [0.986, 1.086] | 1.010 [0.975, 1.047] |
| intersection 2 | `TOP_100` | 0.994 [0.957, 1.033] | 1.002 [0.942, 1.066] | 1.054 [1.005, 1.105] |
| intersection 2 | `TOP_1000` | 0.931 [0.865, 1.002] | 0.945 [0.855, 1.045] | 0.990 [0.908, 1.081] |
| intersection 3 | `TOP_10` | 1.032 [0.967, 1.101] | 1.052 [0.983, 1.125] | 1.042 [0.959, 1.132] |
| intersection 3 | `TOP_100` | **0.940** [0.899, 0.983] | 1.008 [0.954, 1.066] | 0.996 [0.954, 1.039] |
| intersection 3 | `TOP_1000` | **0.962** [0.931, 0.993] | 0.978 [0.860, 1.113] | 1.044 [0.964, 1.131] |
| intersection > 3 | top-k | 0.93-0.98 | 0.94-0.97 | 0.97-0.99 |
| intersections | `TOP_100_COUNT`, `COUNT` (untouched paths) | 0.95-1.03 | 0.97-1.04 | 0.95-1.05 |
| guards | other rows | 0.96-1.12 | 0.94-1.07 | 0.95-1.07 |

Here bit 4 at ALPHA 0 passes the rule on two rows: intersection 3 `TOP_100` 0.940, under the A/A's
0.954, and `TOP_1000` 0.962, under the A/A's 0.964 by 0.002; no row loses. At ALPHA 0.5 no row
wins, and terms `TOP_1000` (1.044 [1.018, 1.070]) sits at the A/A's upper bound (1.044). Over
`serve` (job `261119-165439-7fcba3`, five rounds, no A/A) bit 4 at ALPHA 0 against main is 0.91-1.10
on the intersection rows, with intersection 3 `TOP_1000` 0.936 [0.896, 0.977] the one interval under
1.0; main against Tantivy: intersection 2 `TOP_100` 1.048, `TOP_1000` 1.177, `TOP_100_COUNT` 1.303.
Full `ctest` with the patch at bit 4 at prospect:2ce63ac: passed (job `261119-165432-b1efdc`).

Neither row won in the first deciding run (intersection 3 `TOP_100` 0.950 and `TOP_1000` 0.905, both
inside that A/A), and the first run's best rows (D, intersection 2 and 3 `TOP_1000`) do not clear
this one's. By the replication rule that removed P2's in-block selector (a gain counts only when the
same row clears the A/A in both deciding runs), no row replicated. Intersection 3 `TOP_1000` leans
below 1.0 in all three runs (0.905, 0.962, 0.936 over `serve`): a 4-6% effect on 83 queries that
seven rounds on this host resolve once in two.

## Four axes

| Axis | Before (main) | After (bit 4) | Source |
|---|---|---|---|
| Query speed, intersections | 1.0 | 0.88-1.03 run level, no row beyond the A/A; per-query best 0.95-0.99; rerun 0.93-1.03, two intersection 3 rows beyond the A/A, not replicated | jobs `261117-092109-383ae4`, `261119-165438-2cf08a` |
| Query speed, guards | 1.0 | 0.93-1.12, inside the A/A except one untouched row at ALPHA 0.5 | same job |
| Indexing speed | unchanged | unchanged | reader-only change |
| Memory | unchanged | unchanged | two locals per block loop; no allocation |
| Disk | unchanged | unchanged | no format change; `dump` byte-identical |

## Verdict

**Removed, all three experiments.** Windowed top-k loses at every top-k row (1.12-1.34). Match first
wins at `TOP_100` and `TOP_1000` and loses at `TOP_10`. The adaptive choice between them (bit 4)
fixes `TOP_10`, but its 2-5% on intersection top-k does not clear the A/A spread in the deciding
A/B or over `serve`; the rerun at prospect:2ce63ac clears it on two intersection 3 rows that the
first run did not, so no row replicates. Third non-winner in a row, so the item stops here. What would move the
intersection gap to Tantivy (1.04-1.23) is not the choice of evaluation order: the profile is
`postings_iterator::advance` and scoring every lead document. A P9 retry needs a quieter instrument
(more rounds, or instruction counts) before it can resolve a 3% effect.
Not measured: arm64, Linux, FreeBSD, the gate VM's 5M.
