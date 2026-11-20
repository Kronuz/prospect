# P7: negations, the term's bulk top-k with excluded clauses probed

Track P item P7 (strata `docs/PLAN.md`): membership probes on dense terms. Its counting-window half
is [`p7-membership-probes.md`](p7-membership-probes.md), which left exclusions "not applied". This
entry is the exclusion half. Quest `track-p`, criterion A5; gap rows for A14.

## Why P7 next (job `261117-075402-484d70`, exclusive)

Prospect main `0aad777` against Tantivy 0.26 over `serve`, 1M reordered, seven rounds rotated
(the table in [`p9-planner.md`](p9-planner.md)). The rows where prospect is still slower, as
prospect/Tantivy:

| Class (queries) | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---:|---:|---:|---:|---:|
| negated 2 (4) | 0.976 | **1.183** | **1.155** | **1.637** | **1.147** |
| negated 3 (15) | 0.990 | **1.177** | **1.176** | **1.199** | **1.279** |
| phrase 2 / 3 / > 3 (300) | 1.00-1.13 | **1.16-1.23** | **1.18-1.28** | **1.22-1.25** | **1.19-1.22** |
| intersection 2 (198) | 0.984 | **1.080** | **1.145** | **1.175** | 0.973 |
| intersection 3 (83) | 0.895 | 1.011 | **1.078** | **1.132** | 0.935 |

The negations have the largest ratios, and they point at P7. Phrases point at P8 and P5: six
read-path trims have already lost there ([`phrases-explore.md`](phrases-explore.md),
[`phrase-speed.md`](phrase-speed.md)), and what remains is a format change (pair postings or P8's
prefilter section). P3, P11 and P21 have no losing row at 1M, because terms and unions win.

## Where the negation loss is (in process, `bench`, main)

A negation's top-k had no bulk path. The generic loop walked every posting of the included term,
stepped each excluded clause to it, and scored it. `+jordan -country` `TOP_100` took 422 µs, against
83 µs for `jordan` alone, which skips blocks and groups by bound. In a `sample` of three negations at
`TOP_100`, the top functions were `term_scorer::score` (193 samples), `postings_iterator::advance`
(166), `exclusion_scorer::next` (117) and `skip_blocks` (85).

## Plan (written before the runs)

- **Bit 1, bulk top-k.** When the included side of a negation (nested or not) is a term, top-k runs
  that term's bulk path (blocks, or groups by descending bound in a reordered segment). A hit at or
  below the threshold is dropped unprobed; any other hit is probed against each excluded clause.
  When the groups step back in ordinals, the clauses restart from fresh copies.
