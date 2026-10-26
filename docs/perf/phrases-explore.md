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

# Round 2 (`perf/phrases`): what reaches Tantivy

Branch `perf/phrases` from `explore/phrases` (0555d10). The explorer's `PHR_*` macros are removed
(its numbers above stand); the harness is `bench/quick/phrases-explore/{abx.sh,score.py}`
(`abx.sh OUT ROUNDS "CMDS" name=bin ...` interleaves every variant per round, rotating the order,
best of 3 per query; `score.py OUT A B` gives the geometric mean B/A with a bootstrap interval,
per-round ratios, sums and result mismatches). The Tantivy 0.26 index and `bench_query` are
rebuilt in `.scratch/tantivy` from `corpus-1m.json` (the first million lines of the gate corpus,
`strata-wt/no-stalls/.scratch/corpus-1m.json`); 658 MB, one segment, as before.

## 4. Baseline and where the time sits (before any change)

Baseline on main (d6f15d6), five rounds, `quiet strict`: `TOP_10` 1.369 [1.328, 1.409] and
`COUNT` 1.438 [1.407, 1.471] Tantivy's time (A/A 1.003 and 0.990).

Per-query shape (300 phrases, `COUNT`; candidates counted with a scratch counter): the ratio is
about 1.5 from 15 to 300 candidates per query and falls only on the 6,500-candidate sextile
(1.14). By sums of time the 6 sextiles are 1.39, 1.48, 1.46, 1.50, 1.35, 1.14. The gate metric is
a geometric mean over queries, so the short queries (median 50 candidates, 40 microseconds in
Tantivy) carry it, and the explorer's profile, which is weighted by time, is a profile of the
long tail. `sample` on the 60 shortest phrases (3,100 samples), top of stack by area:

| Area | Prospect, short queries | Tantivy, same queries |
|---|---|---|
| postings navigation (advance, block loads, doc-id unpack) | about 45% | about 54% |
| malloc, free, bzero | 16% | 10% |
| `phrase_scorer` body (`find`, `phrase_freq`, vector assigns) | 12% | 8% (`PhraseScorer::advance`) |
| positions (reader, decode, copy) | about 18% | about 25% (positions_with_offset, memmove, decompress) |
| term lookup (FST, run walk, `decode_term_info`) | 5% | 3% |
| residency `verify` | 4% | 0 |

Phase timers inside `count_snapshot` (scratch, third run of each query): of a short query,
building the scorer (term lookups 12%, constructor 11%) plus destroying it is 27 to 30%; the
count itself 70%. Per query the build makes 50 allocations (19.6 KB): `postings_iterator` is 4.4
KB, `positions_reader` 1.2 KB, `phrase_scorer` 2.6 KB, plus `term_info` copies (each holds a
`vector<impact>`; copied four to five times per term), the term table's deque and per-column
vectors, and the vectors of the constructor. `tail_impacts` for terms under 128 postings is not
the cost (skipping it entirely moved nothing).

Tantivy's side: its phrase scorer reads both position lists whole for every candidate
(`positions_with_offset` into two `Vec<u32>`) and runs a merge intersection; nothing rejects a
candidate before it reads positions, and `TOP_10` is a full scan (its `TOP_10` time equals its
`COUNT` time). Its position blocks sit behind a contiguous array of one-byte bit widths per term,
summed to skip. Lucene's `ExactPhraseMatcher` has no pair index or early reject either; it walks
positions lazily with `nextPosition`. Neither engine does what the explorer's idea 1 proposes, so
parity has to come from a cheaper constant, not a smarter test.

What 145 ns of Tantivy per candidate buys, by that profile: about half is doc-id navigation
(`BlockSegmentPostings::seek`, `load_block`, strictly-sorted bit unpack), a quarter positions
(two `positions_with_offset` calls, bit unpack, copy), the rest the merge and bookkeeping.

## Plan

Hypotheses in order of measured weight, one per experiment; each is dumped byte-identical over
the 962 queries (counted and uncounted, 100k merged and 1M reordered) and A/B-ed against main
with an A/A in the same rotation:

1. E1, build diet: recycle the large per-query objects through a per-thread free list and
   reserve the constructor's vectors (the build is 27 to 30% of a short query).
2. E2, two-term path: no `order_`/`lists_` assigns and no insertion sort for the 2-slot phrase
   (198 of 300 phrases), and no allocation in the tail position path.
3. E3, `TOP_10`: a per-candidate bound from the counts already decoded (the phrase frequency is
   at most the smaller count) skips the position reads of candidates that cannot compete.
4. E4, a pair/bigram posting prototype for the long candidate lists, with its disk cost; only if
   1 to 3 leave the long tail as the loss.
