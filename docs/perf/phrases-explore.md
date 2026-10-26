# Phrases against Tantivy: the loss on main, where it sits, three losers

Branch `explore/phrases` from main `d6f15d6` (format 14). Question: is loss 4 of
`format13-loss-attribution.md` (phrases 1.10 to 1.21 times Tantivy) still there, and what single
change fixes most of it. Answer: the loss is there and larger, it is diffuse, and the three
single changes tried (two that the profile pointed at, one that the ablation did) win nothing.

## Setup

Intel i9-9980HK, macOS 15.6, Release, one thread, `quiet strict` holds. Index: the 1M reordered
index in `~/.scratch/prospect-idx/1000000-reordered`. Tantivy 0.26 (`bench/engines/tantivy-0.26-st`
build_index, one merged segment, 658 MB) on the same 1M corpus; `bench_query.rs` here times it
in process like `prospect_engine bench` (parse included, best of 3 per query). Queries: the 300
phrases of the 962 gate queries. Rounds interleave Tantivy and prospect, order alternating;
the A/A runs prospect against itself in the same rotation. Scored by `bench/quick/ab.py`
(B/A = prospect over Tantivy). Counts agree on every phrase (0 mismatches).

## 1. The loss on main

| Command | B/A [95%], 8 rounds | second run, 10 rounds | A/A |
|---|---|---|---|
| `TOP_10` | 1.335 [1.297, 1.373] | 1.361 [1.324, 1.399] | 1.000, 0.999 |
| `TOP_100` | 1.376 [1.349, 1.404] | | 1.003 |
| `TOP_1000` | 1.363 [1.338, 1.392] | 1.402 [1.375, 1.430] | 1.007, 0.998 |
| `TOP_100_COUNT` | 1.352 [1.325, 1.383] | | 0.996 |
| `COUNT` | 1.390 [1.358, 1.420] | 1.427 [1.399, 1.457] | 0.996, 1.001 |

Prospect is slower, 1.34 to 1.43 times Tantivy's time as a geometric mean (the doc's 1.10 to 1.21
was the harness's, whose per-query floor differs). By sums of time `COUNT` is 1.23 and `TOP_10`
1.08: the loss is on the short queries (5 to 60 microseconds, 1.5 times) and falls to 1.1 to 1.2
on the long ones, and is proportional to the work, not a fixed cost (the extra time over Tantivy
grows from 7 to 110 microseconds across the quintiles).

The same 300 term pairs as conjunctions (`+a +b`, `COUNT`) are at parity: 1.039 [0.989, 1.091]
(prospect 25 ms, Tantivy 37 ms by sums). So the postings intersection is not the loss; the
phrase check on top of it is: 89.5 ms against Tantivy's 56.5 ms, 1.58 times.

## 2. Where the time goes

`sample` profile (phrase `COUNT`, 8 s), inclusive: `phrase_freq` 59% (`positions_reader::read`
37%, of which `decode_block` 16%; `count()` 6%; its own body 13%), postings `advance` 30%.
Sampling over-attributes: it put 6% on residency verification, which removing did nothing (A below).

Ablation (stage build, `COUNT`, sum over the 300 phrases): conjunction plus `count()` per term
50 ms (the conjunction alone 25 ms; the other 25 ms is decoding the wdf blocks, which the position
reads need anyway); full phrase check 116 ms; reading every term's positions without comparing
129 ms. Counters on the same run: 388,397 candidates (`phrase_freq` calls), 780,542 position
reads (two per candidate), 166,948 block decodes, 425,504 block-header steps. Cycles (rdtsc,
2.4 GHz): `read` 173 M of about 281 M total, `decode_block` 102 M, of which stepping over
headers 53 M and verifying plus decoding 43 M. Nearly every candidate is a reject (the
document has both terms, not the phrase), at about 230 ns each, against Tantivy's about 145 ns.

## 3. Experiments (all identical on `dump` before timing)

`dump` of the 962 queries, counted and `PROSPECT_DUMP_COUNT=0`, 100k merged and 1M reordered,
5,772 lines each: byte-identical to main for every variant below.

| # | Hypothesis | Change (macro) | B/A over main [95%] | A/A | Verdict |
|---|---|---|---|---|---|
| A | Position reads pay for per-step verification and a checked summation | window cache of the last verified range in `positions_reader::need`; the wdf prefix sum accumulated unchecked (`PHR_T1`, `PHR_T2`) | `COUNT` 0.968 [0.959, 0.977], `TOP_1000` 0.989, `TOP_10` 1.026 | 0.978, 0.979, 1.023 | loser (inside A/A) |
| B | Stepping over block headers is the cost (53 M of 281 M cycles); a skip table for the position blocks removes it | table of block offsets (every block, or every 8th), built on first use and kept, which models a stored table (`PHR_TABLE=1`, `=8`) | K=1: `TOP_10` 0.987 [0.981, 0.992], `TOP_1000` 0.988, `COUNT` 0.988; K=8: 1.014, 1.009, 1.009 | 0.999, 0.998, 1.001 | loser: about 1%; the miss moves to the decode |
| C | The misses of the two terms' blocks serialize; prefetch both before reading | table K=1 plus a prefetch of each term's target block before the reads (`PHR_PREFETCH`) | 1.070, 1.064, 1.057 (slower) | 0.998, 1.003, 1.002 | loser |

Rounds are noisy (single-round ratios of 0.9 to 1.13 in the A/A); the table gives 1.2% at best.

## What is left

No one change: the 23 to 43% gap is spread over about 230 ns of work per rejected candidate
(`count()` and wdf decode, two position reads at about 220 cycles each, the `phrase_freq`
bookkeeping, the leapfrog), none of which a trim moved. Not tried, in the order I would try them:

1. Fewer rejects: a cheap necessary test before any position read (388k candidates give a few
   thousand phrase hits per run), for example per-block bounds on the position count of a document
   against the lead's, or storing for adjacent term pairs of common phrases a pair posting
   (a format change, Lucene's and Tantivy's "shingle"/pair-index idea).
2. A specialised two-term path (198 of the 300 phrases) with no `order_`/`lists_` vectors,
   reading the second term's positions only up to the lead's last position.
3. A SIMD prefix sum for the delta copy loop and PFOR without exceptions on position blocks
   (writer policy; trades size for decode, the exception patch loop is about 4% of samples).

Hand to the performance engineer if item 1 or 2 is wanted: the stage build and counters are in
`include/prospect/{reader,search}.hh` behind `PHR_*` macros, scripts in
`bench/quick/phrases-explore/` (`abt.sh` Tantivy against prospect, `abn.sh` and `abm.sh` base
against variants with the A/A, `stg.sh` the ablation, `bench_query.rs` the Tantivy timer).
