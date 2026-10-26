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

Branch `perf/phrases` from `explore/phrases` (0555d10). The explorer's `PHR_*` macros and the scripts that
needed them (`abm.sh`, `stg.sh`) are removed (its numbers above stand); the harness is `bench/quick/phrases-explore/{abx.sh,score.py}`
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

## 5. Experiments (round 2)

Every row: `dump` of the 962 queries (counted and uncounted, both schemes, k of 10, 100, 1000) byte
identical to main on 100k flushed, 100k merged, 1M merged and 1M reordered (5,772 lines each),
`verify` 962 queries 0 mismatches on 100k merged and 1M reordered, all 39 ctest targets pass.
Timing: `quiet strict`, `bench/quick/phrases-explore/abx.sh`, 8 interleaved rounds (the order
rotated each round), best of 3 per query, the 300 gate phrases on the 1M reordered index; B/A is the
geometric mean over queries of B's best over A's with a bootstrap interval over queries; the A/A
is main against itself in the same rotation. Each row's A is the state before it.

| # | Hypothesis | Change | B/A `COUNT` [95%] | B/A `TOP_10` [95%] | A/A (`COUNT`, `TOP_10`) | Verdict |
|---|---|---|---|---|---|---|
| E1 | The build and teardown of a short query (27 to 30% of it, 50 allocations of 19.6 KB) is dominated by the large per-query objects | `postings_iterator`, `positions_reader`, `phrase_scorer` recycled through a per-thread stack of 8 blocks; constructor vectors reserved | 0.977 [0.975, 0.980] | 0.981 [0.979, 0.983] | 0.998, 0.998 | win: 5.3% on the shortest fifth (bucket 1), 1% on the longest |
| E2 | `phrase_freq` pays vector assigns and an insertion sort per candidate; the position tail grows a vector by doubling | a two-slot `pair_match` with no ordering vectors; the tail cache reserved to a block | 0.969 [0.966, 0.972] | 0.973 [0.971, 0.977] | 1.000, 1.001 | win: 3% in every fifth |
| E3 | Candidates that cannot beat the k-th score need no position reads: the phrase frequency is at most the smallest term count, already decoded | `can_compete` before `phrase_freq` when a minimum competitive score is set (Lucene mode bounds by the smallest count; Xapian mode computes the exact score, which positions do not change) | 1.010 [1.009, 1.012] (no pruning, no change) | 0.883 [0.858, 0.906] | 1.007, 1.002 | win for `TOP_k`: fifths 0.97, 0.94, 0.90, 0.88, 0.73 |
| E4 | Each phrase scorer builds four 256-entry weight tables; one is enough | `bm25_idf` for the per-term idf, no default-member table | 0.998 [0.996, 1.000] | 0.986 [0.984, 0.987] | 0.986, 0.996 | loser (inside the A/A drift; 1.8% on the shortest fifth against an A/A of 1.3%); removed |
| E5 | `position_index` pays an overflow test per summed count | one 64-bit sum, one test | 0.992 [0.990, 0.994] | 0.987 [0.985, 0.988] | 0.997, 0.998 | marginal win, 0.5 to 1%; kept because the loop is simpler |

Not tried, with the reason: the pair/bigram posting prototype (section 7) is a format change; a
second dictionary-walk or term-info sharing pass (the 41 small allocations that remain cost about
6% of a short query by the profile, and no single site is more than a fifth of that).

## 6. The result against Tantivy

The landing version (E1, E2, E3, E5) against main d6f15d6 and Tantivy 0.26, 8 rounds, A/A in the
same rotation, the 300 gate phrases (`phrases` row of the table is the B/A over main):

| Command | B/A over main [95%] | A/A | main / Tantivy | this branch / Tantivy [95%] |
|---|---|---|---|---|
| `COUNT` | 0.946 [0.941, 0.951] | 1.012 | 1.425 | 1.348 [1.318, 1.377] |
| `TOP_10` | 0.823 [0.800, 0.843] | 0.998 | 1.357 | 1.116 [1.066, 1.166] |
| `TOP_100` | 0.906 [0.892, 0.919] | 0.992 | 1.405 | 1.273 [1.237, 1.309] |
| `TOP_1000` | 0.944 [0.939, 0.950] | 0.999 | 1.403 | 1.325 [1.297, 1.354] |
| `TOP_100_COUNT` | 0.946 [0.943, 0.950] | 1.006 | 1.400 | 1.325 [1.299, 1.356] |

The target (at most 1.0 on `COUNT` and `TOP_10`) is not met: `COUNT` is 5% faster and still 1.35
times Tantivy; `TOP_10` is 18% faster and 1.12 times. By sums of time `COUNT` is 109.2 ms against
Tantivy's 92.2 (1.18).

