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
