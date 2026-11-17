# P13: positions codecs, FOR, PFOR or Elias-Fano per block, and deltas against the document's length

Track P item P13 (strata `docs/PLAN.md`): positions with a per-block choice among FOR, PFOR and
Elias-Fano, and position deltas within a document coded against the document's length. Quest
`track-p`, criterion A10. Prior evidence: strata `docs/perf/query-gap-positions.md` (E7, 5M, on code
before the prospect query wins: EF per block saved 2.4% of positions, FOR cost 6.5%, decode was 8-10%
of a phrase row); this entry measures again on current main.

## Plan (written before the runs)

- **Today's layout.** Per term, every document's positions as the first one absolute and then gaps,
  in one stream cut into 128-value blocks across documents (`writer.hh` `write_positions`), each
  block `bitpack::encode_pfor`, which already picks the smaller of FOR and PFOR (and a constant
  block); fewer than 128 left over are a LEB128 tail. So "FOR or PFOR per block" is today's; the
  candidates are Elias-Fano as a third choice, and codes that use the document's length.
- **Offline first** (`bench/quick/p13_positions.cc`, every position of the 1M merged segment):
  1. Bytes of each candidate: FOR, Elias-Fano per block, min(PFOR, EF) per block, firsts and gaps
     split into two streams, firsts at the length's bit width, Rice codes whose parameter comes from
     the document (`k = bit_width(length / (count + 1)) - 1`, nothing stored), interpolative coding of
     each document's positions inside [1, length].
  2. Decode speed per position of each codec over the phrase queries' terms' blocks.
  3. Profile of main's phrase rows: the share of position block decoding.
- **Prototype** the candidate that can win, behind `PROSPECT_EXPERIMENT_P13` (0 is main), writer and
  reader, the 1M re-indexed and merged by the variant.
- **Instrument.** `classes_ab.py bench` (in process) for the A/B: the phrase classes (`phrase_2`,
  `phrase_3`, `phrase_gt3`; prospect has no proximity operator in the harness's queries, so no
  proximity class exists to measure) and guards (`terms`, `intersection_2`), commands `TOP_10`,
  `TOP_100`, `TOP_1000`, `COUNT`, seven rounds rotated, best of three, A/A from a copy of main's
  binary. Engines (Lucene 10.4, Tantivy 0.26) over `serve` by `classes_ab.py serve`. Deciding jobs
  under `quiet exclusive`.