- **Bit 2.** Nested exclusions are counted apart, by windows (the included term's bits AND NOT the
  excluded ones'), so a counted top-k also takes the bulk path.
- **Bit 4.** Every exclusion is counted by windows, including term minus term.
- The prototype is behind `PROSPECT_EXPERIMENT_P7` in [`p7-exclusion.patch`](p7-exclusion.patch).
  It is reader-side only, with no format change.
- **Instrument.** `classes_ab.py bench`, in process, seven rounds rotated, best of three, under
  `quiet exclusive`, with an A/A from a copy of main's binary. Classes are negated 2 and 3
  (targets), plus terms, intersection 2, phrase 2 and union 2 (guards).
- **Short-list probe.** Twelve negations whose included term is rare (df 7-511), against dense
  (`the`, `city`), moderate and rare excluded terms, judged per case. The reviewer's `tiny_bench`
  times `reader.hh` alone, which this change does not touch.
- **Rule.** Each row is a run-level B/A with its 95% interval. A row wins when its upper bound is
  under 1.0, its ratio is under the A/A's lower bound, and the ratio is at most 0.97. It loses when
  its lower bound is over 1.0, its ratio is over the A/A's upper bound, and the ratio is at least
  1.03. A short-list case loses when its median falls outside its A/A's per-round range.
- **Parity.** `verify` reports 0 mismatches, and `dump` (counted and uncounted, both schemes, k of
  10, 100 and 1,000) is byte-identical to main over the 962 gate queries and the 12 short
  negations.
- **Budget.** About five variants, stopping at the target or at the third loser in a row.

## Result: bits 1, 1+2 and 1+4 against main (job `261117-140026-dba8cc`, exclusive)

Parity is clean for all three variants. Run level B/A [95%]; bold rows win by the rule.

| Row | A/A | bit 1 | bits 1+2 | bits 1+4 |
|---|---|---|---|---|
| negated 2 `TOP_10` | 1.042 [0.941, 1.155] | **0.492** [0.468, 0.518] | **0.514** | **0.546** |
| negated 2 `TOP_100` | 0.986 [0.755, 1.287] | 0.664 [0.509, 0.866] | 0.714 | 0.723 |
| negated 2 `TOP_1000` | 0.897 [0.763, 1.055] | 0.841 [0.701, 1.009] | 0.873 | 0.878 |
| negated 2 `TOP_100_COUNT` | 1.006 [0.917, 1.105] | **0.765** [0.694, 0.843] | **0.757** | **0.797** |
| negated 2 `COUNT` | 1.013 [0.960, 1.070] | 0.991 | 1.067 | 1.118 |
| negated 3 `TOP_10` | 0.920 [0.843, 1.004] | **0.424** [0.378, 0.476] | **0.402** | **0.388** |
| negated 3 `TOP_100` | 0.973 [0.908, 1.042] | **0.619** [0.575, 0.666] | **0.585** | **0.625** |
| negated 3 `TOP_1000` | 1.069 [0.988, 1.157] | 0.953 [0.887, 1.024] | 0.917 | 0.984 |
| negated 3 `TOP_100_COUNT` | 0.996 [0.895, 1.109] | 0.938 [0.822, 1.071] | **1.153** [1.031, 1.291] (lost) | **1.234** [1.141, 1.334] (lost) |
| negated 3 `COUNT` | 0.910 [0.764, 1.083] | 0.832 [0.713, 0.970] | 0.957 | 0.877 |
| guards (terms, intersection 2, phrase 2, union 2) | 0.92-1.06 | 0.93-1.10, one lean (below) | 0.94-1.12 | 0.92-1.06 |
| short negations `TOP_10` | 0.918 [0.841, 1.002] | **0.782** [0.678, 0.902] | 0.762 | 0.823 |
| short negations `TOP_100_COUNT` / `COUNT` | 0.980 / 0.982 | 1.017 / 1.033 | **1.289 / 1.150** (lost) | **1.203 / 1.169** (lost) |

Bits 2 and 4 lose. Counting apart costs a second pass (a fresh copy filled per window), and it
beats the counted generic loop only where the windows are full. Short included lists pay
1.15-1.29, and `+zeppelin -kumquat` `COUNT` pays 2.1x. Both bits are removed.

Bit 1 wins seven negated rows (0.42-0.77) and short `TOP_10` (0.78), and loses no class row. Two
readings need care:

1. **Intersection 2 `TOP_100`** is 1.053 [1.018, 1.089] against an A/A of 0.997 [0.964, 1.031]. By
   the rule that is a loss, but bit 1 does not touch the conjunction's code: its per-query best is
   1.020, and the A/A rows of this session span 0.92-1.07. It is checked again at the landing
   commit.
2. **Short-list cases** (per case, median of per-round ratios). Two `TOP_1000` cases fall outside
   their A/A's range: `+xylophone -music -film` at 1.183 [1.08-1.71] (A/A 0.992 [0.91-1.10]) and
   `+quokka -the` at 1.167 [1.04-1.60] (A/A max 1.14). With k above the matches every hit is
   probed, as in main, but the prototype also built a fresh copy of each excluded clause per
   query. That is an allocation plus a term-info copy, which a 7-posting term cannot amortize.

## The landed form (prospect:bd60f39)

Bit 1 alone, without the switch. The excluded clauses are probed in place, stepping forward from
where they stand. Fresh copies are made only where a reordered segment's groups can step back
(`term_scorer::steps_back`), which removes the per-query copies that the short cases paid for.
Tests: `search_test` `negations()` ranks a term minus one or two clauses (dense, sparse, nested,
nested over a denser clause, everything excluded), counted and uncounted, for k from 1 to every
match. It compares against the generic scan's matches, ranked by score and then docid, in a
docid-ordered segment and in a permuted one whose term spans several block groups.
`test/negation_mutants.py` reverts the probe, the restart on a step back, the nested clauses, and
the match test.

**At the landing commit.** Suite `261117-155032-ecb275` passed. Parity, from an engine built
from the commit, `261117-155033-63b78b`: clean. Mutants 0-2 are caught (`261117-155040-04a4e1`,
`261117-155044-c780b6`, `261117-155047-253843`). The prototype's mutants 0-2 were caught at
prospect:080e0a7 (`261117-141718-c0035a`, `261117-141721-81b492`, `261117-141725-aa9027`).

A/B `261117-155036-9088e8` (exclusive; a main, c its A/A copy, b the landing commit), run level
[95%]; bold rows win by the rule:

| Row | A/A | landing |
|---|---|---|
| negated 2 `TOP_10` | 1.009 [0.947, 1.075] | **0.508** [0.413, 0.624] |
| negated 2 `TOP_100` | 1.022 [0.925, 1.130] | **0.676** [0.668, 0.685] |
| negated 2 `TOP_1000` | 1.082 [0.988, 1.186] | 1.031 [0.932, 1.141] |
| negated 2 `TOP_100_COUNT` | 0.963 [0.889, 1.043] | **0.794** [0.708, 0.891] |
| negated 2 `COUNT` | 1.026 [0.930, 1.131] | 1.019 [0.958, 1.085] |
| negated 3 `TOP_10` | 0.997 [0.925, 1.075] | **0.385** [0.353, 0.420] |
| negated 3 `TOP_100` | 0.937 [0.882, 0.995] | **0.559** [0.515, 0.607] |
| negated 3 `TOP_1000` | 0.994 [0.933, 1.059] | 0.936 [0.905, 0.969] |
| negated 3 `TOP_100_COUNT` | 0.975 [0.937, 1.013] | 0.953 [0.862, 1.053] |
| negated 3 `COUNT` | 1.018 [0.694, 1.493] | 0.933 [0.640, 1.361] |
| intersection 2 `TOP_100` | 0.995 [0.915, 1.082] | 0.989 [0.915, 1.069] |
| guards (terms, intersection 2, phrase 2, union 2) | 0.93-1.05 | 0.91-1.05, none beyond the A/A |
| short negations `TOP_10` | 0.946 [0.851, 1.050] | **0.799** [0.685, 0.933] |
| short negations `TOP_100` / `TOP_1000` | 0.953 / 0.972 | 0.931 / 1.041 [0.937, 1.157] |
| short negations `TOP_100_COUNT` / `COUNT` | 1.003 / 0.951 | 1.002 / 1.001 |

Intersection 2 `TOP_100`, the prototype's one lean row, is 0.989 at landing: the conjunction's
code is unchanged, and the prototype's 1.053 was noise. Of the two short cases the prototype lost,
`+quokka -the` `TOP_1000` is back inside its A/A (1.160 [0.97-1.22], A/A max 1.30).
`+xylophone -music -film` `TOP_1000` still loses: 1.310 [1.01-1.41], against an A/A of 0.958
[0.80-1.24], about 34.7 µs against main's 28.2 µs (means of seven rounds). It has 28 matches and
k = 1,000, so the threshold never rises, nothing is pruned, and every hit is probed as in main;
the bulk path adds its own overhead with nothing to skip. Its class row is neutral, and its
`TOP_10` is 1.007. A selector that keeps the generic loop when the hits collected plus the term's
df cannot reach k would remove this case; it is not measured.

## Four axes

| Axis | Before (main 0aad777) | After (bit 1) | Source |
|---|---|---|---|
| Query speed, negations | 1.0 | `TOP_10` 0.42-0.49, `TOP_100` 0.62-0.66, `TOP_100_COUNT` 0.77-0.94, `TOP_1000` 0.84-0.95, `COUNT` unchanged (2.0-2.4x faster at `TOP_10`) | job `261117-140026-dba8cc` |
| Query speed, other classes | 1.0 | 0.93-1.10, no class row beyond the A/A except intersection 2 `TOP_100` (1.053, untouched code; 0.989 at landing, job `261117-155036-9088e8`) | same job |
| Indexing speed | unchanged | unchanged | reader-only change |
| Memory | unchanged | one probe per negation's top-k: three small vectors per excluded clause, plus a fresh clause copy (about 4.4 KB) per restart in a reordered segment's group walk | code |
| Disk | unchanged | unchanged | no format change; `dump` byte-identical |

## Gap rows at the quest's head (A14, job `261119-165524-86e55d`, exclusive)

Main `0aad777` and the head `2ce63ac` (P7 bit 1 over main) against Lucene 10.4 and Tantivy 0.26
over `serve`, 1M reordered, every gate class, seven rounds rotated, best of three; run level
prospect/engine with its 95% interval, below 1.0 prospect faster; 0 mismatches on every row. A row
"loses" when its interval lies above 1.0.

| Class (queries) | Track P item | Head/Tantivy | Losing rows (head/Tantivy) | Head/main |
|---|---|---|---|---|
| negated 3 (15) | P7 | 0.47-1.15 | `TOP_1000` 1.099, `TOP_100_COUNT` 1.153, `COUNT` 1.151 | `TOP_10` 0.481, `TOP_100` 0.646, `TOP_1000` 0.951; counted 1.01-1.06 |
| negated 2 (4) | P7 | 0.72-1.15 | `TOP_1000` 1.154 | `TOP_10` 0.687, `TOP_100` 0.716, `TOP_100_COUNT` 0.789; others 0.93-0.96 |
| intersection 2 (198) | P2, P9 | 0.89-1.26 | `TOP_1000` 1.145, `TOP_100_COUNT` 1.258 | 0.97-0.99 |
| intersection 3 (83) | P2, P9 | 0.88-1.07 | none | 0.97-1.05 |
| intersection > 3 (19) | P2, P9 | 0.85-1.05 | none | 0.95-1.01 |
| intersection_union 2 (10) | C7 | 0.46-1.19 | `TOP_1000` 1.188 | 0.93-1.06 |
| intersection_union 3, > 3 (20, 10) | C7 | 0.47-0.96 | none | 0.97-1.04 |
| terms (200), frequent terms | C15, P3 | 0.74-1.00 | none | 0.99-1.05 |
| unions 2, 3, > 3 | P3 | 0.37-0.85 | none | 0.92-1.09 |
| phrase 2 (198) | P8, P5 | 1.03-1.28 | `TOP_100` 1.172, `TOP_1000` 1.148, `TOP_100_COUNT` 1.278, `COUNT` 1.238 | 0.96-1.04 |
| phrase 3 (83) | P8, P5 | 1.06-1.22 | `TOP_100` 1.202, `TOP_1000` 1.194, `TOP_100_COUNT` 1.224, `COUNT` 1.216 | 1.00-1.04 |
| phrase > 3 (19) | P8, P5 | 1.03-1.28 | `TOP_1000` 1.244, `TOP_100_COUNT` 1.186, `COUNT` 1.276 | 0.97-1.04 |

P7 closes the negated top-k gap it targets: against Tantivy, negated 3 `TOP_10` goes from 0.969 to
0.466 and `TOP_100` from 1.157 to 0.747, negated 2 `TOP_100` from 1.198 to 0.858 and
`TOP_100_COUNT` from 1.398 to 1.103 [0.946, 1.285] (main's rows from the same run). The estimate
above (`TOP_100` about 0.73-0.78) holds for negated 3. Negated `TOP_1000` (1.10-1.15) still loses
and the counted negations (`TOP_100_COUNT`, `COUNT`, 1.07-1.15) still trail, negated 3's beyond
1.0: at k = 1,000 the threshold prunes little, and the count path is unchanged. Head against
Lucene: 0.20-0.99 on every row but
intersection_union 2 `TOP_100` (1.059 [0.954, 1.175]). This run has no A/A, so head/main rows off
P7's path (union 2 `TOP_100` 1.085 [1.013, 1.163]) are the instrument's spread, not a change: the
head differs from main only in `exclusion_scorer`.

The gaps left at the quest's head are the phrase rows (1.15-1.28, P8 and P5; P13's codecs widen
them), intersection 2 `TOP_1000` and `TOP_100_COUNT` (1.15-1.26; P9's evaluation order does not
close them, `docs/perf/p9-planner.md`), intersection_union 2 `TOP_1000` (1.19) and the negations'
`TOP_1000` and counted rows.

## Verdict

**Kept: bit 1** (a negation's top-k through its term's bulk path, the excluded clauses probed
only for hits that could enter): at the landing commit it wins five negated rows (0.39-0.79) and
short `TOP_10` (0.80), loses no class row, and loses one short case of twelve at `TOP_1000`
(`+xylophone -music -film`, about 6 µs). **Removed: bits 2 and 4**
(counting negations apart by windows), which lose counted rows and short lists. Not measured:
arm64, Linux, FreeBSD, and the gate VM's 5M.
