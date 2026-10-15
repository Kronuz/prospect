# Phase 4 design: structured data

Draft for the architect's review. Phase 4 adds what Xapiand's values, sorting, ranges,
geo, and aggregations need, on the Phase 3 index: ordered numeric keys, doc values
by slot, points, a geo index chosen by measurement, scored filters, computed sort keys, collapse, Xapian's estimate triple
and percent, exhaustive mode, and aggregation collectors. From
`workload-profiles.md` it takes the key dictionary as its own section, optional
sections per segment, and real-time get. All format work lands in one version, format
13. Decisions are numbered D1–D15; PLAN's Phase 4 section and its ratchets set the gate.

Clean room: Xapian's behavior (estimates, percent, collapse, sort key semantics) comes
from its public documentation and from oracle vectors that `strata-oracle` generates
against a pinned Xapian. No Xapian or Xapiand GPL source (`sortable_serialise`
included) is read or ported.

## Format 13 (D1)

One version for every Phase 4 format change, frozen before the code that writes it.
Format 12 segments are rejected with a version error, as format 11 was in Phase 3; a
fixture pins format 13. New sections:

| Section | Holds | Decision |
|---|---|---|
| `keys` | Key → (ordinal, version), FST-indexed key blocks | D12 |
| `values` | Doc values by slot: presence, per-document values, column statistics | D4 |
| `points` | One BKD tree per point slot | D5 |

**Optional sections (D2).** Every section but the header, table of contents, and
footer is optional. A reader treats a missing section as "no such structure": a
segment without `terms` answers no term, one without `stored` returns empty records.
A section present but malformed is still corruption. The gate opens and queries a
segment of every profile in `workload-profiles.md`.

## Ordered keys (D3)

Xapian needed `sortable_serialise` because its values are byte strings compared with
`memcmp`. strata does not keep that requirement: numeric doc values and points are typed
`uint64_t` columns, so what they need is the fixed-width order-preserving map Lucene and
Tantivy use, a few lines in strata's format code, not a library or a variable-length
encoding:

- Signed integers: the sign bit flipped. Unsigned: unchanged.
- Doubles: IEEE 754 bits, all bits flipped for negatives and the sign bit flipped for
  positives; `-0.0` maps as `+0.0`; NaN is rejected.
- Timestamps: signed 64-bit nanoseconds since the epoch, as integers.
- Strings and bytes: ordinals into a sorted per-slot dictionary (D4), so they are never
  compared as values at query time.

Byte-comparable keys remain only where bytes are the interface: a sort key sent between
shards (D8) is the 8-byte big-endian form of the same map. Xapiand's stored values use
GPL `sortable_serialise`; the migration (Phase 8) decodes them once into typed values.

## Doc values by slot (D4)

A slot is a 32-bit id, sparse (Xapiand hashes field names into slots). A document has
zero or more values per slot. A slot's kind is declared in the index's metadata and
fixed for the index's life:

- **Numeric:** values are ordered keys (D3), stored as `uint64_t`.
- **Bytes:** values are byte strings, stored as ordinals into the segment's sorted
  dictionary of that slot's distinct values (a `termdict` per slot).

Per slot and segment:

- **Presence:** a `docset` over ordinals when the slot is sparse, omitted when every
  document has a value.
- **Values:** single-valued columns bit-pack one value per present document, from the
  column's minimum, with a GCD when one divides every delta (Lucene's numeric doc
  values encodings, rebuilt). Multi-valued columns store each document's sorted values
  with an offset column.
- **Statistics:** count of documents, count of values, minimum, maximum, and for
  numeric slots the sum, so `stats` over an unfiltered column reads no values.

Blocks of 4,096 ordinals record their own minimum and maximum, so a range filter skips
blocks, and an aggregation over a filtered set decodes only blocks with matches.

## Points (D5)

A point slot indexes 1-, 2-, or 3-dimensional `uint64_t` keys in a BKD tree, for range and
geo filters that touch many documents:

- Leaves hold up to 512 points with their ordinals, sorted by the split dimension (a
  preset, measured).
- Inner nodes hold split values; the tree is packed in preorder, so a range query
  visits nodes without pointers and adds whole leaves fully inside the range without
  comparing their points.
- 1-D trees serve numeric and date ranges.

A slot may have doc values, points, or both; the query planner (D7) picks per segment.

## Geo (D15)

Xapiand indexes geo as HTM trixel ids because that was what worked when it was built;
it is not load-bearing for strata. Xapiand's geo API (points, circles, polygons, and their
combinations, with distance sorting and the centroid-angle weight) is the requirement,
and the index mechanism is chosen by measurement, with one implementation kept (PLAN's
presets rule):

