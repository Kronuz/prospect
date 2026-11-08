# Phrase speed: three more losers (explore/phrase-speed)

Branch `explore/phrase-speed` from prospect main `0744e51`. Question: is loss 4 of
`format13-loss-attribution.md` (phrases 1.10 to 1.21 times Tantivy) still there on main, and does one
change in the positions reader or `phrase_freq` close most of it. Answer: not confirmed (no Tantivy on
this host), and the three single changes tried win nothing. All come after `phrases-explore.md`'s
three losers and the landed E2/E3 (two-slot match, counts-only skip).

## Setup and limits

- Intel i9-9980HK, macOS 15.6, Release, one thread, `quiet strict phrase` holds. Interleaved rounds
  with an A/A, scored by `bench/quick/ab.py` (B/A of variant over main; above 1.0 is slower).
- The 1M Wikipedia index on this host is a pre-manifest-3 build that main refuses (`manifest: version 2`)
  and its corpus is gone, and there is no cargo, so **Tantivy was not re-run and the current ratio to
  Tantivy is unmeasured**. Used instead: a synthetic corpus (150,000 documents of 120 to 450 words,
  Zipf over 40,000 words with a 35% chance each word follows one of three successors of the previous,
  generator in `.scratch`), one merged segment in docid order (81 MB), and 300 phrases (two thirds
  bigrams, one third trigrams) drawn from words that occur adjacent. These are heavier than the gate's
  (about 5 ms per query), common-word dominated, with few hits.
- Profile of main on that index (`sample`, phrase `COUNT`, self): `positions_reader::read` 31%,
  `pair_match` 22%, `phrase_freq` 10%, `find` 7%, postings `advance` 6%, residency `verify` 5%,
  `bitpack::decode` and unpack 6%.
- Identity: `dump` of the 300 phrases, counted and `PROSPECT_DUMP_COUNT=0`, byte-identical to main
  for every variant (1,800 lines each).

## Experiments

| # | Hypothesis | Change (macro) | `TOP_10` | `TOP_1000` | `COUNT` | A/A (same order) |
|---|---|---|---|---|---|---|
| 1 | A block decode (128 values, exceptions loop) for 1 to 4 positions is waste | extract only the needed values from the bitpack layout (lane k%4, row k/4, exception by `memchr`), full decode on the block's second touch (`PHR_PARTIAL=4`) | 1.020 [1.016, 1.024] | 1.034 [1.030, 1.038] | 1.036 [1.032, 1.040] | 1.002, 0.999, 1.001 |
| 2 | `pair_match`'s linear scan of the longer list (22% self) | galloping then binary search (`PHR_GALLOP`) | 1.002 [1.000, 1.004] | 1.011 [1.008, 1.013] | 1.012 [1.009, 1.015] | 0.996, 1.002, 1.001 |
| 3 | Reading the second term's positions past the lead's last one is waste | bounded `read` that stops at the lead's last wanted position, not memoized (`PHR_BOUND`) | 1.015 [1.010, 1.021] | 1.017 [1.011, 1.024] | 0.997 [0.993, 1.005] | 0.999, 1.000, 1.002 |

All three are losers (slower or inside the noise; rounds spread 0.94 to 1.08). Experiment 1 is the
clearest: the SIMD block decode (about 30 to 50 cycles) is cheaper than the scalar extraction plus
the re-decode when a neighbour lands in the same block. Experiment 3 loses on top-k because matches
read the second term twice (the bounded read is not memoized).

## Reading

Together with `phrases-explore.md` (a skip table 1.2%, no verification 3%, prefetch worse) six
single changes to the read path have been tried and none moved the row more than the noise. The cost
is the sum of required steps per rejected candidate (wdf decode, position-index sum, header step,
block decode, copy, match), each a few percent. What remains is an algorithm that rejects before
reading, which needs data not in format 14 (pair postings, parked), or a per-candidate batch path that
reads many candidates' positions together. Hand to the performance engineer only if a Tantivy rerun on
the real corpus shows the gap above 1.1 on current main; that rerun is the first step.
