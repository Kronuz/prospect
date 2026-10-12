# Phase 2 design: single-segment engine

This document specifies what Phase 2 builds: the segment file, the term dictionary,
postings with skip data, positions, the in-memory writer, the Xapian-compatible
analyzer, scorers, weighting, collectors, and the parity and benchmark harnesses.
Later phases add segments, deletes, doc values, and points on top of these formats
without changing them.

## Conventions

- Every fixed-width integer in the file is little-endian, as in `bitpack`.
- Variable-length integers are LEB128 (`varint::leb128`).
- Ordinals are segment-local document positions `0 .. doc_count - 1`. Docids are the
  host's stable ids; in a fresh single segment, `docid = ordinal + 1` (Xapian docids
  start at 1). The `docids` section maps ordinals to docids from the first format
  version, so merges and replaces can break that equality later.

## Segment file

One file per segment, written once, sequentially, then fsynced:

```
[header 32 B][section]...[section][table of contents][footer 48 B]
```

| Header field | Width |
|---|---|
| Magic `STRSEG01` | 8 |
| Format version | u32 |
| Reserved (zero) | 4 |
| Segment id | 16 |

The header's integrity is covered by the footer's CRC of the table of contents, which
lists the header as region 0.

Sections start at 64-byte-aligned offsets. A section is a run of 64 KiB regions; the
table of contents stores each region's CRC-32C.

| Table of contents | Width |
|---|---|
| Section count | u32 |
| Per section: kind | u16 |
| Per section: reserved | u16 |
| Per section: offset | u64 |
| Per section: length | u64 |
| Per section: CRC-32C of the whole section | u32 |
| Per section: region CRC-32C array, one per 64 KiB | u32 each |

| Footer field | Width |
|---|---|
| Table of contents offset | u64 |
| Table of contents length | u64 |
| Table of contents CRC-32C | u32 |
| Document count | u32 |
| Minimum docid | u64 |
| Maximum docid | u64 |
| Footer CRC-32C (of the preceding 40 bytes) | u32 |
| Tail magic `SEND` | 4 |

The footer is 48 bytes: 44 bytes of fields and the 4-byte tail magic.
Opening reads the footer first; a wrong tail magic or CRC rejects the file before
anything else is read.

### Sections

| Section | Contents |
|---|---|
| `terms` | Term blocks |
| `term_index` | An `fst` over the first term of every block, value = block offset |
| `postings` | Per term: skip data, blocks, tail |
| `positions` | Per term: one position stream |
| `lengths` | Per-ordinal document length, bit-packed fixed width |
| `stored` | zstd blocks of stored records with an offset index |
| `docids` | Ordinal → docid, bit-packed; docid → ordinal via a sorted array |
| `stats` | Document count, total length, length minimum and maximum, docid bounds |

## Term dictionary (`termdict`)

A block tree over sorted terms.

- **Blocks** hold 25–48 consecutive terms and are cut at 48 terms or when the encoded
  size passes 1 KiB.
- **Entries** are prefix-compressed: shared length with the previous entry, suffix
  length, suffix, then the term info. Every 16th entry is a **restart point** with a
  shared length of 0; the block's offset table lists restart offsets only. A lookup
  binary-searches the restarts, then decodes at most 16 entries.
- **Term info:** flags byte (no wdf, no positions, single posting), document frequency,
  collection frequency, maximum wdf, the whole-list competitive pairs (as in skip data),
  and the postings and positions offsets as deltas from the block's base offsets. A
  single-posting term stores its ordinal and wdf instead of a postings offset.
- **Block index:** an `fst` maps each block's first term to the block's offset.
  `floor_value(term)` finds the only block that can hold the term.
- **Automaton matching** (prefix, wildcard, fuzzy) seeks: compute the smallest string
  the automaton can accept at or after the current position, `floor_value` to its
  block, scan the block's entries through the automaton, and repeat from the next
  string past the block. First-term keys are enough for this, and every accepted term
  is found.

## Postings

For a term with `df` documents, after the term info:

- `df == 1`: inlined in the term info.
- `df < 128`: the tail only: ordinal deltas, then `wdf − 1` unless the term is no-wdf.
- Otherwise: skip data, full blocks of 128, then the tail. A block is a `bitpack` FOR
  block of ordinal deltas, then, unless no-wdf, a PFOR block of `wdf − 1`.

### Skip data

- **Level 0**, one entry per block: last ordinal (delta), the block's end offset
  (delta), the positions stream offset and the index within the positions block where
  this postings block's positions start, and the competitive pairs.
- **Level 1**, one entry per 32 blocks, with the same fields over its range.
- **Competitive pairs** are the Pareto frontier of (wdf high, length low) over the
  range. Lengths are stored rounded down to a 1-byte log scale and wdf exactly, so a
  stored pair never understates a score bound; scoring itself reads exact lengths from
  `lengths`. When the frontier has more than four pairs, neighbors merge into
  (maximum wdf, minimum length), which keeps the bound valid.

**Size budget.** The Wikipedia corpus has 4.37 million full blocks. A level-0 entry is
about 8 bytes (two 2-byte deltas, a positions offset of 2–3 bytes, one or two 2-byte
pairs), about 35 MB in all; level 1 adds about 1 MB. With 934.9 MB of deltas and
wdf, the postings section is about 971 MB, 0.98× Lucene's 990.6 MB `.doc` and inside
the 1.02× budget (1,010 MB). The gate measures the real figure.

## Positions

Each term has one position stream. A document's positions are deltas restarting at
that document; the documents' runs are concatenated in ordinal order and packed into
PFOR blocks of 128 with an LEB128 tail. A postings skip entry points to the positions
block and the index within it where its documents start; a scorer decodes forward
from there, using each document's wdf as its run length. Terms flagged no-positions
have no stream.

## Reader safety

- Every offset and length read from the file (table of contents, term info, skip
  entries, position offsets) is checked against its section's bounds before use.
- A decoded block's last ordinal must equal its skip entry's; ordinals must increase
  and stay below the document count.
- Any violation fences the segment: it reports itself unavailable and serves nothing
  further. The codecs are already memory-safe; these checks keep corrupt values from
  stalling iterators.

## Integrity

- **At open:** footer, table of contents, header, and the small pinned sections
  (`term_index`, `stats`, `lengths`) are verified in full.
- **Regions:** `residency::file::open` takes the segment's region checksums. The
  synchronous backend verifies each 64 KiB region the first time a prefetch covers it
  (one atomic flag per region), which also faults the pages in. Asynchronous backends
  verify regions as they load them. A mismatch fences the segment.
- **Whole sections:** on merge, replication, and scrub.
- **`SIGBUS`:** a media error under mmap is a node failure. The process exits through
  its fatal-signal handler and the replication fault model covers the loss. The verified
  block cache (strict mode) reads with `pread`, where media errors are I/O errors.

## In-memory segment

- **Term map:** open-addressing table keyed by term bytes in an arena; each entry holds
  growable slices for ordinal deltas, wdf, and the position stream (LEB128 while
  buffering), plus df, collection frequency, and maximum wdf.
- Documents are inverted on the owner core; memory comes from the node-wide indexing
  budget.
- **Flush:** sort terms, stream them through the `termdict` builder, and re-encode each
  term's slices into blocks, skip data, positions, and tails in one sequential pass.

## Analyzer

A clean-room reproduction of Xapian `TermGenerator` behavior, specified from Xapian's
documentation and the oracle's vectors:

- **Words:** maximal runs of Unicode letters, digits, and marks; an apostrophe or
  similar joins letters inside a word; `&`, `+`, `#` suffix rules as recorded by the
  vectors (`C++`, `C#`).
- **Case:** lowercase folding by Unicode simple case mapping.
- **Positions:** consecutive per word, starting after the document's last position;
  positionless indexing omits them.
- **wdf:** each occurrence adds the field's wdf increment.
- **Prefixes:** a field prefix is prepended to every term it generates.
- **Stemming:** `STEM_NONE`, `STEM_SOME` (stemmed terms with a `Z` prefix, besides the
  unstemmed term), `STEM_ALL`, `STEM_ALL_Z`, with Snowball stemmers.
