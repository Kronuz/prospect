# strata architecture

`strata` is an embeddable search engine in C++20. Its write path produces immutable
segments, its read path decodes compressed block postings with block-max pruning, and
its API is coroutine-based so a host reactor never blocks on it. It is MIT-licensed
and built from small standalone libraries. Replication lives in a separate layer,
described in [REPLICATION.md](REPLICATION.md).

## Layers

```
 Xapiand            HTTP, schema, routing, placement, geo (HTM), sort-key functions
 ───────────────────────────────────────────────────────────────────────────────
 strata::group      one replication group per shard (consensus + journal + core)
 strata core        Index · Snapshot · Search · Segment
 ───────────────────────────────────────────────────────────────────────────────
 building blocks    task · residency · bitpack · fst · termdict · automata · docset ·
                    bkd · fusefilter · sortable · unicode · snowball · analysis ·
                    queryparser · faultfs · benchkit · compressors · hashes · io · fs
```

The core has no network, consensus, schema, or geometry code. Xapiand maps its schema
to terms, slots, and analyzers, and supplies geo shapes, scored filters, and sort-key
functions through engine APIs.

## Data model

- **Index** is one shard. It owns a directory, one writer on one owner core, a
  metadata store, and a sequence of immutable segments listed in a manifest.
- **Document**: a docid, terms with wdf and positions, doc values by slot, a stored
  record, an optional primary key, and a version.
- **Terms** are opaque byte strings. The host encodes field prefixes and types into
  them. Iteration over any byte prefix is ordered. An optional field id groups
  statistics only; it never limits prefix or wildcard queries.
- **Docids** are per-shard, monotonic, and never reused. A replace keeps the
  document's docid. Merges preserve docids, and each segment maps docids to local
  ordinals. `lastdocid` is persisted in the manifest.
- **Doc values** are keyed by the host's 32-bit slot through a per-segment slot
  directory. Presence is sparse. Column types:
  - sorted-numeric: multi-valued i64, u64, or f64;
  - sorted-set: multi-valued bytes with a per-segment ordinal dictionary;
  - binary: opaque value lists, the compatibility path for Xapiand's `StringList`s.
  Each column keeps its value count and lower and upper bounds.
