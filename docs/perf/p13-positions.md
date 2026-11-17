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
