# P2: SIMD search and intersection of decoded blocks, galloping across blocks, bitset rank

Track P item P2 (strata `docs/PLAN.md`): SIMD intersection of decoded blocks, galloping across
blocks, and SIMD rank and select in bitset blocks. Quest `track-p`, criterion A1.

## Plan (written before the runs)

- **Classes.** The gate's intersections, split by `bench/quick/classes_ab.py split` from the 962
  gate queries: `intersection_2` (198), `intersection_3` (83), `intersection_gt3` (19), and the
  `intersection_union` classes (40) as a guard. Commands `TOP_10`, `TOP_100`, `TOP_1000`,
  `TOP_100_COUNT`, `COUNT`.
- **Baseline.** Main against Lucene 10.4 and Tantivy 0.26 (single indexing thread, one merged
  segment each) over each engine's `serve`, by `classes_ab.py serve`, five rounds rotated, under
  `quiet exclusive`. PISA is not installed on this host; its rows stay those of
  `format13-loss-attribution.md` (gate VM, 5M).
- **Experiments**, each behind `PROSPECT_EXPERIMENT_P2` bits in `reader.hh` (0 is main):
  1. Bit 1, search of a decoded block: every one of the block's 128 slots compared with the target,
     branch-free so the compiler vectorizes it (AVX2 at `-march=native`, NEON on arm64), in place of
     four linear probes and a branch-free binary search; a short block's slots past its end hold
     `no_more_docs`. Lucene 10.4's `findNextGEQ` is a vectorized linear search of the same kind.
  2. Bit 2, rank in bitset blocks: the set bits before each word, kept when the block is loaded (its
     popcounts are already taken there to check the count), so `advance_bits` ranks with one
     popcount instead of one per word before the target.
  3. Bit 4, galloping across blocks: `skip_to_block_containing` probes entries 1, 2, 4, ... past the
     next block inside its group before bisecting, instead of bisecting the group's 32 entries.
  4. If the profile still shows the leapfrog itself: a block-against-block intersection of two
     decoded blocks for the counted conjunction.