1. **HTM trixel ranges** in a 1-D tree: the current Xapiand design, the baseline.
2. **S2 cell ids** (Google's cube-face Hilbert curve, 64-bit ids, used by BigQuery,
   MongoDB's `2dsphere`, and CockroachDB) in a 1-D tree: regions cover as cell ranges,
   with tighter coverings and better locality than HTM's triangles.
3. **Latitude and longitude in a 2-D tree**: Lucene's `LatLonPoint` and Elasticsearch's
   `geo_point`, the reference engines' own mechanism; the dateline and poles need care.
4. **Unit vectors in a 3-D tree**: each point as its (x, y, z) on the sphere, so a
   circle is a plane cut of the sphere (a chord-distance box, then an exact test), with
   no dateline, pole, or projection distortion.

Each is built behind the same query API and measured on `geonames` and `nyc_taxis` for
distance, box, and polygon filters and distance sorts, on all four axes; the fastest
becomes the mechanism and the others are deleted. H3 (Uber's hexagons) is a candidate
for geo aggregation buckets only, measured with D11.

## Scored filters (D6)

A scored filter is an iterator with `weight()` and `max_weight()`, so it composes with
block-max pruning like a term. It replaces Xapian's `ValuePostingSource` subclasses
that Xapiand uses:

- Range filters over doc values or points, weighted 0 or by a declared constant.
- Geo filters weighted by Xapiand's centroid-angle rule, served from the geo index (D15),
  with the weight computed from the document's value.

## Query planning for structured clauses (D7)

Per segment, a range or geo clause runs as a points query when its estimated match
count (from the tree's leaf counts) is below the conjunction's lead cost, and as a
doc-values check during iteration otherwise (Lucene's `IndexOrDocValuesQuery` idea).
The cost model is measured, and its thresholds are presets.

## Sort keys (D8)

A sort key is a serializable function over a document's values, so a remote shard
evaluates the same key:

- By slot, ascending or descending; multi-valued slots by minimum, maximum, closest to
  a value, or farthest from it (Xapiand's `MultiValueKeyMaker`).
- By geo distance from a point, by string metrics against a value.
- Ties break by docid, ascending, as Xapian's `Enquire` does.

Top-k by key keeps a bounded heap of keys and skips a block when its minimum or
maximum (D4) cannot enter the heap.

## Collapse (D9)

`collapse_max` by a slot keeps at most N documents per distinct value, the best
ranked, and reports per document the number collapsed into it, as Xapian's collapse
does. The collapse table's memory is charged to the query's budget (D11).

## Estimates, percent, and exhaustive mode (D10)

- The estimate triple (lower bound, estimate, upper bound) and `check_at_least` follow
  Xapian's documented semantics, checked against oracle vectors; Xapiand's E2E suite
  asserts them exactly.
- `_percent` follows Xapian's documented formula, checked against oracle vectors.
- Exhaustive mode counts and aggregates every match, without pruning, on request.

## Aggregations (D11)

Columnar collectors over doc values: terms, histogram, date histogram, range, stats
(count, minimum, maximum, sum, average), and `_terms` over stored term lists.

- Aggregations are exhaustive over the documents that match. Xapian's `MatchSpy` sees
  only the documents the matcher considers, so this is a documented behavior change.
- Every collector's partial result merges associatively, so one shard and many give
  the same answer (DISTRIBUTION.md, "Query"); the gate checks this.
- Bucket memory is charged to the node budget (Phase 3, D17); a query that would
  exceed its share fails with a budget error. There is no fixed bucket count limit
  (Elasticsearch's `search.max_buckets` is a copied number with no mechanism behind it
  here; see `limits.md`).

## The key dictionary (D12)

The primary-key lookup today is the `pk_filter` plus a term lookup, so it depends on the
term dictionary. Format 13 gives keys their own section: key blocks indexed by `fst`
(the same block code as `termdict`), each entry key → ordinal and version, with the
filter in front. A segment without a term dictionary is then valid, and keys support
ordered range scans (the `document` profile).

## Real-time get (D13)

A get by key reads the version map and the writer's buffer before the segments, so it
sees an acknowledged write before refresh, as Elasticsearch's real-time get does.
Buffered stored records are already in an arena; the cost is one lookup.

## Index sorting (D14, proposed)

`workload-profiles.md` places index sorting by a declared doc value (item 6) in Phase 4;
PLAN's Phase 4 lists items 1–3 only. Format 13 reserves the segment flag and sort
description so sorting can land without another format version; the architect decides
whether the code is Phase 4 or Track P.

## Tests

- **Format:** the format 13 fixture; format 12 rejected; a segment of every profile
  (D11) opens and answers.
- **Oracles:** operator parity against `strata-oracle` vectors for ranges, sort keys,
  collapse, estimates, and percent; a doc-values model (a map per slot) under the
  equivalence oracle, through flushes, merges, and deletes.
- **Aggregations:** equal results across one shard and many, at random splits.
- **Fuzzing:** a decoder target for each new section (`keys`, `values`, `points`), one
  CPU-hour each at the gate, and segments of every profile.
- **Lifecycle:** Phase 3's power-loss and digest suites over indexes with values and
  points.

## Benchmarks

The gate's classes (filtered, sort, range, geo, aggregation) need a corpus with
structured fields. The proposal: Elastic's `nyc_taxis` corpus (real trip records:
numbers, dates, pickup and dropoff points), and `geonames` for geo and keyword terms,
both used by Elasticsearch's, Quickwit's, and Tantivy's own benchmarks. Lucene, Tantivy,
and Xapian each get engine code for the classes; Xapian's runs through values,
`ValuePostingSource`, and `MatchSpy`. Ratios follow PLAN: the classes must win against
Xapian and reach ≤ 1.0× of the faster of Lucene and Tantivy, on all four axes.

## Gate

- Phase 3's ratchet rows (they gate Phase 4).
- Filtered, sort, range, geo, and aggregation classes: win against Xapian, ≤ 1.0× the
  faster of Lucene and Tantivy.
- Operator parity against oracle vectors; aggregations equal across one shard and many.
- Every gate's rows (sanitizers, fuzzing, coverage) for the new code.