- **Rule.** Per class and command, the run-level ratio B/A (each round's geometric mean over the
  class's queries, t interval over the rounds). A row wins when its upper bound is under 1.0 and under
  the A/A band and its ratio is at most 0.97; it loses when its lower bound is over 1.0 and over the
  band and its ratio is at least 1.03. Keep a codec if no row loses and either a phrase row wins or the
  segment shrinks by at least 1% with every row inside the band. Results identical: `dump` (counted
  and uncounted) byte for byte with main's and `verify` clean over the 962 gate queries.

## Setup

- Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, Release, `-march=native`; prospect main
  `eef601d`.
- Index: the 1M gate corpus, `prospect_engine index` then `merge` (one reordered segment, two reorder
  threads): 576.3 MB, positions section 348.1 MB (60%). Lucene 10.4's `.pos` is 382.1 MB and Tantivy
  0.26's `.pos` 411.4 MB, so prospect's positions are already the smallest (9% under Lucene's).

## Offline: bytes of each candidate (experiment 1)

`p13_positions sizes` over the merged 1M segment: 294,827,020 positions in 126,703,012 documents'
postings; 2,190,828 full blocks hold 95% of them, 84.7% of those blocks carry PFOR exceptions.
43.0% of positions are a document's first; 68.6% of postings have one position, 14.9% two, 5.7%
three, 10.8% four or more. Every position lies in [1, length]. Stream bytes only (the 3.3 MB of
headers and count blocks are the same in every row):

| Candidate | MB | Against today |
|---|---:|---:|
| PFOR per block, LEB128 tail (today; FOR is inside PFOR's choice) | 344.74 | |
| FOR per block | 367.87 | +6.71% |
| Elias-Fano per block | 342.01 | −0.79% |
| min(PFOR, EF) per block (EF smaller on 59.7% of blocks) | 338.78 | −1.73% |
| min(PFOR, EF), EF only when it saves at least 5% (7.9% of blocks) | 343.08 | −0.48% |
| min(PFOR, EF), EF only when it saves at least 10% (no block) | 344.74 | 0 |
| Gaps stored minus one (strictly increasing), PFOR | 344.48 | −0.08% |
| Firsts and gaps split into two PFOR streams | 341.32 | −0.99% |
| The same, each min(PFOR, EF) | 335.52 | −2.67% |
| Firsts at `bit_width(length)`, gaps min(PFOR, EF) | 328.24 | −4.79% |
| Rice from the document, k − 1, every position | 320.67 | −6.98% |
| Rice from the document, `k = bit_width(length / (count + 1)) − 1`, every position | 306.72 | −11.03% |
| Rice from the document, k + 1, every position | 319.55 | −7.31% |
| Interpolative per document inside [1, length] | 316.96 | −8.06% |

Elias-Fano saves little because no block gains much: EF beats PFOR on 60% of blocks, by under 10%
on every one. Coding against the document's length is where the bytes are: a Rice code whose
parameter comes from the document's length and position count (both known to the reader, nothing
stored) takes 11.0% off the positions, 38.0 MB, 6.6% of the segment. Those codes are per document and
bit-serial, so they read differently from a block codec; experiment 2 prices that.

## Offline: decode speed and profile share (experiment 2, job `261117-032048-bef4fe`, exclusive)

`p13_positions decode`, best of seven, over the blocks of the phrase queries' 573 terms and over the
50 terms with the most positions:

| Codec | ns/position (573 terms) | ns/position (50 terms) | Against PFOR | MB, 573 terms' full blocks |
|---|---:|---:|---:|---:|
| PFOR (today) | 0.287 | 0.273 | 1.0 | 90.49 |
| Elias-Fano per block | 0.723 | 0.681 | 2.5x slower | 89.31 |
| min(PFOR, EF) per block (EF on 66% of blocks) | 0.588 | 0.584 | 2.0-2.1x slower | 88.71 |
| Rice from the document's length | 4.703 | 4.624 | 16.4-16.9x slower | 80.05 (every position) |

Position block decoding (`bitpack::decode`, `unpack_simd`, `positions_reader::decode_block`) is
7-10% of main's phrase rows in a `sample` profile of `bench` over every phrase query (`TOP_10` and
`COUNT`). At 16x PFOR's cost, Rice codes would add more than the whole decode share again to every
phrase row, for 6.6% of the segment: the bytes do not pay for it, so Rice was not prototyped. EF per
block was, as the only candidate within a plausible speed budget.

## Prototype: min(PFOR, EF) per block

`docs/perf/p13-positions.patch` (`PROSPECT_EXPERIMENT_P13=1`): the writer encodes each full
positions block both ways and keeps Elias-Fano when it is smaller (header `0xC0 | l`, `l` the low
bits); the reader decodes it to the same 128 gaps. The 1M re-indexed and merged by the variant:
positions 342.13 MB against 348.09 (−1.71%), file 570.32 MB against 576.29 (−1.04%). `dump` of both
identical, `verify` 0 mismatches over the 962 queries; full `ctest` with the patch applied, 52 of
52 (job `261117-032521-37a35e`).

## Result: EF per block against main (job `261117-032504-3bf776`, exclusive)

In process, 1M reordered, seven rounds rotated, best of three; B is the variant on its own index, C
is a copy of main's binary (the A/A). Run-level ratio [95%]:

| Class | Command | EF/main | A/A | Row |
|---|---|---|---|---|
| phrase_2 | `TOP_10` | 1.061 [0.975, 1.155] | 0.986 [0.931, 1.044] | inside |
| phrase_2 | `TOP_100` | 1.057 [1.012, 1.104] | 1.002 [0.965, 1.040] | loses |
| phrase_2 | `TOP_1000` | 1.055 [0.984, 1.132] | 0.973 [0.916, 1.034] | inside |
| phrase_2 | `COUNT` | 0.993 [0.850, 1.160] | 0.991 [0.857, 1.147] | inside |
| phrase_3 | `TOP_10` | 1.095 [1.019, 1.177] | 1.044 [1.004, 1.087] | loses |
| phrase_3 | `TOP_100` | 1.093 [1.024, 1.167] | 0.990 [0.925, 1.059] | loses |
| phrase_3 | `TOP_1000` | 1.100 [1.031, 1.175] | 1.043 [0.947, 1.149] | slower, inside the band |
| phrase_3 | `COUNT` | 1.021 [0.947, 1.102] | 0.997 [0.968, 1.027] | inside |
| phrase_gt3 | `TOP_10` | 1.028 [0.927, 1.141] | 1.029 [0.966, 1.096] | inside |
| phrase_gt3 | `TOP_100` | 1.049 [0.989, 1.113] | 1.019 [0.909, 1.143] | inside |
| phrase_gt3 | `TOP_1000` | 1.086 [1.010, 1.168] | 1.021 [0.931, 1.120] | slower, inside the band |
| phrase_gt3 | `COUNT` | 1.076 [1.007, 1.150] | 0.971 [0.907, 1.041] | loses |
| terms | four commands | 0.976-1.033 | 1.024-1.067 | inside |
| intersection_2 | four commands | 0.977-1.054 | 0.980-1.011 | inside |

No row wins; four phrase rows lose (1.06-1.10x slower) and every phrase row but one leans above
1.0. Over `serve` (job `261117-032507-1bd2ec`, five rounds) the variant against main is 0.97-1.16,
with `phrase_2` `TOP_100` 1.103 [1.015, 1.199], `phrase_2` `COUNT` 1.074 [1.033, 1.116] and
`phrase_3` `TOP_100` 1.076 [1.031, 1.122] above 1.0: the same direction from a second instrument.

## Against the baselines (job `261117-032507-1bd2ec`, over `serve`)

| Class | main/Lucene | main/Tantivy | EF/Tantivy |
|---|---|---|---|
| phrase_2 | 0.81-0.90 | 1.10-1.40 | 1.11-1.50 |
| phrase_3 | 0.66-0.71 | 1.10-1.25 | 1.12-1.34 |
| phrase_gt3 | 0.71-0.77 | 1.17-1.22 | 1.13-1.40 |

Main is 1.1-1.5x faster than Lucene 10.4 on phrases and Tantivy 0.26 is 1.1-1.4x faster than main;
EF per block widens the Tantivy gap. Positions decode is not where that gap is: the profile's top
frames are `postings_iterator::advance`, `phrase_freq`, `find` and `positions_reader::read`.

## Indexing speed (job `261117-032510-cabfbf`, exclusive)

`index` of the 1M corpus (flush only), alternating, three rounds: main 28.89, 29.36, 28.07 s, the
variant 30.09, 29.65, 28.63 s; per round 1.04, 1.01, 1.02 (medians 28.89 and 29.65, +2.6%). Peak
RSS 384-432 MB against 390-415 MB, no difference beyond the spread. The flushed, unmerged index is
691,216-691,500 KB against 691,360-691,624 KB: EF saves nothing on small segments.

## Four axes

| Axis | Before (main) | After (EF per block) | Source |
|---|---|---|---|
| Query speed, phrases | 1.0 | 0.99-1.10; four rows lose, none wins | job `261117-032504-3bf776` |
| Query speed, guards (terms, intersection 2) | 1.0 | 0.98-1.05, inside the A/A band | same job |
| Indexing speed | 28.89 s (1M, median of 3) | 29.65 s, +2.6% (1.01-1.04 per round) | job `261117-032510-cabfbf` |
| Memory | 384-432 MB peak RSS indexing | 390-415 MB, same; reader unchanged | same job |
| Disk | 576.29 MB merged, positions 348.09 MB | 570.32 MB (−1.04%), positions 342.13 MB (−1.71%) | `p13_positions sizes`, the variant's index |

## Verdict

**Removed, both candidates.** Elias-Fano per block saves 1.04% of the segment and costs 2.0-2.5x
PFOR's decode, which shows as 1.06-1.10x slower phrase rows and 2.6% slower indexing: by the rule
(keep only on a phrase win, or at least 1% smaller with every row inside the band) it loses. Rice
codes from the document's length save 11.0% of positions (6.6% of the segment) at 16x PFOR's decode
against a 7-10% decode share of phrase rows: removed offline. Today's PFOR per block (FOR inside its
choice) stays; no format bump. The patch stays here as the record of what was measured. Not
measured: arm64, Linux, FreeBSD, the gate VM's 5M, PISA (not on this host); none of them changes a
2.5x decode cost into a win on a 1% byte saving.