- **Instrument.** `classes_ab.py bench`: `prospect_engine bench` in process, sides rotated per round,
  best of three per query and round; 1M gate corpus, one reordered segment; A/A from a copy of
  main's binary in the same session. Screening under `quiet strict`; the deciding row under `quiet
  exclusive` as a campaign job.
- **Rule.** Per class and command, the run-level ratio B/A (each round's geometric mean over the
  class's queries, t interval over at least five rounds). A row wins when its upper bound is under
  1.0 and under the A/A band and its ratio is at most 0.97; it loses when its lower bound is over 1.0
  and over the band and its ratio is at least 1.03. Keep an experiment if a target row wins and no
  row loses; results must be identical: `dump` (counted and uncounted, both schemes, k 10, 100,
  1,000) byte for byte with main's, and `verify` clean, over the 962 gate queries.

## Setup

- Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, `-DCMAKE_BUILD_TYPE=Release -march=native`
  (the harness engine's flags); prospect main `eef601d`; variants built from it with
  `-DPROSPECT_EXPERIMENT_P2=BITS` and [`p2-intersection.patch`](p2-intersection.patch) applied.
- Index: the 1M gate corpus, `prospect_engine index` then `merge` (one reordered segment, two
  reorder threads); Lucene 10.4 and Tantivy 0.26 from the harness's single-thread engines, each
  force-merged to one segment by its own `BuildIndex`/`build_index`.
- Parity: `dump` (counted and uncounted) byte-identical to main's and `verify` 0 mismatches over the 962
  gate queries for bits 1+2 and for 1+2+8; every A/B row below has 0 count mismatches.

## Baseline: main against Lucene and Tantivy (job `261117-021317-afb4da`, exclusive)

Over `serve`, five rounds rotated, best of three, run level prospect/engine (below 1.0: prospect
faster). Lucene is slower on every intersection row; Tantivy is the fastest baseline at `TOP_100`,
`TOP_1000` and `TOP_100_COUNT`.

| Class | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---:|---:|---:|---:|---:|
| intersection 2 / Lucene | 0.690 | 0.782 | 0.805 | 0.954 | 0.754 |
| intersection 3 / Lucene | 0.760 | 0.911 | 0.757 | 0.895 | 0.733 |
| intersection > 3 / Lucene | 0.838 | 0.856 | 0.829 | 0.862 | 0.927 |
| intersection 2 / Tantivy | 0.940 | **1.148** | **1.103** | **1.302** | 1.018 |
| intersection 3 / Tantivy | 0.801 | **1.135** | 0.965 | **1.221** | 0.947 |
| intersection > 3 / Tantivy | 0.875 | 0.973 | **1.022** | 0.983 | 0.933 |

Profile (`sample`, 5 s, self time, intersection 2): main's `TOP_1000` spends 32% in the conjunction's
block loop and 26% in `postings_iterator::advance`, its `COUNT` 18.5% in `advance`; with bits 1+2,
27% and 21%, and 16%. Inside `advance`, a target in the
decoded block took four linear probes and a seven-step binary search; a target in a bitset block
took one popcount per word before it (up to 63).

## Experiments (screens under `quiet strict`, five rounds; per-query best against main)

| # | Change | Intersection classes | Verdict |
|---|---|---|---|
| 1 | Bit 1: all 128 slots compared, branch-free (51 AVX2 instructions, no branch) | 0.84-0.96 | **kept** |
| 2 | Bit 2: rank per bitset word, kept at load | 0.93-1.06 alone; 0.94-0.99 over bit 1 | **kept** (below) |
| 3 | Bit 4: galloping across a group's block entries | 0.95-1.11 (`TOP_1000` 1.06 run level) | removed, a loser |
| 4 | Bit 8: a window's lead postings filtered block against block (merge) in counted conjunctions | 0.95-1.06 over 1+2 | removed, neutral |

Galloping loses because the target's block is usually the next one (already checked before any
search) or far away, where the bisection's five probes of bit-packed entries are cheaper than a
gallop's extra ones. The block filter is neutral because counted conjunctions with dense terms take
the window AND (`prefer_windows`), and with a rare lead each candidate still lands in a different
block of the other term, which is the per-candidate `advance` again.

## Result: bits 1 and 2 against main (job `261117-021758-5aded6`, exclusive)

In process, seven rounds rotated, best of three; A/A is a copy of main's binary in the same session.
Run level B/A with its 95% interval (below 1.0: faster).

| Class | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---|---|---|---|---|
| intersection 2 | 0.874 [0.790, 0.966] | 0.833 [0.791, 0.876] | 0.859 [0.817, 0.902] | 0.857 [0.804, 0.914] | 0.922 [0.812, 1.046] |
| intersection 3 | 0.863 [0.801, 0.928] | 0.852 [0.806, 0.901] | 0.881 [0.830, 0.935] | 0.782 [0.674, 0.907] | 0.838 [0.767, 0.917] |
| intersection > 3 | 0.806 [0.738, 0.879] | 0.884 [0.820, 0.952] | 0.874 [0.805, 0.949] | 0.807 [0.714, 0.912] | 0.891 [0.828, 0.960] |
| A/A, intersection 2 | 0.982 | 0.997 | 0.970 | 0.999 | 1.036 |
| bit 2 over bit 1, intersection 2 | 0.952 [0.892, 1.016] | 0.938 [0.883, 0.997] | 0.994 | 0.996 | 0.995 |

Fourteen of fifteen intersection rows win by the rule (intersection 2 `COUNT`, 0.922, straddles 1.0);
none loses. Bit 2 alone wins one row over bit 1 (intersection 2 `TOP_100`) and leans below 1.0 on the
rest. The guard classes in the same job, run level B/A: unions 0.86-0.99, phrases 0.87-0.96 (their
postings intersection is the same `advance`), negations 0.82-0.94, single terms 0.96-1.03 (A/A
0.94-1.03); no row loses.

## After: main with bits 1+2 against Lucene and Tantivy (job `261117-075402-484d70`, exclusive)

Prospect main `0aad777` (bits 1+2 landed through quest `p2-intersection`), the same harness and
1M index, seven rounds rotated, best of three; a separate session from the baseline, so the
before/after deltas carry both runs' noise. Bold: Tantivy still faster.

| Class | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---:|---:|---:|---:|---:|
| intersection 2 / Lucene | 0.810 | 0.814 | 0.813 | 0.873 | 0.741 |
| intersection 3 / Lucene | 0.803 | 0.787 | 0.727 | 0.808 | 0.742 |
| intersection > 3 / Lucene | 0.866 | 0.805 | 0.757 | 0.806 | 0.790 |
| intersection 2 / Tantivy | 0.984 | **1.080** | **1.145** | **1.175** | 0.973 |
| intersection 3 / Tantivy | 0.895 | **1.011** | **1.078** | **1.132** | 0.935 |
| intersection > 3 / Tantivy | 0.904 | 0.945 | 0.962 | 0.997 | 0.849 |

Against Tantivy, `TOP_100` moved from 1.148 to 1.080 (intersection 2) and 1.135 to 1.011
(intersection 3), `TOP_100_COUNT` from 1.302 to 1.175 and 1.221 to 1.132; `TOP_1000` of
intersection 2 stays behind (1.103 then, 1.145 now, inside the two runs' spread). The remaining
gap is the conjunction's top-k path, taken up by P9 (`p9-planner.md`).

## Four axes

| Axis | Before (main) | After (bits 1+2) | Source |
|---|---|---|---|
| Query speed, intersections | 1.0 | 0.78-0.92 (1.09-1.28x faster) | job `261117-021758-5aded6` |
| Query speed, other classes | 1.0 | 0.82-1.0, no row lost | same job |
| Indexing speed | unchanged | unchanged | reader-only change; the writer is untouched |
| Memory | 27.1 MB peak RSS (`bench`, intersection 2) | 27.0-27.4 MB, same within 0.5%; +64 B per open postings iterator (the rank bytes) | `/usr/bin/time -l`, best of three |
| Disk | unchanged | unchanged | no format change; `dump` byte-identical |

At 1.09-1.28x faster on intersections, a conjunction-heavy workload spends about 8-22% less CPU on
those queries; for the gate's mix of classes the saving is smaller (single terms do not move).

## Verdict

**Keep bits 1 and 2** (an SIMD in-block search and a bitset rank directory); remove bits 4 and 8.
Landing needs `include/prospect/reader.hh` without the experiment switch (one mechanism, no
`#if`), its tests, and the other platforms once on that commit: arm64 (NEON compares, 4 lanes),
Linux and FreeBSD through CI. Not measured here: arm64 and the gate VM's 5M.

