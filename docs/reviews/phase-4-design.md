# strata review: Phase 4 design

- Date: 2026-10-15.
- Reviewed: `docs/design/phase-4.md` and `docs/design/limits.md`, both uncommitted in the
  working tree at strata `b3f0e52`. The design was read again after the owner revised it
  during the review: D3 became "Ordered keys", D5 allows three dimensions, and D15
  "Geo" is new. Line numbers below are the revised draft's.
- Checked against:
  - PLAN: Goals 1–5, "Ratchets", "Every gate", Phase 3R's start rule, Phase 4, Phase 8,
    Track P (P1, P11, P20), Track D (D5, D7, D8).
  - ARCHITECTURE (Data model, Segment format, Collectors, Libraries), DISTRIBUTION
    (Scale contract, Layers, Query, Open decision 1), REPLICATION (consistency modes),
    FEASIBILITY (the Xapian surface rows, the effort table, and the clean-room
    procedure), BENCHMARKS (Corpora, Search, Ratchet rows, the counterpart table),
    TESTING and `docs/testing/parity.md`.
  - `docs/design/workload-profiles.md`, `docs/design/phase-3.md`, `docs/design/track-c.md`
    (Limits), and my Phase 3 and Track C design reviews.
  - `include/strata/{format,reader,index}.hh` at `b3f0e52` plus the working tree's
    `format.hh` and `index.hh` changes (`max_term_bytes`).
  - `termdict` `cb00651`, `fst` `f8122e2`, `docset` `8d320d2`, `bitpack` `21456fa`, `htm`
    `2ed0569`.
  - Xapiand at `361a2ebf6`, its MIT parts only: the documentation and its E2E
    assertions (`docs/src/content/docs/`), and `src/multivalue/keymaker.h`. Nothing under
    `src/xapian/`, and not `sortable_serialise`, was opened.
- Checked by reading the files above and grepping. Nothing was built, run, fuzzed, or
  benchmarked: the host is running timed trials on the shared VM.

## Verdict

**APPROVE WITH CONDITIONS.**

The decomposition is right, and most of it should stay:

- One format version for every Phase 4 change, frozen before the code that writes it.
- Typed fixed-width `uint64_t` keys for numeric columns and trees, with Lucene's and
  Tantivy's order-preserving map, and bytes as per-slot dictionary ordinals. D3's
  revision is correct for the core: a variable-length byte encoding and a library for it
  would earn nothing there.
- Columnar doc values by sparse 32-bit slot with per-block bounds, BKD points, and a
  planner choosing between them per segment.
- Geo chosen by measurement among named mechanisms, one kept (D15), rather than HTM
  inherited because Xapiand had it.
- Scored filters that carry `max_weight()`, so they compose with block-max pruning.
- Exhaustive aggregations as one documented behavior change, not a second MatchSpy
  implementation.
- No fixed bucket count; memory pays for buckets.
- The key dictionary out of the term dictionary, optional sections, and real-time get,
  taken from `workload-profiles.md` before the format freezes.

Format 13 is not safe to freeze as drafted. It leaves out structures that PLAN,
ARCHITECTURE, and Xapiand's documented behavior already require, and each one would
cost a format 14:

- Documents whose geo value is a shape, which no point-tree candidate in D15 can index
  (R5).
- Term lists for `_terms` (R7).
- The logical type of a numeric slot (R2).
- Presence with rank (R4).
- A date range that holds Xapiand's dates (R3).

Several claims are wrong as written:

- Aggregations are not exact across splits with floating-point sums (R12).
- The bucket limit has a mechanism, at the coordinator (R13).
- D2's "every section is optional" contradicts what the reader must have (R1).
- D3 removes `sortable` without re-homing what PLAN's Phase 8 needed it for (R3).
- The geo weight and distance key put geometry into a core that DISTRIBUTION says has
  none, and D15's candidates differ in where their geometry runs (R5, R8).

The conditions are the required changes below. Format 13, and every piece of code that
writes or reads it, waits for R1–R7 and a revision check. Code that does not touch the
format can start now, as the last table states.

## Required changes

### Format 13

1. **R1. The scope and rules of format 13** (`phase-4.md:16-32`).
   - **The section list is incomplete.** Besides `keys`, `values`, and `points`,
     format 13 carries:
     - whatever D15's kept geo mechanism needs for shape documents (R5);
     - term lists (R7);
     - the index-sort description and segment flag (D14);
     - a per-segment hook for field-update generations (`workload-profiles.md:81-85`,
       "cheap before Phase 4, expensive after"), for example a values generation in the
       manifest's segment entry, even if no code writes one yet;
     - a continuation flag in the stored section's frame header, so a record can span
       frames later (R19).
     A reserved field costs bytes now. A missing one costs a format 14 and a migration.
   - **Not every section is optional** (`28-32`). Today the reader requires `stats` (28
     bytes), `lengths`, `norms` (one byte per document), `docids`, and `versions`
     (`reader.hh:106-133, 378-383`). The design names three groups of sections:
     - Mandatory in every segment: header, `docids`, and `stats`. A segment without
       docids has no documents.
     - Required together: terms, term index, postings, and lengths; norms when
       Lucene-compatible scoring is configured; `versions` whenever `keys` is present.
     - Free: `stored`, `values`, `points`, and term lists.
   - **What happens to `version(ord)` and lengths when their section is absent**, for
     the `log` profile and for keyless segments.
   - **Unknown section kinds.** `decode_toc` accepts any 16-bit kind (`format.hh:222`).
     The reader ignores one it does not know, and never verifies it (`reader.hh:86-103`,
     `default: break`). Under format 13, an unknown kind in a known version is
     corruption.
   - **A segment records which structures it was built with**: a set of structure
     flags in the header or footer, checked against the table of contents at open. "A
     missing section" then means "never built", not "lost to a writer bug".
   - **The freeze waits for Phase 3's disk rows and for D15's measurement.** The
     per-section disk row after a full merge is still unmeasured
     (`docs/gates/phase-3-ratchets.md`, "Still unmeasured"); a miss there may need a
     format change, and under "one version" it joins format 13. D15 decides whether
     geo needs interval slots, cell-range postings, or neither (R5); that too is format.
     Format 13 freezes after both.
   - **Check:** a format-13 fixture with every section, one per profile, and one per
     slot kind. A reader test removes each section from a valid segment: a free
     section absent answers "no such structure"; a mandatory or required section
     absent, or an unknown kind, fails as corruption.

