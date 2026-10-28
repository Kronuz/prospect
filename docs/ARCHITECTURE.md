# prospect architecture

prospect is the search library: the inverted plugin (terms, postings, positions, lengths,
norms), its builder, inverter, and merge, scoring, queries, aggregation, sorting, collapse, doc
values, reordering, and the search layout that names them. It is a layout of
[strata](https://github.com/Kronuz/strata)'s segment core: strata owns the index and its
lifecycle (segments, manifests, merges, checkpoints, budgets, files), the frame of every
segment file, and the keys and stored sections; prospect adds the sections and the read path
that make an index searchable. The name stands for Pruned Ranking Over Scored Postings with
Early-terminating Cursor Traversal (strata's `docs/PLAN.md`, "Names").

What the two share (the data model, the write path, the manifest and commit, concurrency,
memory, I/O, distribution, and the library rules) is in strata's
[ARCHITECTURE.md](https://github.com/Kronuz/strata/blob/main/docs/ARCHITECTURE.md). This file
holds what is prospect's.

## One implementation per mechanism

prospect keeps one implementation of each mechanism: scoring, encodings, query operators.
Behavior compatible with Xapian, Lucene, or Tantivy is a parameter or preset of that
implementation, never a second code path, and its default is the preset measured fastest on
the gate corpus. The weighting has two presets, both from the BM25 family (Robertson's Okapi
formula, whose engines differ in k1, b, idf, and length handling): `bm25`, with Lucene's and
Tantivy's parameters over one-byte quantized lengths, and `bm25_xapian`, Xapian's BM25Weight
over exact lengths, for Xapiand's ranking continuity and the oracle's parity vectors.

## On strata's core

```
 prospect           search_layout = layout<inverted_plugin, keys_plugin, stored_plugin, values_plugin, term_lists_plugin>
                    scorers · collectors · aggregation · sort keys · collapse · reorder
 ───────────────────────────────────────────────────────────────────────────────
 strata::core       basic_index<Layout> · segments · manifests · merges · checkpoints
                    keys and stored plugins · the frame and section table · faultfs
 ───────────────────────────────────────────────────────────────────────────────
 building blocks    docset · bitpack · fst · analysis (unicode, snowball) on prospect's
                    side; termdict · fusefilter · completion · task · ... on strata's
```

`prospect::prospect` links `strata::core`, which CMake fetches pinned by commit, plus `docset`,
`bitpack`, `fst`, and `analysis`. The core never names a search type: it folds over a layout's
plugins and calls their optional static hooks (`builder`, `inverter`, `records`, `buffered`,
`total_length`, `length`, `consistent`, `refuse`, `report`, `check`), and the merge asks
`strata::order_source<Layout>` for an order. strata's
[core-split.md](https://github.com/Kronuz/strata/blob/main/docs/design/core-split.md) lists each
hook and its caller.

prospect fills them:

| Header | Holds |
|---|---|
| `prospect/reader.hh` | `inverted_plugin` and its reader, `search_layout`, `segment` |
| `prospect/values.hh`, `prospect/values_writer.hh` | the values plugin: part, reader, check, digest records; builder and merge |
| `prospect/writer.hh`, `prospect/tuple_buffer.hh`, `prospect/inverter.hh` | the inverted builder, the tuple inverter, and the buffer the core's index inverts into |
| `prospect/term_lists.hh`, `prospect/term_lists_writer.hh` | the opt-in term-lists plugin: each document's terms by ordinal and prefix ([mechanisms/term-lists.md](mechanisms/term-lists.md)) |
| `prospect/merge.hh` | the inverted plugin's merge and `merge_segments` |
| `prospect/reorder.hh`, `prospect/bisection.hh` | `strata::order_source<search_layout>`: bisection over the postings |
| `prospect/check.hh`, `prospect/index.hh` | the check report, and `index`, `transaction`, `operation`, `index_options` over the search layout |
| `prospect/format.hh` | the search sections' encodings: length quantization, skip data, ordinal blocks, threshold seeds, term info |
| `prospect/search.hh`, `prospect/searcher.hh`, `prospect/scorer.hh` | scorers, weights, top-k, counting, and search over segments |
| `prospect/aggregate.hh`, `prospect/docvalues.hh`, `prospect/sortkey.hh`, `prospect/collapse.hh`, `prospect/ordered.hh`, `prospect/exact.hh` | Phase 4's structured data |

`prospect/strata.hh` brings strata's names into namespace `prospect` (and its `format` and
`detail`), so prospect's code names the core's types unqualified. Callers outside both qualify:
`prospect::index`, `strata::faultfs`. The specialization
`strata::order_source<prospect::search_layout>` is declared in `reader.hh` and defined in
`reorder.hh`, so a unit that names the layout without `reorder.hh` does not compile
(`order_source_negative`).

## The search sections

Every segment is strata's frame: sections, a table of contents, and a footer with the format
version and a CRC32C. The section kinds are one numbering space every plugin claims from
(`strata/core/format.hh`). prospect's are the inverted plugin's and Phase 4's:

| Section | Contents |
|---|---|
| Term blocks | Prefix-compressed blocks of 25–48 terms with document frequency, collection frequency, and postings pointers |
| Term index | A small resident FST over block prefixes |
| Postings | 128-ordinal blocks: FOR ordinal deltas, PFOR wdf; LEB128 tails (`varint::leb128`); two skip levels with competitive (wdf, doclen) pairs |
| Positions | 128-position PFOR delta blocks |
| Lengths | Exact per-document length (sum of wdf over non-boolean terms), bit-packed fixed width, with segment minimum and maximum |
| Doc values | Slot directory and columns (numeric: delta, GCD, table, or bit-packed; sorted-set ordinals; binary). The values plugin writes the opaque `bytes` column per slot first ([mechanisms/values.md](mechanisms/values.md)) |
| Points | BKD trees, 1-D numeric and 2-D interval |
| Term lists | Opt-in per field |

Codec rules:

- Blocks use a fixed 4×32-bit interleaved lane layout on disk. SSE4.1 and NEON decode
  it natively; AVX2 processes two lane groups.
- A term with only boolean postings (wdf 0) stores no wdf. A block whose wdf are all 1
  stores a flag instead.
- Postings, positions, and term lists carry segment ordinals, not docids. A flushed
  segment's ordinals follow docid order, since a flush sorts by docid and a replaced
  document's old copy stays in its earlier segment until a merge drops it. A merge may
  reorder its output; then docid tie-breaks and docid-order walks go through the docid
  map's permutation (strata's docids section).
- A term with document frequency 1 stores its ordinal in the term block.
- Tails (fewer than 128 entries, which is most terms' whole posting list) use the
  `varint` format; a tail encoding that measures smaller is added to `varint`.
- Block maxima store competitive (wdf, doclen) pairs, never precomputed scores, so
  distributed rescoring with global statistics keeps valid bounds.

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
   when scoring, `score()` and `max_score(from, upto)`.
   - Conjunctions leapfrog from the cheapest iterator.
   - Top-level disjunctions use block-max MaxScore in 4,096-ordinal windows. Nested
     disjunctions do not lead: under `AND_MAYBE` the optional side only follows the
     required side's documents, where WAND's pivoting has nothing to skip. Measured on
     the harness's 40 `+a b c` queries, the nested disjunction cost 14% of the time, all
     in heap updates; disjunctions of up to four clauses advance their children
     directly instead. WAND waits for a query shape where a nested disjunction leads.
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
