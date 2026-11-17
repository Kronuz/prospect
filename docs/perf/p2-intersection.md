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

Profile (`sample`, main, intersection 2): `TOP_1000` spends 31% in the conjunction's block loop and
23% in `postings_iterator::advance`; `COUNT` 20% in `advance`. Inside `advance`, a target in the
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