2. **R2. Slot declarations** (`phase-4.md:55-61`). "Declared in the index's metadata"
   does not work as written. Metadata is the host's key-value store, and the core has
   no schema code (`DISTRIBUTION.md:78`).
   - **A slot table that strata owns**, kept in the manifest or under a reserved
     metadata prefix. It is changed by a new operation type in the transaction, so a
     declaration is replicated, ordered with documents, and replayed like them.
     Xapiand's dynamic fields declare a slot in the same transaction as the first
     document that uses it.
   - **What a declaration holds:**
     - the logical type: `i64`, `u64`, `f64`, the date representation of R3, `bytes`
       (sorted set), or `binary` (an ordered list, ARCHITECTURE's StringList path,
       `ARCHITECTURE.md:54`);
     - single- or multi-valued;
     - whether the slot has points, with dimensions and bytes per dimension (R5).
     "Numeric" (`59`) is not enough: D3's map sends `i64`, `u64`, and `f64` to the same
     `uint64_t` domain, so the column statistics' `sum` (`71-72`), every aggregation,
     and every typed sort key need the logical type to decode a value.
   - **Evolution.** Declarations only append. Redeclaring a slot identically is a
     no-op. Declaring a different kind is a deterministic rejection. A value for an
     undeclared slot, a value of the wrong kind, and a NaN all reject in stage 1, added
     to Phase 3's list of rejections before stage 2 (`phase-3.md:239-242`).
   - **Segments describe themselves.** Each segment's slot directory records the
     declaration it was written under, so `install`, merges, and frozen indexes (P20)
     never read the table to decode it.
   - **Hash collisions.** Xapiand's slots are xxh32 hashes of field names
     (`FEASIBILITY.md:112`). With F fields in one index, the chance that some two
     collide is about 1 − e^(−F²/2^33):
     - 1.2% at 10,000 fields;
     - 69% at 100,000 fields.
     Xapian stores bytes, so a collision between two fields of different types goes
     unnoticed there. With fixed kinds, it becomes a permanent write rejection. The
     design states the behavior: a deterministic rejection that names both fields, or
     slots that the host assigns densely instead of hashing (Phase 7 seam work). Dense
     slots would also shrink every slot directory.
   - **Check:** T2's digest equality, with declarations interleaved with documents and
     replayed; a rejection test for each conflict above; and an open test that decodes
     a segment without the index's table.

3. **R3. Ordered keys, dates, and what replaces `sortable`** (`phase-4.md:34-51`).
   - **The revision stands for the core.** Typed `uint64_t` columns need only the
     fixed-width map (sign flip; IEEE bits flipped by sign), as Lucene's and Tantivy's
     do. No library, no variable-length form. Infinities are allowed and order last and
     first; the design says so beside NaN's rejection (`44`).
   - **Timestamps as signed 64-bit nanoseconds** (`45`) cover only 1677-09-21 to
     2262-04-11. Xapiand stores dates as a double of seconds since the epoch (Xapiand
     `field-types/date-type.md:16`), and its date math takes any year. Lucene's
     millisecond longs cover about ±292 million years. Under the limit rule
     (`limits.md:3-8`), the range is at least the largest of the references: dates are
     `f64` seconds, as Xapiand stores them, or `i64` with a unit declared per slot (R2)
     and the range stated per unit. A nanosecond slot that cannot hold year 1 or year
     9999 is not Xapiand's date type.
   - **The cross-shard sort key is not one 8-byte value** (`49-50`). Bytes slots are
     segment-local ordinals, which mean nothing on another shard; distance and
     string-metric keys are computed doubles; Xapiand's keys are composite (R9). The
     wire key is a typed tuple, as R9 requires; the 8-byte form is one component type
     of it.
   - **PLAN's Phase 8 still needs an MIT replacement.** PLAN removes
     `sortable_serialise` from Xapiand "with `sortable`" (`PLAN.md:498`), and
     ARCHITECTURE says `sortable` "also replaces Xapiand's GPL `sortable_serialise`"
     (`ARCHITECTURE.md:328`). Xapiand's host code uses it outside the engine: in its
     accuracy terms for ranges (`schemas/accuracy.md`) and in values it builds itself.
     With D3, strata no longer provides that replacement. The design names it: the
     same 8-byte big-endian map inside Xapiand's own MIT code, written from this
     design, not from the GPL file. PLAN (`445`, `498`), FEASIBILITY's effort row
     (`FEASIBILITY.md:28`), and ARCHITECTURE (`30`, `328`) change in the same commit.
   - **The migration** (`50-51`) never decodes `sortable_serialise` bytes in strata.
     It runs through DUMP and RESTORE (`FEASIBILITY.md:199`): Xapiand, still linked to
     Xapian, dumps typed MessagePack documents, and strata indexes those.
   - **Check:**
     - round-trip and order tests over every type's extremes: minimum and maximum
       integers, ±0, ±infinity, the smallest subnormals, and years 1 and 9999 in the
       chosen date form;
     - NaN rejected in stage 1.

4. **R4. The doc-values encoding** (`phase-4.md:53-75`).
   - **Presence needs an on-disk form with rank.** `docset` is an in-memory library
     with no serialized form (`docset.hh`; no encode, decode, or view from bytes).
     A sparse column also has to map an ordinal to its value index, which is the rank
     of the ordinal among the present ones. The design specifies:
     - the encoding: per 65,536-ordinal block, a sorted list of 16-bit lows, a bitset
       with a rank directory, or "all present";
     - the rank directory's interval;
     - whether this lands in `docset`, as a library extension, or in strata.
   - **Widths up to 64 bits.** `bitpack` packs widths 0–32 (`bitpack/README.md:13`).
     The span of one day in nanoseconds (8.64 × 10^13) already needs 47 bits, and D3's
     map puts every negative double above 2^63. Under the reuse rule, `bitpack` gains
     33–64-bit widths rather than strata growing a second packer. The design also
     names the table encoding that ARCHITECTURE lists (`ARCHITECTURE.md:130`), or drops
     it. The GCD rule (`67-69`) runs on mapped values, where it rarely helps for
     doubles, so it is a measured choice per block.
   - **Multiplicity per kind:**
     - numeric multi-valued is a sorted multiset: duplicates kept, so `sum` over
       `[5, 5]` is 10;
     - `bytes` is a sorted set;
     - `binary` keeps order and duplicates.
     The design states which one each Xapiand feature reads: StringList keys,
     aggregations, and collapse (R10).
   - **Block bounds as a recorded preset.** The 4,096-ordinal block (`74-75`) is
     recorded in the section, not fixed by the format, so a measurement can change it
     without a format change.
   - **Statistics under deletes.** A segment's count, value count, and sum include its
     deleted documents.
     - The minimum and maximum stay valid as bounds.
     - The counts and the sum answer queries only for segments without deletes, as C3
       rules for document frequencies (`phase-3.md:279-280`).
     - "`stats` over an unfiltered column reads no values" (`72`) holds only then.
     - Xapian's `value_freq`, which Xapiand reads over the remote protocol
       (`FEASIBILITY.md:112`), is the count of present ∧ live ordinals. It is computed
       once per live-docs generation and cached.
   - **Exact sums.** Integer sums are 128-bit, since an `i64` sum overflows past 2^63.
     Double sums are exact accumulations, rounded once at the end (R12). The section
     stores the representation it uses.
   - **Check:** the doc-values model oracle (`phase-4.md:198-199`) with deletes at
     every point, including tests that the statistics shortcut is refused on segments
     with deletes; and round-trips at widths 33–64.