Other axes. Time elsewhere (all 962 gate queries, 6 rounds, main = 1): `COUNT` 0.968 [0.966,
0.970], `TOP_10` 0.940, `TOP_100` 0.978, A/A 1.003, 0.993, 1.005; by class the intersections are
0.988, the unions 0.986, the negations 0.983 (`COUNT`), none above the A/A band. Memory: maximum
resident set 99.91 MB against 99.93 MB on the 300-phrase run, peak footprint +20 KB; the
recycling stacks hold at most 8 blocks of 4.4 KB, 1.2 KB and 2.6 KB per thread (66 KB). Disk: no
format change, identical. Indexing and merge throughput: not touched by the changes except that
merge uses the same iterators; merging the 100k flushed index, six alternating runs, 7.95 s main
against 7.80 s here (median, all runs 7.56 to 8.06 s). Platforms: measured on this Intel Mac
(macOS 15.6, AppleClang 17) only; the Linux, FreeBSD and arm64 hosts are not measured (they are
the lead's), and the thread-local stack is the one change that depends on the platform's
allocator, so those runs are needed before landing.

What it saves: per query, `COUNT` goes from 375 to 364 microseconds on average (112.5 and 109.2
ms for the 300), `TOP_10` from 341 to 277 (102.3 and 83.2 ms). A million phrase queries a day
save 11 CPU-seconds as `COUNT` or 64 CPU-seconds as `TOP_10`; at a few cents per core-hour that
is far below a cent a day. The saving is negligible in dollars or power at any plausible query
rate; the value is the benchmark ratio.

## 7. What would reach Tantivy: the pair posting, measured as an estimate

No trim in sections 5 and 6 changes the shape: the loss is a constant factor on every query, and
the changes recovered about a quarter of it for `COUNT` and two thirds for `TOP_10`. What remains
for `COUNT` is spread over the same areas as before (postings navigation near parity with
Tantivy's; the build and teardown; the phrase body; 41 small allocations; `verify`), each worth a
few percent. Neither Tantivy nor Lucene rejects a candidate before it reads positions, so there is
no cheaper necessary test to copy; the only way to avoid the reads is to not have the candidates,
which is a pair (bigram) posting: a posting list per adjacent pair of terms, holding the phrase
frequency as its wdf, so a two-term phrase is one list (its `COUNT` is the list's df, its score
needs no positions) and a longer phrase intersects the pair lists of its adjacent terms before
reading any position.

Cost, counted on the 1M corpus (`.scratch/bigram.cc`, the first million documents, 295 M tokens,
36.6 M distinct adjacent pairs, 244 M (document, pair) postings in all). Indexing only the pairs
whose both terms are frequent keeps the lists long and the dictionary small:

| Both terms with df at least | Pair lists | Postings | At about 0.9 byte a posting | Share of the 576 MB segment | Gate phrases covered |
|---|---|---|---|---|---|
| 30,000 (3%) | 316 k | 76 M | 68 MB plus the dictionary | about +12% | 24 of 300 (8%) |
| 10,000 (1%) | 2.1 M | 125 M | 113 MB plus the dictionary | about +21% | 74 of 300 (25%) |
| 3,000 (0.3%) | 6.7 M | 169 M | 152 MB plus the dictionary | about +30% | 139 of 300 (46%) |

Bound on the benefit (not a measurement): if a covered phrase's `COUNT` costs the 3 microseconds of
a one-term lookup, the `COUNT` geometric mean against Tantivy goes from 1.35 to 0.86 (df 30,000),
0.40 (df 10,000), 0.16 (df 3,000). The geometric mean reaches 1.0 when about 30% of the queries
are covered at half of Tantivy's time, so the 1% threshold (+21% disk) is about where `COUNT`
reaches parity on this phrase set, and the set is real-query derived, so the share covered is
workload dependent. The uncovered phrases (a rare term with anything) stay at the ratios of
section 6, which is where most of the geometric mean sits.

Not prototyped: it is a writer, merge, inverter and reader change with deletes and reordering
interacting with a second postings family, so it needs fixtures, byte identity, decoder fuzzing
and a decoder-safety review before it lands, and its indexing cost (a pair is known only once a
term's df is) is unmeasured. It trades about +12% to +21% disk against Tantivy's 658 MB for the
phrase row, so it is the lead's and the architect's decision whether a phrase row justifies it.

## What is left (Next)

1. The pair posting above, if the disk is acceptable; it is the only item found that reaches 1.0.
2. Without a format change, a second pass over the build (41 small allocations, about 6% of a
   short query; `term_info` copies its `impacts` vector four times per term) and over `verify`
   (3.6% of a short query), each worth 1 to 3%.
3. Measure the landing version on Linux, FreeBSD and arm64 (the thread-local block stack and the
   allocator it avoids are platform dependent).
