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
