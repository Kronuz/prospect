# Phase 4 design: structured data

Revision 2, answering the architect's review (`docs/reviews/phase-4-design.md`: R1–R20,
its conditions, and revision check 1's N1–N9). Phase 4 adds what Xapiand's values, sorting, ranges, geo, and
aggregations need, on the Phase 3 index: ordered numeric keys, a slot table, doc values
by slot, points, a geo index chosen by measurement, scored filters, computed sort keys,
collapse, Xapian's estimate triple and percent, aggregations, and delete by query. From
`workload-profiles.md` it takes the key dictionary as its own section, optional
sections per segment, real-time get, and the index-sort reservation. All format work
lands in one version, format 13. Each decision names the review items it answers;
PLAN's Phase 4 section and its ratchets set the gate.

Clean room: Xapian's behavior (estimates, percent, collapse, sort ties, `MatchSpy`
contents, HTM-era geo match sets) enters as a specification from its public
documentation plus `strata-oracle` vectors. No Xapian or Xapiand GPL source is read or
ported, `sortable_serialise` included. S2's cell math, if kept, comes from its published
description; if its Apache-2.0 library is used instead, its notice ships in `NOTICE`.

## Two layers (D1)

strata is two layers with one boundary, and format 13 draws it:

- **The lifecycle core**: the write buffer, immutable segments, manifests and commits,
  merges, live docs, the version map and key resolution (which docid a key names),
  snapshots, durability, the memory budgets, and the deterministic interface (`apply`,
  `checkpoint`, `install`, the digest). It treats a segment as a set of typed sections
  and knows nothing about search.
- **Section plugins**: the `keys` and `versions` sections' encoding, stored records,
  terms and postings, doc values, points, and term lists. Plugins never touch the
  lifecycle.

**The plugin interface.** At flush a plugin receives the segment's documents in output
order and the slot directory. At a merge it receives its input sections, the
permutation, the old-to-new docid map, and each input's live docs, so it can copy what
it need not rewrite (stored frames with no deletes, postings blocks with only their
base rewritten, P17) instead of decoding every section. Either way it returns its section's bytes, its
structure flag, and its digest records, which depend only on live documents' content,
never on segmentation. At open it receives its section's bytes and the segment's
version, and a plugin that reads format N − 1 does so itself; the core never
translates sections.

A segment with only `keys` and `stored` is a sorted key-value store with versions,
snapshots, and atomic batches (the `document` profile, measured against RocksDB in
`workload-profiles.md`); adding sections makes it the search engine. The registry is
resolved at compile time (CRTP), meant to cost nothing per query. That is measured:
the refactor lands after Phase 3's ratchet rows are recorded, and a before-and-after run
of every query class and both indexing rows (five interleaved trials or more) shows no
regression. It lands before Phase 3R's code depends on the index API, so Phase 3R
builds on the final boundary. Whether the core becomes a library of its own is decided
after format 13 freezes.

## Format 13 (D2; R1, R20)

One version for every Phase 4 format change. Format 13 freezes once, after Phase 3's
per-section disk rows are measured and after D15's geo result, since either may change
the format.

**Sections.** Format 12's, plus:

| Section | Holds | Decision |
|---|---|---|
| `keys` | Key blocks (key → ordinal) indexed by `fst`, with the key filter | D12 |
| `values` | The slot directory, presence, columns, dictionaries, statistics | D4 |
| `points` | One tree per point slot, dimensions and leaf size recorded per tree | D5 |
| `term_lists` | Opt-in per field: each document's terms with wdf, positions optional | D11 |

**Reserved now, written later:** the index-sort flag and sort description (D14); a
values generation in the manifest's segment entry, for field-update generations
(`workload-profiles.md` item 4); a continuation flag in the stored section's frame
header, so a record can span frames (R19); the width of persisted exact accumulators.

**Which sections a segment needs:**

- **Mandatory:** header, `docids`, and `stats`.
- **Required together:** `terms`, `term_index`, `postings`, and `lengths`; `norms`
  when Lucene-compatible scoring is configured; `versions` whenever `keys` is present.