5. **R5. Points and geo** (`phase-4.md:77-112`).
   - **Dimensions are a format field, not a cap of 3** (`79`). Three is what D15's
     unit-vector candidate needs, not a mechanism's limit. Lucene's points take up to 8
     index dimensions, and its shape index uses 7 (triangles). The tree records its
     dimension count in one byte, decoders accept up to 8, and Phase 4 builds what D15
     keeps. The design also states bytes per dimension: latitude and longitude at 8
     bytes each are twice Lucene's `LatLonPoint` (4 bytes each), against a ≤ 1.0×
     points disk row (R16). The quantization and its error are stated for whichever
     candidate is kept.
   - **Leaf size** (`82-83`) is now called a preset; each tree also records its own
     leaf size, so changing the preset needs no format change.
   - **Materializing results.** A points query collects matching ordinals before
     iteration. The design states the structure: a bitset of `doc_count` bits, or a
     sorted array below a density, from a per-core pool, so Phase 3's property row
     holds and allocations do not grow with the number of matches (`PLAN.md:89`).
     Multi-valued documents are counted once. Counts from leaf and node counts answer
     only on segments without deletes; elsewhere they are estimates (D7).
   - **Location.** FEASIBILITY sizes `bkd` as its own library (`FEASIBILITY.md:31`). The
     design names it (`Kronuz/bkd`, MIT, with its own fuzz target) or keeps it in strata
     with a reason.
   - **D15: the scope is right, and incomplete on the document side.** Four candidates
     measured behind one API, one kept, the rest deleted, is the presets rule applied
     correctly. HTM as a baseline rather than a given is right. What the draft misses:
     - **Xapiand documents hold shapes, not only points.** A geo field accepts
       `_point`, `_circle`, `_polygon`, and `_chull` values (Xapiand
       `field-types/geospatial-type.md:20-98`). "Points, circles, polygons, and their
       combinations" (`94-95`) reads as the query side only. Candidates 3 and 4 index
       points; a shape value does not fit in a point tree. Candidates 1 and 2 cover a
       shape as cell ranges, which is an interval per range: a document range [a, b]
       meets a query range [c, d] when a ≤ d and b ≥ c, a 2-D dominance query over
       (start, end). That is the "2-D interval" tree PLAN names (`PLAN.md:445`) and
       ARCHITECTURE keeps (`ARCHITECTURE.md:56, 131`), and D5 dropped it.
     - So each candidate is measured with its shape-document path, or the kept
       mechanism is split in two, which breaks "one implementation":
       - HTM and S2: point documents as single cells, shape documents as covering
         ranges in an interval tree (2-D), or as cell terms at several levels.
       - Latitude and longitude, and unit vectors: shape documents need a
         tessellation into triangles, Lucene's `LatLonShape` route, at 7 dimensions in
         2-D, more for 3-D. That exceeds D5's cap of 3 and is a second mechanism unless
         it is the only one.
       Alternatively the design states that shape documents are out of Phase 4, with
       the E2E rows that store shapes listed and PLAN changed.
     - **Where the geometry runs.** `DISTRIBUTION.md:78` keeps geometry out of the
       core. For 1 and 2, the host computes coverings and the core indexes integer
       ranges, which fits. For 3 and 4, the box query is the core's, but the exact test
       that follows (point in circle, in polygon) is geometry. The design states, per
       candidate, which side runs each step, and amends DISTRIBUTION if the exact test
       enters the core (R8).
     - **H3 is not load-bearing.** Xapiand's documented aggregations include no geo
       bucket aggregation (`search/aggregations/`). H3 leaves Phase 4 until a
       requirement names it.
   - **What D15's measurement must include**, each row on all four axes:
     1. **Coverings.** For every query shape (circle, convex polygon, convex hull,
        box), and for every shape document under 1 and 2: cells per covering, and the
        false-positive area at a stated cell budget. HTM's at Xapiand's default
        accuracy levels; S2's at a stated `max_cells`.
     2. **The exact test.** Candidates per true match, and the cost of the exact
        test per candidate, so a loose covering cannot look fast by skipping it.
     3. **Match sets.** Every candidate's match set equals an exact spherical oracle,
        computed in double precision from the documents' coordinates. Xapiand's HTM
        coverings may be approximate at its accuracy level; if strata becomes exact,
        that is a documented behavior change checked against the E2E geo rows
        (`tutorials/spatial-search.md:132, 215`, and the 328 km North Dakota circle).
     4. **Distance sort.** Its cost depends on the stored coordinate form, not the
        tree: haversine on latitude and longitude needs trigonometry per document; unit
        vectors need one dot product, then one `acos` per heap entry. The stored form is
        measured with the index form, since one form is kept for both.
     5. **The centroid-angle weight.** Its cost per scored document under each stored
        form, and its block bound for block-max (R8): 2-D and 3-D trees give a cell's
        box, which bounds the angle; 1-D cell ranges give a coarser bound. "Nearest
        first" top-k without an explicit sort, as the E2E asserts, is a measured query.
     6. **Geography.** `nyc_taxis`' points all lie inside one city, which flatters a
        latitude-longitude tree. `geonames` is global. The query set includes circles
        and polygons across the dateline and around both poles, and radii from 100 m to
        a hemisphere.
     7. **Corpora with shapes.** Both named corpora hold points only. A shape corpus
        (for example Rally's OSM `geoshape` track, or shapes generated to Xapiand's
        documented types, fixed by seed and checksum) is added for shape documents.
     8. **Dependency cost.** S2's reference implementation is Apache-2.0 C++ with a
        large dependency tree. Either strata implements the cell-id and covering math
        from the published description, with its own tests, or the dependency's
        build and license cost is stated. HTM is already an MIT Kronuz library, but
        pulls `cartesian`, whose in-place operators are known-buggy (Xapiand
        `AGENTS.md`, Traps); it stays on the host side.
   - **The keep rule.** "The fastest becomes the mechanism" (`110-111`) names one axis.
     The kept mechanism meets every Phase 4 ratchet row on both corpora, and among
     those, the geo classes' geometric mean decides. A candidate that wins time and
     breaks the disk row is not kept.
   - **Staging.** Rows 1, 2, 4, and 5 run as offline measurements before engine code,
     and a candidate eliminated there is eliminated by its numbers. The survivors get
     engine code. This cuts four engine integrations to two without skipping a
     measurement.
   - **Check:** points queries against brute force for 1-, 2-, and 3-D trees and for
     interval intersection, over many segments with deletes and reordering; the D15
     table in `docs/perf/` with every row above; and the points disk row.