- **Points** index sorted-numeric slots (1-D) and interval slots (2-D start and end).
- **Stored record**: an opaque byte string (Xapiand's MessagePack).
- **Term lists**: opt-in per field, storing each document's terms with wdf and
  positions. Without them, the host re-analyzes the stored record.
- **Metadata**: a key-value store whose current version is referenced by the
  manifest, so metadata commits atomically with documents.
- **Identity**: an index UUID and a format version in every manifest.

### Three counters

| Counter | Meaning |
|---|---|
| revision | Commit counter, +1 per commit that changes something; Xapiand's `_revision` |
| seqno | Per-operation admission order; under groups, the pair (log index, position in batch) |
| version | Per-document, user-visible `_version` |

## Write path

```
 receiving core: prepare (analysis) ──message──▶ owner core: stage 1 (sequential)
                                                    docid, version check, pk map
                                                          │
                                                          ▼
                                               stage 2: inversion into the buffer
                                                          │ budget or refresh
                                                          ▼
                                       flush (background) → segment file → snapshot
```

1. **Ownership.** One writer per shard, owned by one core. Operations reach it as
   messages and get a total order at admission.
2. **Prepare.** Analysis runs on the receiving core and ships a prepared document:
   terms, positions, slot values, stored record.
3. **Stage 1 (sequential).** Assigns docids, checks expected versions against the
   live version map and then the segments, and updates the primary-key map. It is
   deterministic.
4. **Stage 2 (inversion).** Appends to the in-memory segment: an open-addressing hash
   map of terms with bytes in an arena, posting slices for doc deltas, wdf, and
   positions, doc-value arrays, interval arrays, and a stored-record arena.
5. **Deletes and updates.** An update is delete-by-key plus add with the same docid.
   Inside the buffer, order is positional (the document count at delete time). At
   flush and refresh, deletes resolve against older segments through the
   primary-key filter and a term-block lookup, and set bits in a new live-docs
   generation.
6. **Batches.** A transaction is an atomic batch. Cancel undoes its buffered
   operations.
7. **Memory.** One indexing budget covers the node across all shards. When it is
   exceeded, the largest buffer flushes first.
8. **Flush** runs off the reactor and writes one segment file sequentially.
9. **Refresh** publishes a new snapshot with the flushed segments and live docs
   (near real time, no fsync). The default interval is 1 s.
10. **Commit**: segment fsync, live-docs fsync, manifest write and fsync, directory
    fsync (`F_FULLFSYNC` on macOS). Revision increments only when something changed.
11. **Merge**: a tiered policy, rate-limited, in the background. Postings re-encode
    block by block; stored-record blocks copy without recompression when no document
    in them is deleted.
12. **Partitioned writers** are an option behind a gate: hash the primary key to a
    partition, give each partition a buffer on another core, keep merges
    partition-local, and cut commits with a marker message through FIFO channels.
    They are built only if single-core inversion measures below twice the target
    ingest rate.

## Segment format

A segment is one file: sections, a table of contents, and a footer with the format
version and a CRC32C. Live docs are per-generation side files.

| Section | Contents |
|---|---|
| Term blocks | Prefix-compressed blocks of 25–48 terms with document frequency, collection frequency, and postings pointers |
| Term index | A small resident FST over block prefixes |
| Postings | 128-docid blocks: FOR doc deltas, PFOR wdf; VByte tails; two skip levels with competitive (wdf, doclen) pairs |
| Positions | 128-position PFOR delta blocks |
| Lengths | Exact per-document length (sum of wdf over non-boolean terms), bit-packed fixed width, with segment minimum and maximum |
| Doc values | Slot directory and columns (numeric: delta, GCD, table, or bit-packed; sorted-set ordinals; binary) |
| Points | BKD trees, 1-D numeric and 2-D interval |
| Stored records | Compressed blocks (zstd) with an offset index |
| Term lists | Opt-in per field |
| Primary-key filter | Binary fuse filter |
| Docid map | Segment ordinal to docid, with a docid lookup |

Codec rules:

- Blocks use a fixed 4×32-bit interleaved lane layout on disk. SSE4.1 and NEON decode
  it natively; AVX2 processes two lane groups.
- A term with only boolean postings (wdf 0) stores no wdf. A block whose wdf are all 1
  stores a flag instead.
- Postings, positions, and term lists carry segment ordinals, not docids. A
  replaced document keeps its docid at a late ordinal, so docid tie-breaks and
  docid-order walks go through the docid map.
- A term with document frequency 1 stores its ordinal in the term block.
- Tails (fewer than 128 entries, which is most terms' whole posting list) use the
  `varint` format; a tail encoding that measures smaller is added to `varint`.
- Block maxima store competitive (wdf, doclen) pairs, never precomputed scores, so
  distributed rescoring with global statistics keeps valid bounds.

## Manifest and commit

`manifest-N` is checksummed and carries the index UUID, format version, revision,
`lastdocid`, `N`, total length, the metadata reference, the segment list with sizes
and CRCs, live-docs generations, and, under groups, the applied log position.

- A file is deleted only when no snapshot, manifest in use, or journal checkpoint
  references it.
- Standalone recovery opens the highest valid manifest; under groups it opens the
  manifest the journal checkpoint references. Recovery never falls back silently, and
  corruption fails closed.
- Checksums: footers and small sections at open; stored-record and compressed blocks
  on every read; 64 KiB region checksums when the residency layer loads a region;
  whole files on merge, replication, and scrub.
- Decoders are memory-safe on corrupt input and fuzzed.

## Read path

```
 Query ─rewrite─▶ plan ─residency request─▶ (suspend) ─▶ windows of docids ─▶ collectors
        wildcard/fuzzy: term blocks ∩ automaton
        range: BKD or doc values; geo: 2-D interval BKD + scored filter
```

1. **Snapshot.** A query takes a per-core snapshot handle (published to each core by
   message, so there are no shared atomic refcounts). Segments never change under it.
2. **Rewrite.** Multi-term queries resolve to term sets per segment by intersecting
   term blocks with automata. Expansions combine as `SYNONYM`, as in Xapian.
3. **Residency.** At query start, the plan requests the postings ranges it needs
   (positions only for phrase and near) and suspends until they are resident.
4. **Scorers** are iterators with `doc()`, `next()`, `advance(target)`, `cost()`, and,
   when scoring, `score()` and `max_score_up_to(target)`.
   - Conjunctions leapfrog from the cheapest iterator.
   - Top-level disjunctions use block-max MaxScore; nested scoring disjunctions use
     block-max WAND. A cost model measured on the benchmark queries picks between
     them.
   - `FILTER` and `AND_NOT` use non-scoring iterators; repeated filters are cached
     per segment as bitsets.
   - `SYNONYM`, `ELITE_SET`, `MAX`, `SCALE_WEIGHT`, `AND_MAYBE`, and `XOR` follow
     Xapian semantics, checked against oracle vectors.
   - Scored filters (ranges, geo) expose `weight()` and `max_weight()`.
5. **Bulk scoring** runs over docid windows (4,096 to start, tuned). The query
   coroutine yields between windows.
6. **Scoring.** BM25 implements Xapian's formula exactly: smoothed IDF, the k3
   query-frequency factor, `min_normlen`, the k2 term, `(k1 + 1)` scaling, and defaults
   `k1=1`, `k2=0`, `k3=1`, `b=0.5`, `min_normlen=0.5`. Bool weighting is also
   available.
7. **Statistics.** `N` and average length are exact (from the length section and
   live docs). Termfreq counts deleted documents until merge, and the parity gate
   measures the drift.
8. **Collectors.**
   - Top-k by score with a rising threshold that feeds pruning; ties break by docid
     ascending.
   - Sort by computed sort keys: host-supplied, serializable functions over doc
     values.
   - Collapse by slot with `collapse_max`.
   - Aggregations run columnar over doc values in batches.
   - Totals follow Xapian's lower bound, estimate, and upper bound; `_percent`
     follows Xapian's formula.
   - Exhaustive mode turns pruning off when aggregations or exact totals are
     requested.
9. **Expansion.** Relevance-feedback expansion scores candidate terms from the top
   documents' stored term lists.

## Concurrency and async

- **Scheduler concept.** The engine defines `post(handle)` and `now()`. The host's
  per-core executor is the run queue; the engine owns none. Bindings: Asio (for
  `reactor`) and a deterministic inline scheduler for tests and simulation.
- **I/O concept.** Async reads and residency requests, implemented on
  `cluster/completion` (io_uring, POSIX AIO, Asio adapter).
- **`task<T>`.** Resumes its awaiter on the awaiter's executor, propagates exceptions,
  uses symmetric transfer, observes cancellation at yield points, and allocates frames
  from a per-core pool with no `malloc` on the hot path.
- **Suspension points.** Query start (residency), segment boundaries, window
  boundaries, and stored-record fetches. Plain `advance()` never suspends.
- **Budget.** The cycle counter is read once per window against a slice of
  100–250 µs. The window size and per-window positional checks are capped so one
  window's worst case is bounded and measured.
- **Scheduling.** Least-attained service: a task's first slice runs at high priority,
  and a task that exhausts its budget is demoted. Each core caps demoted tasks and
  sheds load with an overload error at the cap.
- **Guarantee.** No reactor stall exceeds one slice plus one window, and light-query
  latency stays fair under heavy load. A saturated core does not borrow idle cores:
  this is latency fairness, not throughput isolation. Writes cross to the owner core
  as messages; flush and merge run on background threads or in slices.

## I/O

- **Default: mmap with asynchronous residency.** Zero-copy decode, no double caching.
  At query start the residency layer brings the plan's ranges in with
  `madvise(WILLNEED)` or io_uring on Linux and POSIX AIO on macOS.
- **Pinned per segment:** term index, skip and impact data, lengths, live docs,
  primary-key filter, doc-value columns in active use, and the manifest.
- **Verified block cache** (second backend): `pread` into a cache, checksummed on
  load. It serves indexes much larger than RAM on slow storage, and the strict mode in
  which every read is verified (mmap pages re-read after eviction are not
  re-verified).
- **Metric:** major page faults on reactor threads per 1,000 queries, and p99 latency
  with the index at twice RAM.

## Distribution

A shard search returns a mergeable result: statistics, top-k hits with sort keys,
aggregation partials, and estimate triples. Exact global scoring gathers statistics
from all shards first, then scores with them. The shard RPC carries queries, scored
filters, sort-key functions, and aggregation requests, and it replaces Xapian's
remote protocol.

## Text analysis

- **Default analyzer:** a clean-room reproduction of Xapian `TermGenerator` behavior
  (word rules, case folding, `Z`-prefixed stems, stop strategies, CJK n-grams),
  validated by oracle vectors.
- **UAX #29 analyzer**: available, not the default, because it changes terms the E2E
  suite asserts.
- **ICU**: an optional plugin for `cjk_words` only.
- **Unicode tables** are generated from a pinned UCD version.
- **Snowball**: the 35 algorithms Xapian ships, generated with Snowball 3.0.0, checked
  in with the license and a regeneration script; each language gated on a vocabulary
  compared against Xapian.
- **Query parser**: `queryparser` implements Xapian's `QueryParser` syntax and
  behavior. It is separate from `boolean-parser`, Xapiand's other query language.

## Libraries

Rules:

1. Before a new library or component is written, the
   [Kronuz catalog](https://github.com/Kronuz/cpp-libraries) and every `Kronuz/*`
   C++ repository are checked for the same capability. An existing library is
   extended rather than duplicated.
2. A library that lands or changes updates `Kronuz/cpp-libraries` in the same series.
3. Changes to `cluster`, and to the repositories planned to split out of it
   (`completion`, `journal`, `consensus`, `metrics`, `ledger`), are coordinated with
   the Detent work that owns them today (`~/code/Detent/docs/LIBRARY_BOUNDARIES.md`).

New standalone libraries:

| Library | Contents |
|---|---|
| `task` | `task<T>`, scheduler and I/O concepts, budgets, cancellation, `when_all`; Asio binding as a separate target |
| `residency` | Prefetch, pinning, and the verified block cache on `cluster/completion` |
| `bitpack` | FOR, PFOR, the no-wdf and all-ones flags, fixed 4-lane layout, SIMD with scalar fallback; scalar tails use `varint` |
| `fst` | Sorted-input builder; `get`, `floor`, range streams, automaton intersection |
| `termdict` | Block-tree term dictionary on `fst` |
| `automata` | Levenshtein DFAs, wildcard and prefix automata, intersection with any ordered term source; edit distances come from `string-similarity` |
| `docset` | Iterator concept, dense and sparse sets, live docs, filter-cache bitsets |
| `bkd` | Block KD-tree, 1-D and 2-D interval |
| `fusefilter` | Binary fuse filters (about 9 bits per key, 0.4% false positives) |
| `sortable` | Order-preserving number encoding, MIT; also replaces Xapiand's GPL `sortable_serialise` |
| `unicode` | UCD tables, case and accent folding, UAX #29 |
| `snowball` | Snowball stemmer packaging |
| `analysis` | Xapian-compatible generator, UAX #29 tokenizer, CJK, stoppers |
| `queryparser` | Xapian `QueryParser` syntax and behavior |
| `faultfs` | Power-loss and fault-injection `IO` implementation, extracted from the journal's test model (`journal_test.cc`); segment and manifest I/O go through `completion`'s `IO` interface so one model covers them |
| `benchkit` | Interleaved-trial runner, noise-band rule, JSON output; histograms from `metrics` |

Reused Kronuz libraries:

| Library | Use |
|---|---|
| `completion` (today `cluster/completion`) | Async I/O backends (needs queue depth above one) |
| `consensus`, `journal` (today in `cluster`) | `strata::group`; the consensus peer transport (TLS over TCP, moving from Detent) carries log replication and snapshot transfer |
| `metrics` (today `completion/metrics.h`) | Counters and duration histograms |
| `ledger` (planned extraction) | The node-wide indexing memory budget and storage admission |
| `varint` | Every scalar integer and length-prefixed string in segment, manifest, and RPC formats |
| CRC32C (today `journal/codec.h`) | One checksum implementation, moved into `hashes` with hardware acceleration rather than copied |
| `string-similarity` | Edit distances: the reference in `automata` tests and the ranking of fuzzy candidates. It gains Xapian's distance (adjacent transpositions, over code points) |
| `endian` | Byte order in on-disk formats |
| `compressors` | zstd for stored records |
| `hashes` | Hashing in in-memory term maps |
| `io`, `fs` | File and directory operations |
| `reactor` | The host runtime the Asio binding targets |

`varint` encodes Xapiand's `serialise_length` format. Xapiand's `src/length.{cc,h}`
is MIT by Olly Betts' permission (Xapiand commit `1691e5b2e`), and `varint` is an
independent MIT implementation of the same format.

Kronuz libraries that stay in the host, not the engine: `htm`, `cartesian`, `msgpack`,
`double-metaphone`, `soundex` (the engine exposes scored-filter
and sort-key APIs), and `boolean-parser`. Not used on engine paths: `bloom-filter`
(compile-time size, 20 probes), `queue` (blocking), `scheduler` (own thread),
`allocators` (global operator override), and `lru-cache` on hot paths.