- **Free:** `stored`, `values`, `points`, and `term_lists`.

A keyless segment has no `versions`, and `version(ord)` is 0. A conditional operation
(an expected version) whose document has no key rejects in stage 1, in any index; a
docid operation that also names the key, as Xapiand's numeric ids do, stays
conditional. (Today a docid operation with an expected version reaches the version check
with version 0, `index.hh`; the fix lands with Phase 4's operations.) A segment without
`lengths` (the `log` and `document` profiles) has no scored text fields.

**Structure flags.** The footer records the structures the segment was built with, and
at open they must match the table of contents. A free section absent and flagged absent
answers "no such structure". A section the flags name but the table lacks, a mandatory
or required section absent, or an unknown section kind in a known version, is
corruption (today `decode_toc` accepts any kind and the reader skips it).

**Compatibility.** No index exists outside tests and benchmark runs, so format 12 is
rejected and every Phase 3 gate index is rebuilt. Format 13 is the first format with a
compatibility promise: from it on, readers carry N − 1, because Phase 3R's checkpoints
and Track D8's rolling upgrades make indexes outlive a binary.

## The slot table (D3; R2)

strata owns the slot table; the core has no schema code, so this is a table, not a
schema. It lives in its own generation file, `slots-<gen>.kv`, which the manifest
references as it references metadata generations (the manifest's format version changes
with format 13), so a commit rewrites the table only when it changed. It changes only
through a new transaction operation,
`declare_slot`, so a declaration is replicated, ordered with documents, and replayed
like them. Xapiand's dynamic fields declare a slot in the transaction of the first
document that uses it.

A declaration holds:

- the logical type: `i64`, `u64`, `f64`, `date` (D3a), `bytes` (a sorted set), or
  `binary` (an ordered list with duplicates, Xapiand's StringList);
- single- or multi-valued;
- whether the slot has doc values, points, or both, and for points their dimension
  count and bytes per dimension (D5).

Declarations only append. Redeclaring identically is a no-op; declaring a different
type, multiplicity, or structure is a deterministic rejection. A value for an
undeclared slot, a value of the wrong type, and a NaN reject in stage 1, joining Phase
3's rejections before stage 2. Each segment's slot directory copies the declarations it
was written under, so `install`, merges, and frozen indexes decode it without the table.

**Hash collisions.** Xapiand's slots are xxh32 hashes of field names: some two of F
fields collide with probability about 1 − e^(−F²/2^33), 1.2% at 10,000 fields and 69% at
100,000. Under typed slots, a collision between fields of different types is a
deterministic rejection that names both fields. Phase 7's seam work moves Xapiand to
dense slots that the host assigns from the table, which removes collisions and shrinks
every slot directory.

### Ordered keys (D3a; R3)

strata does not need `sortable_serialise`; that was Xapian's need, since its values are
byte strings compared with `memcmp`. Numeric doc values and points are typed `uint64_t`
columns, which need only the fixed-width order-preserving map Lucene and Tantivy use, in
strata's format code:

- `i64`: the sign bit flipped. `u64`: unchanged.
- `f64`: IEEE 754 bits, all bits flipped for negatives and the sign bit flipped for
  positives; `−0.0` maps as `+0.0`; `−∞` and `+∞` order first and last; NaN rejects.
- `date`: `f64` seconds since the epoch, Xapiand's own representation, through the
  `f64` map. Its range covers any year Xapiand's date math produces, past Lucene's
  millisecond longs (about ±292 million years), so the limit rule holds; precision is
  stated per magnitude (about 0.24 µs near the present).
- `bytes`: ordinals into the slot's sorted per-segment dictionary, never compared as
  values at query time.

**Across shards** a sort key is a typed tuple (D8), not one 8-byte value; the 8-byte
big-endian form of this map is one of its component types.

**Xapiand's replacement.** Xapiand uses `sortable_serialise` outside the engine, in its
accuracy terms and in values it builds itself. Phase 8 replaces it with the same 8-byte
big-endian map in Xapiand's own MIT code, written from this section, never from or
against the GPL file. PLAN, FEASIBILITY, and ARCHITECTURE change with this revision.
Migration runs through DUMP and RESTORE: Xapiand, still linked to Xapian, dumps typed
MessagePack documents, and strata indexes those; no `sortable_serialise` bytes are
decoded in strata.

## Doc values (D4; R4)

Per slot and segment:

- **Presence**, an on-disk form with rank, in blocks of 65,536 ordinals: "all present",
  a sorted list of 16-bit lows (up to 4,096 entries), or a bitset with a rank entry
  every 512 bits. A present ordinal's value index is its rank. This lands in `docset`
  as a serialized view (the library has none today), with its own fuzz target and CI.
- **Columns.** Single-valued columns pack one value per present document, from the
  block's minimum, with a GCD where one divides every delta (measured per block; it
  rarely helps mapped doubles). Widths go to 64 bits: `bitpack` gains widths 33–64
  rather than strata growing a second packer. The table encoding (few distinct values)
  is measured on the structured corpora and kept only if it wins the disk row.
- **Multiplicity:** numeric multi-valued slots are sorted multisets (duplicates kept, so
  `sum` over `[5, 5]` is 10); `bytes` slots are sorted sets; `binary` slots keep order
  and duplicates. StringList sort keys read `binary`; aggregations and collapse read the
  declared type's form.
- **Blocks** of 4,096 ordinals record their minimum and maximum. The block size is
  recorded in the section, a preset a measurement can change without a format change.
- **Statistics:** count of documents, count of values, minimum, maximum, and an exact
  sum (128-bit for integers; D11's exact accumulator for `f64`, rounded once). They
  include deleted documents: minimum and maximum stay valid as bounds, while counts and
  sums answer queries only on segments without deletes, as C3 rules for document
  frequencies. Xapian's `value_freq`, read over the remote protocol, is the count of
  present and live ordinals, computed once per live-docs generation and cached. These
  shortcuts are Track P11's.

## Points (D5; R5)

A point slot indexes `uint64_t` values in a BKD tree with up to 8 index dimensions (the
ones the tree splits on) and up to 16 data dimensions (the values stored per point), as
Lucene's points take. Each tree records both counts and the bytes per dimension. Phase 4 builds the dimension counts D15 keeps, and
quantization and its error are stated for the kept geo form.

- Leaves hold up to 512 points with their ordinals, sorted by the split dimension; each
  tree records its leaf size, a preset.
- Inner nodes hold split values, packed in preorder; a range query adds whole leaves
  inside the range without comparing their points.
- 1-D trees serve numeric and date ranges; 2-D interval trees (dominance over (start,
  end)) serve range-valued documents; the geo dimension counts follow D15.
- A points query materializes its matches as a bitset of `doc_count` bits, or a sorted
  array below a density preset, from a per-core pool, so allocations do not grow with
  matches; a multi-valued document counts once. Leaf and node counts answer counts only
  on segments without deletes, and are estimates otherwise.
- The tree is `Kronuz/bkd`, an MIT library (FEASIBILITY sizes it as one), built and
  queried in memory against brute force first, with its own fuzz target and CI.

## Geo (D15; R5, R8)

HTM trixels are not load-bearing; Xapiand's geo API is the requirement. A geo field
holds points, circles, polygons, and convex hulls (`_point`, `_circle`, `_polygon`,
`_chull`), queries use the same shapes, and results sort by distance or rank by the
centroid-angle weight. Four candidates are measured behind one API, each with its
shape-document path:

| Candidate | Point documents | Shape documents | Host computes | Core computes |
|---|---|---|---|---|
| 1. HTM ranges | One cell id, 1-D tree | Covering ranges, 2-D interval tree | Coverings | Range intersection |
| 2. S2 cell ids | One cell id, 1-D tree | Covering ranges, 2-D interval tree | Coverings | Range intersection |
| 3. Latitude and longitude | 2-D tree | Triangles, 7-D tree (`LatLonShape`'s route) | Tessellation | Box and triangle relations |
| 4. Unit vectors | 3-D tree | Triangles of unit vectors (9 data dimensions, ≤ 8 index dimensions) | Tessellation | Box and plane relations |

**Geometry in the core** is one closed numeric primitive, and DISTRIBUTION.md's "no
geometry in the core" is amended to allow it: dot products of stored unit vectors
(three `f64`, or the kept quantized form) with host-supplied unit vectors and plane
normals. A circle is a dot product against a threshold; a convex polygon is the signs of
dot products with its edge planes; the centroid-angle weight and the distance key are
the `acos` of a dot product. The host decomposes non-convex polygons into convex pieces.
A document's centroids live in a declared slot of three `f64` (D3). Candidate 3's
box-against-triangle relations in latitude and longitude are not dot products; if it is
kept, they reduce to the primitive (by testing the triangle's unit-vector form), or
DISTRIBUTION is amended again with the measurement that justified it.

**What the measurement includes,** each row on all four axes, in `docs/perf/`:

1. Coverings: cells per covering and false-positive area at a stated cell budget, for
   every query shape and every shape document under candidates 1 and 2.
2. The exact test: candidates per true match, and its cost per candidate.
3. Match sets equal to an exact spherical oracle in double precision. If strata becomes
   exact where Xapiand's HTM coverings were approximate, that is a documented change,
   checked against the E2E geo rows (`tutorials/spatial-search.md`).
4. Distance sort by stored form: haversine on latitude and longitude, or one dot
   product per document and one `acos` per heap entry on unit vectors.
5. The centroid-angle weight's cost per scored document and its block bound under each
   form; "nearest first" without an explicit sort, as the E2E asserts, is a measured
   query.
6. Geography: `geonames` (global), with circles and polygons across the dateline and
   around both poles, radii from 100 m to a hemisphere. `nyc_taxis` covers one city and
   is not used for geo.
7. A shape corpus: shapes generated to Xapiand's documented types, fixed by seed and
   checksum, and Rally's OSM shapes where available.
8. Dependency cost: S2 implemented from its published description, or the library's
   build and license cost stated. HTM's `cartesian` dependency stays on the host side.

**The keep rule:** the kept candidate meets every Phase 4 ratchet row on `geonames` and
the shape corpus,
and among those, the geo classes' geometric mean decides. Rows 1, 2, 4, and 5 run
offline first; a candidate eliminated there gets no engine code. A fifth arrangement is
measured with them: one 2-D interval tree holding both point documents (as degenerate
intervals) and shape coverings, so a slot needs one tree. H3 leaves Phase 4,
since Xapiand has no geo bucket aggregation.

## Scored filters (D6; R8)

A scored filter is an iterator with `weight()` and `max_weight()`:

- `max_weight()` is an upper bound over the filter's remaining matches under the query's
  statistics, and never increases (a source may lower it while iterating, as Xapian's
  `ValuePostingSource` may).
- An optional `max_weight(from, to)` bounds a block, so block-max and MaxScore prune
  with it; without it, the global bound applies. A constant-weight range filter's block
  bound is its constant, or 0 when the block's [min, max] (D4) misses the range.
- Filters have a versioned wire form, as sort keys do, sized for the scale contract: a
  covering of a large circle is thousands of ranges, sent delta-coded.

## Query planning (D7)

Per segment, a range or geo clause runs as a points query when its estimated match
count times a factor (a measured preset) is below the lead clause's cost, and as a
doc-values check during iteration otherwise (Lucene's `IndexOrDocValuesQuery` rule). A
range clause alone is its own lead and runs on points; a disjunction of ranges runs
each on points and unions the bitsets. Aggregation queries iterate every match but
score lazily, so the aggregation class pays for no scoring it does not use. Xapiand's
accuracy terms for ranges become unnecessary with points; Phase 7 decides whether
Xapiand stops generating them.

## Sort keys (D8; R9)

A sort key is an ordered list of components, each with a slot, a direction, and a
selection rule for multi-valued slots: ascending takes the smallest value, descending
the largest, and distance and string-metric components measure against a reference
value (closest or farthest). Missing values sort last in either direction.
Relevance-then-key and key-then-relevance are components if the seam's inventory shows
Xapiand uses them.

- Keys cross shards as typed tuples compared component by component, bytes carried as
  bytes, never ordinals, so a coordinator merges without knowing the slots.
- Block skipping applies to components monotone in the slot's value; distance and
  string-metric components skip a block only with an interval bound of the function
  over the block's [min, max].
- Ties break by docid ascending; in permuted segments an equal key never skips, as
  Phase 3's R2 rules for scores.
- String metrics come from `Kronuz/string-similarity`.

## Collapse (D9; R10)

`collapse_max` by a slot keeps at most N documents per collapse key, the best ranked.
The key is the slot's complete value list, which is Xapian's semantics (it compares the
slot's stored bytes): in canonical order for `bytes` and numeric slots, and in stored
order for `binary` slots, so [a, b] and [b, a] are different keys there; the seam inventory confirms Xapiand
needs no per-value collapse.

- Per-document collapse counts are exact in exhaustive mode and lower bounds under
  pruning, since a skipped document was never counted.
- The uncollapsed estimate triple is reported with the collapsed one.
- Across shards, each shard collapses to `collapse_max` per key and the coordinator
  collapses again; dropped copies' counts add. The collapsed lower bound is the largest
  shard's; the upper bound is the sum.
- The collapse table is charged to the query budget (D13a).

## Estimates and percent (D10; R11)

Three tiers:

1. Exact, and equal to Xapian's, when `check_at_least` is at least the number of
   matches or strata counted every match. Until `check_at_least` matches are counted,
   no window is skipped as uncompetitive; then pruning resumes.
2. `lower ≤ true ≤ upper`, always.
3. The estimate equals Xapian's only in stated cases where Xapian's does not depend on
   its matcher's algorithm.

Oracle vectors check tiers 1 and 3; a property test checks tier 2 under random pruning.
The gate lists Xapiand's E2E estimate assertions (45 `count`, 13 `total`) with each
one's dataset size and `check_at_least`, and shows each is tier 1 (every aggregation
request there uses `_check_at_least: 1000` on the 1,000-document `bank` index).
`_percent` is computed at the coordinator after the merge, from the global top hit's
weight and matched-term fraction under global statistics; whether a scored filter
counts as a term is settled by oracle vectors. Triples merge across shards by summing
each bound, exact in tier 1; with collapse, D9's rule applies.

## Aggregations (D11; R7, R12)

Collectors over doc values: `count`, `sum`, `avg`, `min`, `max`, `stats`,
`extended_stats`, `variance`, `std_deviation`, `median`, `mode`; buckets `values`,
`terms`, `filter`, `histogram`, `date_histogram`, and `range`; sub-aggregations nested
under buckets; and `_terms` over format 13's `term_lists`.

- **Exact across splits:** integer sums in 128 bits; `f64` sums and sums of squares in
  an exact accumulator (a fixed-point superaccumulator, about 270 bytes dense; with sum
  and sum of squares per bucket, 10^5 histogram buckets take about 54 MB, charged to
  the query budget), rounded once when the result is formed. A square enters as the
  error-free pair (x·x, fma(x, x, −x·x)), since a plain x·x already rounds. `median`
  and `mode` come from exact (value → count) partials.
- **Histogram edges:** integer intervals bucket in the mapped integer domain; a
  fractional `f64` interval buckets by floor((x − offset) / interval), compiled with
  `-ffp-contract=off`, so x86_64 and arm64 agree on every edge. Xapiand
  computes on doubles and warns past 2^53; strata's results differ only there, and the
  documentation says so.
- **Terms partials are complete per shard,** so one shard and many give byte-equal
  responses; their memory is charged to the query budget. Counting runs per segment
  over ordinals and merges by bytes, with no global ordinals.
- **Calendar intervals** (months, years, time zones) are host-computed range
  aggregations; the core has fixed intervals only.
- **No nesting cap:** the request decoder and the evaluation of nested
  sub-aggregations are both iterative, and their state is charged to the query budget,
  so depth is bounded by memory, not by a copied number; a fuzz input nests 10^6 deep.
- Aggregations are exhaustive over the documents that match; Xapian's `MatchSpy` sees
  only those its matcher considers. Every aggregation request in the E2E suite
  considers every match, so the change is not observable there.

## Query memory (D13a; R13)

A query budget, a `byte_budget` separate from indexing's (Phase 3, D17), with a
per-query share, is charged with buckets, collapse tables, sort heaps, points bitsets,
exact accumulators, and decoder state. Exhausting it fails the query, or that shard's
part, with a budget error marked partial, as DISTRIBUTION's deadlines are; indexing
never waits on it. A coordinator budget holds merge state, and partials stream in chunks
under the transport's 256 MiB frame ceiling. There is no bucket count limit: coordinator
memory and the frame ceiling are the mechanisms, recorded in `limits.md`.

## The key dictionary (D12; R6)

`keys` holds key blocks (key → ordinal) indexed by `fst` with `termdict`'s block code,
and the key filter (the binary fuse filter over xxHash64, seed 0, I12) moves into it.
The key term leaves the term dictionary: queries on the key prefix route to `keys`, and
the planner, `verify`, and the digest's records change with them. Versions come from
the `versions` section only. Key range scans merge per-segment iterators, skip deleted
copies, and see one snapshot. The disk row counts keys once.

## Real-time get (D13; R14)

A get, by key or by docid, looks in order at: the version map (deleted keys included);
each of the K active inversion buffers (Phase 3, D18); each frozen buffer whose flush
has not settled; the segments, newest first. A get during a background flush finds a
document that is in neither the buffer nor a published segment. The guarantee per
consistency mode: `linearizable` captures the commit position and waits for apply to
reach it; `session` compares its token with the applied position; `nearest` reads the
replica's applied state; `local` and `none` acknowledge before commit, so a get may not
see the write. D13 lands before Phase 3R's gate, whose linearizability checker covers
gets during flushes and merges.

## Index sorting (D14)

Format 13 reserves the flag and sort description. If Lucene's fastest supported
configuration for a Phase 4 class includes an index sort (see Benchmarks), sorting is
Phase 4 code; otherwise it is Track P.

## Delete by query (D16; R17)

An operation that deletes every live document matching a query, evaluated at apply on
the index state at that position, so replicas agree; the count is in the verdict. The
test-parity inventory assigns it to Phase 4.

- **Buffered documents** are not yet inverted, so apply evaluates the boolean query
  directly against each live buffered document's terms and slot values: linear in the
  buffer, bounded by its share. The count never depends on when a buffer flushes, and
  a test checks that the buffer path and the segment path match the same set after a
  flush, under T2's flush schedules.
- **Plain boolean matching only:** no top-k, cutoffs, pruning, deadlines, or partial
  results; a query that needs any of them rejects in stage 1.
- **Memory:** the match set streams into the live-docs deletions per segment; it and
  the undo entries a later rejection in the same transaction needs are charged to the
  indexing budget, and it never rejects for memory, as a delete by key never does.
- **Check:** digest and verdict equality under every flush schedule in T2.

## Determinism and the write path (D17; R15)

- The digest gains a `V` record (docid, slot, logical type, values in canonical order)
  and a slot-table record; points are built from values, so `V` covers them.
- `document` gains values per slot; `doc_bytes`, the transaction's disk estimate, and
  the flush's sort of points per slot count them, charged to the indexing budget with a
  spill path like the docid sort.
- Merges rebuild columns and trees from live documents, in docid or reordered order;
  1-D trees merge by stream. The values and points write amplification is reported
  beside T3's.
- The log command that carries a transaction gains a version field, designed with Phase
  4's document fields before Phase 3R freezes it.

## Scale (D18; R17)

Stated and measured at the gate: pinned bytes per open segment for slot directories,
block bounds, bytes-slot dictionary indexes, and tree inner nodes, and their total for
an idle index at 1,000 indexes per node; partial-result bytes at 10^4-way fan-out; and
filter and sort-key wire sizes.

## Document size (R19)

Not Phase 4. Chunked transactions are a Track C design coordinated with Detent, and it
answers the review's questions first: checkpoints and truncation inside a transaction,
leader changes, memory at apply, the buffer share as the binding limit, and reading back.
Format 13 reserves the frame continuation flag. The stored-record sizes of Xapiand's
harness datasets are measured first; Phase 7's gate decides the default against
Elasticsearch's 100 MB request bodies.

## Tests

- **Format:** a format-13 fixture with every section, one per profile, one per slot
  type; a reader test that removes each section (free: "no such structure"; mandatory,
  required, or flagged: corruption) and adds an unknown kind (corruption).
- **Oracles:** `strata-oracle` vectors for ranges, sort keys, collapse and its counts,
  estimates per tier, `_percent` with scored filters, `MatchSpy` contents, and HTM-era
  geo match sets; the equivalence oracle's doc-values model (a map per slot) through
  flushes, merges with concurrent deletes, reordering, `install`, and replay; T2's
  schedules with declarations, values, and points.
- **Brute force:** points (1–8 dimensions, and interval intersection), top-k by key
  tuple at k = 1, 10, and 100, and `verify` (pruned equal to exhaustive) with filters,
  sorts, and collapse, over many segments with deletes and reordering.
- **Aggregations:** randomized splits into 1–64 shards and random segmentations, with
  responses compared byte for byte.
- **Budgets:** an aggregation past its share fails while indexing continues at its
  unloaded rate; a 10^4-way merge runs in the simulator.
- **Fuzzing,** one CPU-hour each: `keys`, `values` (presence and rank, columns,
  dictionaries), `points`, `term_lists`, the table of contents, the slot table and its
  operation, and the wire decoders for filters, sort keys, aggregation requests, and
  partial results.
- **Test parity:** Lucene's point, BKD, shape, doc-values, doc-values query, and
  sort-field tests; Tantivy's range, fast-field, and aggregation collectors; delete by
  query.

## Benchmarks (R16)

- **Corpora:** `nyc_taxis`' first 20 million rows (fixed by count and checksum) for the
  numeric, date, sort, and aggregation classes; `geonames` (about 11.4 million points)
  for geo and keyword terms; the Xapiand-shaped corpus for sparse hashed slots; and
  D15's shape corpus.
- **Queries:** per class, a named set adapted from Rally's `nyc_taxis` and `geonames`
  operations (range, distance-amount aggregation, date histogram, ascending and
  descending sort, terms), plus D15's geo set.
- **Configurations,** one per engine for every Phase 4 class: Lucene with points and
  doc values, `IndexOrDocValuesQuery`, `LatLonPoint` and `LatLonShape`, and its facets
  module; Tantivy with fast fields and its aggregation module; Xapian with values,
  `MatchSpy`, and `ValuePostingSource`, plus `LatLongDistancePostingSource` for geo
  (the faster counts). A class an engine cannot express names its reference.
- **All four axes:** indexing on the structured corpus at ≥ 1.0×; disk at ≤ 1.0× whole
  and per section, with new counterpart rows (`values` against `.dvd`, `.dvm`, and
  `.fast`; `points` against `.kdd`, `.kdi`, and `.kdm`; `keys` against the id field's
  terms; term lists reported); serving memory at ≤ 1.0×; allocations that grow with
  distinct buckets and collapse keys, never with matches.

## Gate

- Phase 3's ratchet rows, stated at the commit the gate reviews.
- Filtered, sort, range, geo, and aggregation classes: win against Xapian and reach
  ≤ 1.0× the faster of Lucene and Tantivy, on all four axes.
- Operator parity against oracle vectors; aggregations equal across one shard and many.
- Every gate's rows (sanitizers, fuzzing, coverage, test parity) for the new code, with
  `bitpack`, `docset`, and `bkd` green on every platform in their own CI.