6. **R6. The key dictionary** (`phase-4.md:172-178`).
   - **Is the key term still in the term dictionary?** Phase 3 makes the key the
     host's own unique term with prefix `Q` (`phase-3.md:221-223`). Today a lookup
     reaches it through the term dictionary (`index.hh:913-927`). With a `keys`
     section, there are two choices:
     - Keep the term in both places. Disk pays twice, and BENCHMARKS' counterpart for
       keys changes.
     - Drop it from the term dictionary. Queries on `Q` terms are then routed to
       `keys`, and the planner, `verify`, and the digest's `P` records change with
       them.
     The design picks one and states its disk effect.
   - **One source for versions.** Each entry holds key → ordinal and version (`176`),
     while `versions` already holds one version per ordinal (`format.hh:93`). The
     design keeps one: the entry holds the ordinal, and the version comes from
     `versions`.
   - **`pk_filter`.** The design states whether it stays its own section or moves into
     `keys`, and keeps its hash (`phase-3.md:74-75`, I12).
   - **Range scans.** Key range scans across segments merge per-segment iterators and
     skip deleted copies. The scan's order and consistency are a snapshot's.
   - **Check:** key lookup time at 1 and 50 segments no worse than Phase 3's T4
     key-filter row, and the disk row with keys configured alike.

7. **R7. Term lists.** D11's `_terms` aggregation reads "stored term lists"
   (`phase-4.md:161`), and nothing in format 12 or format 13 stores them. ARCHITECTURE
   has them as an opt-in section (`ARCHITECTURE.md:58, 133`). BENCHMARKS places them in
   Phase 4 (`BENCHMARKS.md:175`). Xapiand's info endpoint carries about 40 E2E
   assertions on them (`FEASIBILITY.md:111`), and Phase 5's expansion reads them. One
   of two:
   - Format 13 includes the section: per field, per document, terms with wdf and,
     optionally, positions.
   - D11 drops `_terms` until a later format, and PLAN's Phase 4 changes with the
     reason.

### Query semantics