- **Stopping:** `STOP_NONE`, `STOP_ALL`, `STOP_STEMMED` with a stopper.
- **CJK:** n-grams under `FLAG_CJK_NGRAM`.

The oracle's term-generation vectors are extended to these inputs: prefixes, wdf
increments, each stop strategy with a stopper, and positionless indexing. The analyzer
passes every vector before the parity gate runs.

## Scorers

All scorers implement `strata::scorer`. Scoring scorers accept
`set_min_competitive_score(s)` from their parent.

| Scorer | Behavior |
|---|---|
| Term | Decodes one block at a time; `block_end` and `max_score` come from skip data; term-level bounds come from the term info |
| Conjunction | Leapfrogs from the lowest-cost scorer; block-max AND skips blocks whose summed maxima cannot compete |
| Required plus optional (Xapian `AND_MAYBE`) | Iterates the required side; adds optional scores where they match; skips optional work when required score plus optional bound cannot compete |
| Disjunction (top-k) | Block-max MaxScore at the top level, using each term's global bound to split essential and non-essential clauses; block-max WAND nested |
| Disjunction (exhaustive) | Windows of 4,096 ordinals with a bitset, for counts and aggregations |
| Phrase, near | Conjunction first, then positions |
| Filter, and-not | Non-scoring iterators |

Evaluation runs in windows of 4,096 ordinals; the query coroutine checks its
`task::budget` between windows.

## Weighting

- **Xapian BM25** (default): Xapian's formula and defaults (`k1 = 1`, `k2 = 0`, `k3 = 1`,
  `b = 0.5`, `min_normlen = 0.5`), exact lengths, exact `N`; the term bound uses the
  term's maximum wdf.
- **Lucene BM25** (`k1 = 1.2`, `b = 0.75`, Lucene's IDF) for the equal-scoring benchmark
  rows. Lengths are exact; Lucene quantizes them to one byte, so scores differ slightly
  and the rows say so.
- **Bool**: constant scores.

## Collectors

- **Top-k:** a fixed-size heap whose minimum feeds the minimum competitive score.
- **Count:** a single-term count returns the document frequency (Phase 2 has no
  deletes). Other queries count exhaustively; the result is Xapian's (lower, estimated,
  upper) triple, exact when counting is requested.
- **Top-k plus count:** counts every match and scores only documents in blocks whose
  bound beats the heap threshold.

## Parity harness

- The oracle's rank vectors come from an index built with `TermGenerator` (no stemmer)
  and queries parsed with `QueryParser` and `FLAG_DEFAULT`.
- strata builds its queries from the recorded `Query(...)` descriptions, which carry the
  operators and wqf, instead of reimplementing `QueryParser` in Phase 2.
- Pass: identical match sets, and top-10 (docid, weight) at relative error ≤ 1e-9.
  Scores within that tolerance count as ties ordered by docid, since summation order
  differs between engines.

## Benchmark settings

- Gate rows analyze with whitespace tokenization, as the baseline engines do.
- **Indexing row:** from JSON lines to a committed single segment, 1,000,000 documents
  (one segment fits every engine's writer: Tantivy caps a writer thread below 4 GB),
  each engine with a 2 GiB indexing memory budget and one indexing thread.
- **`termdict` row:** exact lookups of present and absent keys (absent keys sampled from
  the query log and from perturbed vocabulary terms) against Lucene's `seekExact` on the
  same vocabulary, in a microbenchmark that excludes the pipe.

## Test plan

- Scorers against a naive evaluator (full scans) on random queries over random and
  Wikipedia segments: same match sets and scores.
- Pruning against no pruning: identical top-k.
- `termdict` against `std::map`: lookups, floors, and automaton matches.
- Reader fuzzing at native speed, with executions per second and coverage recorded.
- A golden segment fixture compared byte for byte.
- Corruption tests: every truncation and random corruption of the fixture either fences
  the segment or answers queries without leaving its bounds.