## Follow-up: choosing the in-block search by expected skip distance

Bit 1's 128-slot compare landed as four probes, a bisection of a remainder past 32 slots, and
otherwise a compare from the target's 16-slot row (prospect:387a47f): short lists stayed neutral,
but the 1M intersection rows kept only part of the full compare's gain. Three selectors, in
[`p2-find-in-block.patch`](p2-find-in-block.patch) (`PROSPECT_EXPERIMENT_FIB`):

1. FIB 1: the probes, then the remainder compared from its row (no bisection).
2. FIB 2: the probes only when the target is expected within four slots at the remainder's
   mean gap, `(target - doc) * slots < 4 * span` (division-free); otherwise straight to the
   compare from the row.
3. FIB 3: FIB 2, with the compare in 32-slot windows stopping at the first that reaches target.

FIB 9 (the 128-slot compare) is the reference. Same rule as above (a win's ratio is also under the A/A's lower bound); a variant is kept only if
every case of the reviewer's short-list probe stays within its A/A as well.

**1M A/B** (job `261117-113100-ed8f75`, exclusive, seven rounds rotated, best of three; main
0aad777 and its copy as A/A). Run level B/A [95%]; bold wins by the rule.

| Row | A/A | FIB 1 | FIB 2 | FIB 3 | full 128 |
|---|---|---|---|---|---|
| intersection 2 `TOP_100` | 0.966 [0.916, 1.020] | 0.934 [0.853, 1.022] | 0.960 [0.869, 1.060] | 0.964 | 0.942 [0.899, 0.987] |
| intersection 2 `TOP_1000` | 1.017 [0.994, 1.040] | **0.947** [0.917, 0.978] | **0.966** [0.943, 0.989] | 0.970 | 0.965 [0.904, 1.029] |
| intersection 2 `TOP_100_COUNT` | 1.003 [0.961, 1.047] | **0.917** [0.885, 0.951] | **0.943** [0.906, 0.981] | 0.979 | **0.881** [0.839, 0.926] |
| intersection 3 `TOP_100_COUNT` | 0.990 [0.949, 1.032] | **0.888** [0.829, 0.952] | **0.920** [0.848, 0.999] | 0.974 | **0.895** [0.817, 0.982] |
| intersection classes, range | 0.963-1.115 | 0.888-1.049 | 0.894-0.985 | 0.913-1.013 | 0.865-0.965 |
| guards, range (unions, negated 3, phrase 2, terms) | 0.915-1.044 | 0.844-1.045 | 0.910-1.045 | 0.909-1.028 | 0.840-1.064 |