8. **R8. Scored filters** (`phase-4.md:114-122`).
   - **The bound contract.**
     - `max_weight()` is an upper bound over the filter's remaining matches under the
       query's statistics, and never increases. Xapian's documented
       `ValuePostingSource` lets a source lower its maximum as it iterates.
     - An optional `max_weight(range)` bounds a window or block, so block-max and
       MaxScore prune with filters tighter than the global maximum. Without it, the
       global maximum applies.
     - A constant-weight range filter's block bound is the constant, or 0 when the
       block's [min, max] (D4) misses the range.
   - **A wire form.** Xapiand sends `ValuePostingSource`s to remote shards
     (`FEASIBILITY.md:120, 130`). D8 makes sort keys serializable; D6 does not say
     the same of filters. Filters get the same versioned form, sized at the scale
     contract: an HTM or S2 covering of a large circle is thousands of ranges.
   - **Geometry in the core.** The centroid-angle weight (`121-122`) and the geo
     distance key (D8, `138`) are geometry, and `DISTRIBUTION.md:78` says the core has
     none. There are two consistent ways:
     - Name a closed numeric primitive, the angle between two unit vectors stored as
       three `f64` values (or D15's kept stored form), with its formula in the design,
       and amend DISTRIBUTION to allow that primitive.
     - Make weights and keys functions that the host registers by name, versioned, so
       every node in a rolling upgrade (Track D8) evaluates the same one.
     The design also states which slot holds a document's centroids, since the weight
     reads them, and which holds its index form (D15).
   - **Check:**
     - `verify` (pruned equal to exhaustive) with scored filters at k = 1, 10, and 100,
       both scoring schemes, many segments, deletes;
     - oracle vectors for the geo ranking that Xapiand's E2E asserts
       (`tutorials/spatial-search.md:132, 215`: nearest first, without an explicit
       sort);
     - fuzzing of the filter wire decoder.

9. **R9. Sort keys** (`phase-4.md:131-142`).
   - **Composite keys.** Xapiand's sort key spans several slots, each with its own
     direction (Xapiand `src/multivalue/keymaker.h:359-366`, MIT):
     - ascending takes a document's smallest value;
     - descending takes its largest;
     - string-metric and geo components compute a distance to a reference value.
     D8 conflates multi-valued slots with multiple slots. It defines:
     - a key as an ordered list of components, each with a slot, a direction, and a
       selection rule (minimum, maximum, closest, or farthest);
     - the position of missing values;
     - the mixed relevance-and-key modes, if the seam's inventory shows Xapiand uses
       Xapian's documented `set_sort_by_relevance_then_key` or
       `set_sort_by_key_then_relevance`.
   - **Keys merge across shards as typed tuples**, compared component by component,
     with bytes components carried as bytes, never as segment ordinals (R3). A
     coordinator then merges without knowing the slots.
   - **Block skipping** (`141-142`) applies only to components that are monotone in
     the slot's value. Distance and string-metric components skip a block only with an
     interval bound for the function over [min, max], or never.
   - **Ties.** Ties break by docid ascending, and in permuted segments (P1, D14) an
     equal key never skips, exactly as Phase 3's R2 rules for scores
     (`phase-3.md:276-278`).
   - **String metrics** come from the existing `string-similarity` library, not a new
     copy.
   - **Check:**
     - top-k by key equal to a brute-force sort by (key tuple, docid) at k = 1, 10, and
       100, on multi-segment indexes with deletes and with a reordered merge;
     - oracle vectors for each component type.

10. **R10. Collapse** (`phase-4.md:144-148`).
    - **The key's identity on multi-valued slots.** In Xapian, collapse compares a
      slot's stored bytes, so a multi-valued field collapses by the whole list.
      Xapiand's own comment says collapse uses a `MultiValueKeyMaker`
      (`keymaker.h:363-364`), which is a composite key. The design states which of
      these Xapiand needs, from the seam's inventory, and which value of a strata
      `bytes` set a document collapses on: the whole set, or each value.
    - **Pruning.** The k-th score of the collapsed list is a valid threshold, since
      collapse only removes documents. But a document skipped below that threshold was
      never counted into a kept one, so the per-document collapse counts are lower
      bounds unless the query is exhaustive. The design says so, and checks it against
      Xapian's documented meaning of `get_collapse_count`.
    - **The uncollapsed triple.** Xapian reports one (`get_uncollapsed_matches_*`).
      D10 includes it.
    - **Across shards.** Each shard collapses locally to at most `collapse_max` per
      key, and the coordinator collapses again; the counts of a key's dropped copies
      add up. Collapsed-match bounds do not add across shards:
      - the lower bound is at least each shard's own;
      - the upper bound is at most their sum.
      The design gives the merge rule and its exactness.
    - **Memory.** The collapse table is charged to R13's query budget.

11. **R11. Estimates and `_percent`** (`phase-4.md:150-156`).
    - **Split the contract.** Xapian's estimate depends on its own matcher's choices.
      Block-max MaxScore skips different documents, so "follow Xapian's documented
      semantics, checked against oracle vectors" cannot hold everywhere. The design
      states three tiers:
      1. All three values are exact, and equal Xapian's, when `check_at_least` is at
         least the number of matches, or when strata counted every match.
      2. `lower ≤ true ≤ upper` always holds.
      3. The estimate equals Xapian's only in the stated cases where Xapian's estimate
         does not depend on its algorithm.
      The oracle checks tiers 1 and 3; a property test checks tier 2 under random
      pruning.
    - **Enumerate the E2E assertions.** Xapiand's E2E asserts `count` 45 times and
      `total` 13 times. The design lists each one's dataset size and `check_at_least`,
      and shows it falls in tier 1. Every one of the 29 aggregation requests uses
      `"_check_at_least": 1000` on the 1,000-document `bank` index.
    - **`check_at_least` and block-max.** Until `check_at_least` matches are counted,
      no window is skipped for being uncompetitive; after that, pruning resumes. The
      design states this.
    - **`_percent` across shards.** Xapian's formula uses the top document's weight
      and the fraction of query terms it matched. In a distributed query, that is the
      global top hit's. Each shard sends its top hit's matched-term count, and the
      coordinator computes percent after the merge, under global statistics
      (DISTRIBUTION's exact mode). The design also states whether a scored filter
      counts as a term in that fraction, from oracle vectors.
    - **Merging the triple.** The design gives the merge rule across shards (sums of
      each bound without collapse; R10's rule with it) and says it is exact in tier 1.

12. **R12. Aggregations** (`phase-4.md:158-170`).
    - **The collector set.** Xapiand documents these aggregations:
      - metrics: `count`, `sum`, `avg`, `min`, `max`, `stats`, `extended_stats`,
        `variance`, `std_deviation`, `median`, `mode`;
      - buckets: `values`, `terms`, `filter`, `histogram`, `range`;
      - sub-aggregations nested under buckets "with no hard limit" on depth (Xapiand
        `search/aggregations.md:40`).
      D11 lists terms, histogram, date histogram, range, and stats. Each missing
      collector is added, or excluded with its count of E2E assertions. `median` alone
      asserts an exact value (`2414.425`, `median-aggregation.md`).
    - **Exactness across splits.** "One shard and many give the same answer" fails
      with floating-point sums, because addition of doubles is not associative.
      Required:
      - integer sums in 128 bits;
      - double sums, and the sums of squares that variance needs, in an exact
        accumulator rounded once when the result is formed;
      - `median` and `mode` from exact (value → count) partials, charged to memory;
      - histogram buckets computed in the mapped integer domain, or in floating point
        built with `-ffp-contract=off`, so x86_64 and arm64 shards agree on bucket
        edges.
      Xapiand computes on doubles and warns about values past 2^53
      (`aggregations.md:46-48`). strata's exact results differ from Xapiand's only
      there, and the design states it.
    - **Terms partials.** Either complete per shard (exact; memory charged), or
      truncated with an error bound, as Elasticsearch's `doc_count_error_upper_bound`
      does. The gate's "equal across one shard and many" means equal responses, bit
      for bit, and only the complete form can meet it.
    - **Ordinals across segments.** Counting runs per segment over ordinals and merges
      by bytes, with no global ordinals. If global ordinals are built, their memory is
      charged, and their build time per refresh is measured.
    - **The MatchSpy change** (`163-164`) is safe for the E2E suite, since every
      aggregation request there considers every match. The design records that, so the
      change is shown, not asserted.
    - **Nesting.** The aggregation request decoder has a depth limit (R18), since
      recursion without a cap is a known trap (Xapiand `AGENTS.md`, Traps).
    - **Check:** randomized splits into 1–64 shards and random segmentations, with
      responses compared byte for byte; and oracle or model checks for each collector.

13. **R13. Query memory and the bucket claim** (`phase-4.md:148, 167-170`).
    - **D17's budget is the wrong one.** D9 charges "the query's budget (D11)", and
      D11 charges "the node budget (Phase 3, D17)". D17 is the indexing budget: at its
      limit, stage 1 suspends (`phase-3.md:304-306`). An aggregation charged there
      stalls writes on the node.
    - **Required: a query memory budget.** It is a separate `byte_budget` instance,
      with a per-query share. It is charged with:
      - buckets, collapse tables, and sort heaps;
      - points bitsets (R5) and exact accumulators;
      - the coordinator's merge state.
      Exhausting it fails the query, or that shard's part of it, with a budget error,
      marked partial as DISTRIBUTION's deadlines are. Indexing never waits on it.
    - **`search.max_buckets` does have a mechanism.** Elasticsearch's limit protects
      the coordinating node's heap and the response, and strata has both:
      - coordinators merge partials from up to 10^4 shards (`DISTRIBUTION.md:18`);
      - every partial crosses a transport whose frame ceiling is 256 MiB
        (`track-c.md`, Limits, "RPC 256 MiB").
      With B buckets per shard, the coordinator holds about 10^4 × B entries before
      the tree merge reduces them. The design charges that state to a coordinator
      budget, and streams partials in chunks under the frame ceiling or bounds them by
      it. limits.md records the result: no count limit, and the bytes bounded by these
      named mechanisms.
    - **Check:**
      - an aggregation that exceeds its share fails while concurrent indexing
        continues at its unloaded rate;
      - a 10^4-way merge in the simulator at the per-node memory stated.

14. **R14. Real-time get** (`phase-4.md:180-184`).
    - **Where a get looks**, in order:
      1. the version map, deleted keys included;
      2. each of the K active inversion buffers (D18);
      3. each frozen buffer whose flush has not settled;
      4. the segments, newest first.
      A get during a background flush must find a document that is in neither the
      buffer nor a published segment. Gets by docid take the same path through the
      buffer's docid index (`index.hh:903-909`). Xapiand gets by docid as much as by key.
    - **The guarantee per mode.** "Sees an acknowledged write" holds only where
      acknowledgement follows apply.
      - `local` and `none` acknowledge before commit (`REPLICATION.md:120-125`), so the
        leader may not have applied the write yet.
      - `linearizable` captures the commit position, waits until apply reaches it, then
        reads.
      - `session` compares the token against the applied position, not the visible
        one.
      - `nearest` reads the replica's applied state.
      The design states each.
    - **Order with Phase 3R.** Phase 3R's gate checks linearizability over get-by-id
      histories (`PLAN.md:435`, `workload-profiles.md:7`), and REPLICATION already
      serves get-by-id from the version map (`REPLICATION.md:134`). Either D13's code
      lands with Phase 3R, or Phase 3R tests the Phase 3 path and D13 keeps that path's
      semantics unchanged.
    - **Check:** the linearizability checker over histories in which gets run during
      background flushes and merges, in every consistency mode.

### Determinism, lifecycle, and the gate

15. **R15. Values in the deterministic core.**
    - **The digest** (`phase-3.md:389-405`) has no record for values. Two replicas
      that disagree on a value pass T2. It gains a `V` record: docid, slot, logical
      type, and the values in canonical order. Points are built from values, so the
      `V` records cover them, and the design states that. The slot table (R2) gets a
      record of its own.
    - **The write path.** `document` (`index.hh:42-46`) gains values per slot. Three
      places count them:
      - `doc_bytes` (`index.hh:711`);
      - the transaction's disk estimate (`index.hh:889-899`);
      - the flush's sort of points per slot, charged to D17 with a spill path, as the
        docid sort is (`phase-3.md:348`).
    - **Merges.** A merge rebuilds columns and trees from live documents only, in
      docid order or in the reordered order (P1, D14). 1-D trees merge by stream
      rather than by re-sorting. The design states the merge's write amplification for
      values and points beside T3's.
    - **The command encoding.** The command that carries a transaction in the log gains
      a version field, and is designed with Phase 4's document fields before Phase 3R
      freezes it. Otherwise a journal written before format 13 cannot be replayed after
      it.
    - **Check:** T2's schedules with values, declarations, and points, and the
      equivalence oracle's doc-values model (`phase-4.md:198-199`) over flushes,
      merges with concurrent deletes, reordering, `install`, and replay.

16. **R16. Benchmarks and the four axes** (`phase-4.md:206-222`).
    - **The corpus.** `nyc_taxis` is about 165 million documents (Rally). Five
      interleaved trials of four engines on an 8-vCPU, 16 GB VM is not a gate run. The
      design fixes:
      - a subset by document count and checksum (for example the first 20 million
        rows);
      - `geonames` (about 11.4 million points);
      - the Xapiand-shaped corpus (`BENCHMARKS.md:68-69`) for sparse hashed slots,
        which `nyc_taxis`' few dense fields never exercise;
      - D15's shape corpus (R5).
      The claim that Tantivy's and Quickwit's own benchmarks use `nyc_taxis` (`211`) is
      cited or removed.
    - **The queries.** Each class names its query set, adapted from Rally's
      `nyc_taxis` and `geonames` operations: range, distance-amount aggregation, date
      histogram, ascending and descending sort, and terms, among others; and for geo,
      R5's set.
    - **Engine configurations.** One index configuration per engine for all Phase 4
      classes, stated:
      - Lucene: points with doc values, `IndexOrDocValuesQuery`, `LatLonPoint` and
        `LatLonShape`, and its facets module for aggregations;
      - Tantivy: fast fields and its aggregation module;
      - Xapian: values, `MatchSpy`, and `ValuePostingSource`, plus its documented
        `LatLongDistancePostingSource` as an alternative for geo, the faster of the two
        counting.
      If Lucene's fastest supported configuration includes an index sort
      (`PLAN.md:12-14`), then D14 is Phase 4 code, not Track P. A class one engine
      cannot express (Tantivy has no geo query, if that holds at 0.26) names its
      reference engine.
    - **All four axes**, as PLAN's ratchets carry them ("one that wins an axis may not
      push another past its ratchet"):
      - Indexing on the structured corpus at ≥ 1.0×.
      - Disk at ≤ 1.0× whole, and per section against new rows in the counterpart
        table (`BENCHMARKS.md:166-175`): `values` against `.dvd` and `.dvm` (and
        `.fast`); `points` against `.kdd`, `.kdi`, and `.kdm`; `keys` against the id
        field's terms; term lists excluded and reported.
      - Serving memory at ≤ 1.0×.
      - The allocation property row extended: allocations grow with distinct buckets
        and collapse keys, never with matches.
    - **`verify`** gains the new classes: pruned equal to exhaustive with filters,
      sorts, and collapse.

17. **R17. "Every gate" rows** (`PLAN.md:105-116`).
    - **Fuzzing.** One CPU-hour for each decoder and parser the phase adds, not only
      the three sections (`phase-4.md:201-202`):
      - `keys`, `values` (presence and rank, columns, dictionaries), `points`
        (including whatever D15 keeps for shapes), and term lists;
      - the slot table and its operation;
      - the wire decoders for scored filters, sort keys, aggregation requests, and
        partial results;
      - format 13's table of contents.
    - **Fixtures**, as R1 lists.
    - **Coverage** at TESTING's targets.
    - **Test parity.** The inventory's Phase 4 rows:
      - Lucene's point, BKD, shape, doc-values format, doc-values query, and
        sort-field tests;
      - Tantivy's range, fast-field, and aggregation collectors (`parity.md:77`);
      - deletes by query, which `parity.md:54` assigns to Phase 4 and the design does
        not mention. The design adds them or reassigns them.
    - **A scale statement per component.** The draft has no scale section, where Phase
      3 had R6. It states:
      - pinned bytes per open segment for slot directories, block bounds, bytes-slot
        dictionary indexes, and tree inner nodes, and their total for an idle index at
        1,000 indexes per node;
      - partial-result bytes at 10^4-way fan-out (R13);
      - filter and sort-key wire sizes (R8, R9).

18. **R18. limits.md for Phase 4, and its existing rows.**
    - **New rows**, each naming its mechanism and the references:
      - the slot id range: 32-bit; Xapian reserves `BAD_VALUENO`, so 0 to 2^32 − 2
        unless stated otherwise;
      - values per document per slot;
      - the length of a `bytes` or `binary` value, bounded today by the command (below);
      - point dimensions (R5: a format field up to 8, not a cap of 3) and bytes per
        dimension;
      - the date range (R3);
      - leaf size and block size as presets (R4, R5);
      - buckets and collapse keys (R13);
      - `collapse_max`;
      - sort components per key;
      - nesting depth of request decoders (R12);
      - cells or ranges per geo covering (R5, R8);
      - the 64-section cap in the table of contents (`format.hh:217`), whose mechanism
        is bounding the decoder's allocation.
    - **The document limit's default is 1 MiB, not 64 MiB.** `limits.md:26-29` says
      the largest replicated document is "the smaller of the command ceiling (64 MiB)
      and the stored-record limit (64 MiB)". But a group's default command limit is 1
      MiB (`limits.md:21`), and 64 MiB is only the ceiling that node limits may be
      raised to (`track-c.md`, Limits). Out of the box, a replicated transaction (a
      document plus framing, or a bulk batch) is under 1 MiB. The row says so.
    - **The term row** states its effective bound under groups: 2^32 − 1 holds
      standalone, but any replicated term is under the command limit.
    - **Lucene's row** cites the binding limit. The per-thread RAM hard limit
      (`IndexWriterConfig`'s `RAMPerThreadHardLimitMB`, 1,945 MB by default) fails a
      document whose inversion exceeds it, before Java's array limits matter.

### The document-size item (`limits.md:24-37`)

19. **R19. Ruling: not Phase 4, except one reserved flag.**
    - **It does not belong in Phase 4.** The chunked transaction is a change to the
      consensus command path, which Track C owns and Detent shares (R16 and the
      limits-by-position rules in `track-c.md`). The stored-record frame run is a
      storage format change. Only the second touches format 13, and only as a reserved
      continuation flag in the frame header (R1).
    - **What the proposal must answer before it is designed:**
      - **Checkpoints and log truncation.** A checkpoint or truncation cannot fall
        inside an open chunked transaction, or the checkpoint carries its chunks, and
        `install` then carries them too. The applied position's pair (log index,
        transactions applied) needs a third state: "inside transaction T, chunk i".
      - **Leadership changes.** A new leader that holds only some chunks ends the
        transaction with a deterministic abort entry, so every replica reaches the same
        verdict.
      - **Memory at apply.** Chunks are read back from the journal at the last chunk,
        never held in memory. The retained log (16 GiB per group, `track-c.md`) bounds
        them.
      - **The real binding limit.** A single document cannot split across segments.
        Phase 3 lets a transaction flush part of itself (`phase-3.md:243-245`), not one
        document. So a document's inversion must fit the index's buffer share. At
        1,000 indexes per node, that share is far below gigabytes, so the document
        limit is min(chunks × command bytes, buffer share, stored-record run). The
        proposal states which binds, and at what node budget.
      - **Reading back.** A record spread over many frames still reaches the caller
        whole. Either the read API returns the record frame by frame, or it allocates
        the validated total. C9's bound per frame stays either way.
    - **What decides the target value.** Xapiand already keeps large content in its
      Haystack-style volumes, and DISTRIBUTION's open decision 1 (Track D7) decides
      where they live. What reaches the stored record is the MessagePack document.
      Before choosing, measure the size distribution (p50, p99, maximum) of what
      Xapiand's harness datasets put in the stored record.
    - **The deadline is Phase 7, not Phase 4.** At the system level, Elasticsearch
      accepts request bodies up to 100 MB by default (`http.max_content_length`), and
      Phase 7's gate compares against it. So by Phase 7's gate, strata either accepts
      a document of at least that size in its default configuration, through chunking
      or a raised default with its measured decoder cost, or the gap is recorded in
      limits.md as an exception with that cost. Until then, limits.md keeps 64 MiB as
      "open, scheduled", with this ruling.

20. **R20. The migration rule and the text.**
    - **Format 12 → 13.** Rejecting format 12 (`phase-4.md:18-19`) is right while no
      index exists outside tests and benchmark runs. The design states that premise,
      and that every Phase 3 gate index is rebuilt. From the first index that must
      survive an upgrade, which Phase 3R's checkpoints and Track D8's rolling upgrades
      make close, readers carry N − 1. The design states whether format 13 is that
      first promised format, and decides it before Phase 3R's gate.
    - **Text fixes:**
      - Tests cite "every profile (D11)" (`196`); the profiles are D2.
      - D9 says "query's budget (D11)" while D11 says "node budget (D17)"; R13 replaces
        both.
      - D14 says PLAN lists items 1–3 only (`188-189`). `workload-profiles.md:129-131`
        places item 6 in Phase 4, while its status line (`3-7`) schedules 1–3. One
        sentence settles which governs; R16 may move D14 into Phase 4.
      - D5's "1-D trees serve numeric and date ranges" (`87`) lists no 2-D or 3-D use
        now that D15 owns geo; it names what each dimension count serves, by D15's
        result.

## Conditions

1. **No format-13 code until a revision check passes R1–R7.** Format 13 freezes once,
   after Phase 3's per-section disk rows and D15's measurement (R1).
2. **Phase 4's gate includes Phase 3's ratchet rows** (`PLAN.md:453`). Most of them are
   unmeasured today (`docs/gates/phase-3-ratchets.md`). Phase 4's gate report states
   them at the commit it reviews, not carried from an earlier session.
3. **Clean room.**
   - Every Xapian behavior Phase 4 reproduces enters as a specification from the
     public documentation plus `strata-oracle` vectors: estimates, `_percent`,
     collapse counts, sort tie rules, `MatchSpy` contents, and HTM-era geo match sets.
   - Xapiand's replacement for `sortable_serialise` (R3) is written from this design's
     map, never from the GPL file or compared against its bytes; migration runs through
     DUMP and RESTORE.
   - S2's cell-id and covering math, if implemented, comes from its published
     description; if the Apache-2.0 library is used, its notice ships in `NOTICE`.
4. **Library commits.** `bitpack` (widths 33–64), `docset` (an on-disk form with rank),
   and `bkd`, each land with their own CI green on Linux, macOS, and FreeBSD, x86_64 and
   arm64, with their fuzz targets, and with the `cpp-libraries` catalog updated. A
   library change that Detent links is coordinated first.
5. **Track P.** P11 (count and aggregation answers from per-segment statistics) is the
   home of R4's statistics shortcuts. Each lands with its four-axis entry in
   `docs/perf/`, and only on segments without deletes. D15's comparison is a
   `docs/perf/` entry like a Track P item, every platform included.

## Findings

Not covered by the required changes:

1. **The planner** (`phase-4.md:124-129`). Lucene's `IndexOrDocValuesQuery` compares
   the points cost against the lead cost with a factor, not equality. The design states
   strata's rule:
   - the comparison and its factor, as a preset;
   - the decision for a range clause alone, where the clause is its own lead;
   - the decision for a disjunction.
   Xapiand also indexes "accuracy" terms for ranges (`schemas/accuracy.md`). With
   points, those terms become a disk cost the planner never needs; Phase 7 decides
   whether Xapiand keeps generating them.
2. **Exhaustive mode** (`phase-4.md:156`) is implied by aggregations and by
   `check_at_least` up to its count. Scoring can still skip documents whose score
   cannot matter while counting continues. The design states that aggregation queries
   iterate every match but score lazily, as Lucene's collectors do. Otherwise the
   aggregation class pays for scoring it never uses.
3. **Calendar intervals.** A date histogram with month or year buckets needs time-zone
   rules. The core has fixed intervals only. Calendar buckets are host-computed range
   aggregations, or the design states where the time-zone data lives.
4. **`stats` sum precision on disk.** If R4's exact accumulators are persisted per
   segment, their width is a format field (R1).
5. **The working tree's limit changes** (`format.hh:71-73`, `index.hh:1044`) raise the
   accepted term length to 2^32 − 1 at stage 1. Under groups, R18's effective bound
   applies first. A unit test for a term just under and just over the command limit,
   once Phase 3R carries terms in commands, makes the two limits agree.

## Unaffected

These parts stand as written:

- One format version for all Phase 4 work, frozen before code, with a fixture and the
  previous version rejected.
- D3's map: typed `uint64_t` keys with the sign flip for integers and the sign-aware bit
  flip for doubles; −0.0 folded into +0.0; NaN rejected; bytes as dictionary ordinals.
- Slots as sparse 32-bit ids with a per-segment directory; per-block minimum and
  maximum for skipping; GCD and minimum-relative packing as measured choices.
- BKD trees packed in preorder, with the leaf size a preset; whole leaves inside a range
  added without comparing their points.
- D15's method: candidates behind one API, measured, one kept, the rest deleted.
- Scored filters as iterators with `weight()` and `max_weight()`.
- Ties by docid ascending.
- Exhaustive aggregation over matches, as a documented change from `MatchSpy`, which
  the E2E suite does not observe.
- Associative partial results as the distributed shape.
- The key dictionary out of the term dictionary, optional sections, and real-time get,
  scheduled before the format freezes.

## Which code starts now

| Item | Starts now | Waits for |
|---|---|---|
| D3's ordered-key map | Integers and doubles in strata's format code, with the round-trip and order tests | Dates (R3); PLAN, FEASIBILITY, and ARCHITECTURE amended in the same commit |
| `bitpack`, `docset` extensions | 33–64-bit widths; an on-disk presence form with rank, as library work behind their own CI | Its use in format 13 (R1, R4) |
| `bkd` | 1-, 2-, and 3-D build and query in memory, against brute force, with a fuzz target | Shape documents' path and per-dimension widths (R5) before any on-disk layout |
| D15 offline measurements | Coverings, false-positive area, exact-test cost, and stored-form cost (R5's rows 1, 2, 4, 5), on `geonames` and a shape corpus, once the host's trials finish | Nothing; engine integration of the survivors waits for those rows |
| Oracle vectors (`strata-oracle`, GPL side) | Value ranges, sort keys, collapse and its counts, estimates in each tier, `_percent` with scored filters, `MatchSpy` contents, HTM-era geo match sets | Nothing |
| Format 13 (all sections) | Nothing | R1–R7, Phase 3's disk rows, D15's result, and a revision check |
| Slot table and its operation | Nothing | R2, and R15's command-encoding version |
| Doc values, points, keys, term lists in segments | Nothing | Format 13 |
| Scored filters, sort keys, collapse | In-memory semantics against brute force, behind no wire form | R8–R10 before any wire form or pruning hook |
| Estimates and `_percent` | Nothing | R11 |
| Aggregation collectors | Exact accumulators and the per-segment ordinal counting, as standalone code with tests | R12 and R13 before the query path uses them |
| Query memory budget | Nothing | R13 |
| Real-time get | Nothing | R14, and its order with Phase 3R |
| Index sorting (D14) | The format reservation, inside R1 | R16's ruling on Lucene's configuration; Track P otherwise |
| Benchmark engine code | Lucene, Tantivy, and Xapian engines on `geonames`, once the host's trials finish | R16's corpus subset, query sets, and configurations before any gate run |
| Document size (chunked transactions, frame runs) | Nothing in Phase 4 | R19: a Track C design coordinated with Detent; the continuation flag in R1 |
