# Phase 2 design: single-segment engine

This document specifies what Phase 2 builds: the segment file, the term dictionary,
postings with skip data, the in-memory writer, scorers, BM25, and collectors. Later
phases add multiple segments, deletes, doc values, and points on top of these
formats without changing them.

## Segment file

One file per segment, written once, sequentially, then fsynced:

```
[header 32 B][section]...[section][table of contents][footer 32 B]
```

- **Header:** magic `STRSEG01`, format version (u32), segment id (16 bytes), and the
  header's CRC-32C.
- **Sections** are 64-byte aligned, so SIMD loads and mmap prefetch never straddle
  sections. Each section is a sequence of 64 KiB regions; a region's CRC-32C is stored in
  the table of contents, which lets `residency` verify a region when it first loads it.
- **Table of contents:** for each section, its kind (u16), offset (u64), length (u64),
  and CRC-32C of the whole section; then the region CRC-32C arrays.
- **Footer:** offset of the table of contents, its length and CRC-32C, the document
  count, the segment's ordinal range, and the footer's CRC-32C.

Opening a segment verifies the header, the footer, and the table of contents, and pins
the small sections (term index, skip roots, lengths). Large sections are verified per
region on first load and in full on merge, replication, and scrub.

| Section | Contents |
|---|---|
| `terms` | Term blocks (below) |
| `term_index` | An `fst` over the first key of every term block, value = block offset |
| `postings` | Per term: skip data, then FOR/PFOR blocks of ordinal deltas and wdf, then the LEB128 tail |
| `positions` | Per term, per document: position deltas, PFOR blocks of 128 and an LEB128 tail |
| `lengths` | Per-document length (sum of wdf over non-boolean terms), bit-packed fixed width |
| `stored` | zstd blocks of stored records with an offset index |
| `docids` | Ordinal → docid, bit-packed; docid → ordinal via a sorted array |
| `stats` | Document count, total length, length minimum and maximum |

## Term dictionary (`termdict`)

A block tree over sorted terms.

- **Term blocks** hold 25–48 consecutive terms. A block starts with its entry count and
  a LEB128 offset table; each entry is the term's length shared with the previous term,
  its suffix, and its term info: document frequency, collection frequency, postings
  offset, positions offset, and a flags byte (no wdf: all postings are boolean; single
  posting: the ordinal inlined instead of a postings offset). Offsets are deltas from the
  block's base offsets.
- **Block index:** an `fst` maps each block's first term to the block's offset.
  `floor_value(term)` finds the only block that can hold the term; one block scan
  answers the lookup.
- **Prefix, wildcard, and fuzzy** intersect the automaton with the index to find
  candidate blocks, then scan the blocks.
- **Builder:** consumes terms in sorted order (from the in-memory segment's sort at
  flush), cutting blocks at 48 terms or when the block's encoded size passes 1 KiB.

## Postings

For a term with `df` documents:

- `df == 1`: inlined in the term info; no postings bytes.
- `df < 128`: the LEB128 tail only: ordinal deltas, then wdf − 1 (omitted for no-wdf terms).
- Otherwise, full blocks of 128 followed by the tail. Each block is a `bitpack` FOR
  block of ordinal deltas, then (unless no-wdf) a PFOR block of wdf − 1.
- **Skip data** precedes the blocks. Level 0 has one entry per block: the block's last
  ordinal, the byte offset of its end, its positions offset, and its competitive
  (wdf, doclen) pairs (the Pareto frontier of wdf high and doclen low, at most four
  pairs). Level 1 has one entry per 32 blocks with the same fields over its range. Both
  levels are LEB128 delta-coded and read without decoding blocks.

## Positions

Positions of one (term, document) are sorted and unique. They are stored per document
as deltas: full PFOR blocks of 128 and an LEB128 tail, located through the postings
skip entries and a per-block running count.

## In-memory segment

- **Term map:** open-addressing hash table keyed by term bytes in an arena; each entry
  holds growable byte slices for ordinal deltas, wdf, and positions (LEB128 while
  buffering), plus df and collection frequency.
- **Documents** are inverted on the owner core as they arrive; the arena and slices
  come from the node-wide indexing budget.
- **Flush:** sort the terms, then stream them through the `termdict` builder and
  re-encode each term's slices into blocks, skip data, and tails. One sequential pass
  writes every section; the table of contents and footer follow.

## Scorers

All scorers implement `strata::scorer`; scoring ones also receive the minimum
competitive score from their parent (`set_min_competitive_score`), which block-max
pruning uses.

| Scorer | Behavior |
|---|---|
| Term | Decodes one block at a time; `block_end` and `max_score` come from skip data |
| Conjunction | Leapfrogs from the lowest-cost scorer; block-max AND skips blocks whose summed maxima cannot compete |
| Disjunction (top-k) | Block-max MaxScore at the top level, block-max WAND nested, chosen by cost |
| Disjunction (exhaustive) | Windows of 4,096 ordinals with a bitset, for counts and aggregations |
| Phrase, near | Conjunction first, then positions |
| Filter, and-not | Non-scoring iterators |

Evaluation runs in windows of 4,096 ordinals; the query coroutine checks its
`task::budget` between windows.

## Weighting

- **Xapian BM25** (default): Xapian's formula with its defaults (`k1 = 1`, `k2 = 0`,
  `k3 = 1`, `b = 0.5`, `min_normlen = 0.5`), exact lengths and exact `N`; checked
  against the oracle's rank vectors at relative error ≤ 1e-9.
- **Lucene BM25** (`k1 = 1.2`, `b = 0.75`, Lucene's IDF): for equal-scoring benchmark
  rows against Lucene and Tantivy. Exact lengths are used; Lucene quantizes lengths to
  one byte, so scores differ slightly, which the benchmark rows state.
- **Bool**: constant scores.

## Collectors

- Top-k: a fixed-size heap; its minimum is fed back as the minimum competitive score.
- Count: exhaustive counting with pruning off; returns Xapian's (lower, estimated,
  upper) triple, which is exact when counting is requested.
- Top-k plus count: top-k with counting, as the harness's `TOP_100_COUNT`.

## Benchmark engine

A `strata` engine for the search-benchmark-game harness (index and serve over the same
pipe protocol) measures every Phase 2 gate row next to Lucene, Tantivy, and Xapian.