No row of any variant loses. Parity (job `261117-113110-0f8110`): `dump` counted and uncounted
byte-identical to main and `verify` 0 mismatches over the 962 gate queries, all four variants.

**Short-list probe** (job `261117-113107-dc09de`, exclusive, reviewer's `tiny_bench`, seven
rounds rotated; median of per-round ratios to main, [min-max]). The worst case of each:

| Build | A/A band | FIB 1 | FIB 2 | FIB 3 | full 128 |
|---|---|---|---|---|---|
| default flags (SSE2) | 0.98-1.06 | sparse50&blocks2000 **1.148** [1.02-1.22], A/A 1.040 [0.89-1.13] | sparse50&blocks2000 1.101 [0.97-1.23] | tail100&blocks2000 1.047 [1.01-1.17] | 1.20-1.77 on four cases |
| `-march=native` (AVX2) | 0.93-1.01 | 1.028 | one&tail40&tail100 1.029 | tail40&tail100 1.117 [1.04-1.39] | 1.27-1.64 on three cases |

FIB 1 loses sparse targets in a long list on the portable build: every round is over 1.0 and the
median is past the A/A's range (four predictable missed probes, then a compare of the remainder at
four lanes, against the bisection's six steps). FIB 2 skips those probes; its worst case (1.101)
stays inside that case's A/A range (0.89-1.13). FIB 3's windows add a branch per window, and the
1M gains go away.

**Verdict: FIB 2 kept**, landed in `reader.hh` without the switch. Against main it wins three
intersection rows, among them two where Tantivy leads (intersection 2 `TOP_1000` 0.966 and
`TOP_100_COUNT` 0.943), keeps every short-list case inside its A/A, and removes the bisection.
It recovers about half of the full compare's 1M gain (`TOP_100_COUNT` 0.943 against 0.881); the
rest is what short lists pay for comparing every slot. FIB 1 and 3 removed.

| Axis | Before (main 0aad777) | After (FIB 2) | Source |
|---|---|---|---|
| Query speed, intersections | 1.0 | 0.894-0.985 (three rows win, none lose) | `261117-113100-ed8f75` |
| Query speed, short lists | 1.0 | inside the A/A on every case | `261117-113107-dc09de` |
| Indexing speed | unchanged | unchanged | reader-only change |
| Memory | unchanged | unchanged | no new state; one multiply per in-block search |
| Disk | unchanged | unchanged | no format change; `dump` byte-identical |
