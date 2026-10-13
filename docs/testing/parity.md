# Test-parity inventory

TESTING.md, "Test parity": one row per test area of the Lucene and Tantivy suites, the
strata tests covering it, and its status. Lucene's and Tantivy's tests are read for what
they check and re-implemented; Xapian's are never read (clean room) and enter as oracle
vectors (`tools/parity.cc`, the oracle repository). This first version covers the Phase 2
scope (postings, term dictionary, stored fields, norms, scorers, collectors, counting,
weighting, integrity), read from Lucene 10.4.0 (`releases/lucene/10.4.0`) and Tantivy
0.26.0. Phase 3 rows (index writers, merges, deletes, crashes) follow before its gate.

Statuses: **covered** (a strata test checks the same behavior), **partial** (some of it;
the rest is listed), **pending** (owed, with its phase), **n/a** (strata has no such
feature, with the reason).

## Lucene

| Lucene test | What it checks | strata | Status |
|---|---|---|---|
| `TestBoolean2` | Fixed and random boolean queries against expected hits and scores | `search_test` (OR, AND, AND_MAYBE, AND_NOT, exclusion chains, conjunctions of disjunctions, against brute force, both weighting presets); `verify` on the gate queries; `parity` | covered |
| `TestBlockMaxConjunction` | Pruned conjunctions equal exhaustive ones | `search_test` pruned top-k against the model; `verify` (ordinals, ties as sets) | covered |
| `TestMaxScoreBulkScorer` | Disjunctions of two and three clauses, with skipping, filters, and deletes | `search_test` OR kinds, pruned; `multisegment_test` with deletes | covered |
| `TestWANDScorer` | WAND with minimum-should-match and filters | strata has no WAND (MaxScore, ARCHITECTURE "C7") and no minimum-should-match | n/a; nested WAND pending Phase 3 (review C7) |
| `TestReqExclBulkScorer` | Exclusions, random and two-phase | `search_test` kinds 3, 7, 10; `multisegment_test` (`live_scorer` under two-phase parents) | covered |
| `TestReqOptSumScorer` | Required plus optional: must and filter, block bounds, frequent and rare optional clauses | `search_test` kinds 2, 8, 9 (pruned and exhaustive) | partial: non-scoring filters arrive with scored filters (Phase 4) |
| `TestConjunctionDISI` | Conjunctions, approximations, nested approximations | `search_test` kind 5 (phrase inside a conjunction); `multisegment_test` (nested `live_scorer`) | covered; sub-conjunction collapsing n/a (no query rewriting) |
| `TestPhraseQuery` | Exact phrases, slop, order, repeated terms, missing terms, positions | `search_test` kinds 4, 5 (phrases), 6 (NEAR, Xapian's window semantics); `parity` phrases and NEAR | covered (phrases repeating a term, "a b a", in the brute-force comparison); sloppy phrases are Xapian's NEAR, not Lucene's slop (n/a) |
| `TestTermScorer` | next, advance, top docs | `segment_test` (iteration, advance against the model), `search_test` | covered |
| `TestTopDocsCollector` | Order, ties, minimum competitive score, totals | `search_test` ties and pruning; ties by ordinal and docid (`segment_test`, `multisegment_test`) | partial: paging past the first k (search-after) is an API of Phase 7; totals are always exact (estimates arrive with Phase 4) |
| `TestTopDocsMerge` | Merging per-shard top docs | `multisegment_test` (merging per-segment top k by score and docid) | partial: merging across shards is Track D5 |
| `TestBM25Similarity` | BM25 parameter validation | weighting presets have fixed parameters | n/a until parameters are settable; scores against Lucene's own on the gate queries pending (Phase 3) |
| `TestTermsEnum` | Term enumeration, floor blocks, common prefixes, automaton intersection | termdict's tests (random keys, golden fixture, restart runs, fuzzing); `segment_test` term iteration | partial: automaton intersection is Phase 5 (`automata`) |
| `TestNorms` | Norm values at the byte range's edges | `segment_test` length quantization (monotone, rounding down) and lengths against the model | covered |
| `TestAllFilesDetectMismatchedChecksum`, `TestCheckIndex` | Every file's checksum is checked; an offline index checker | `segment_test` (every changed byte detected, every truncation); `files_test` (every bit flip, every truncation); `index_test` (fail-closed open) | partial: an offline index checker is pending (Phase 3) |
| `TestLongPostings`, `TestBagOfPostings` | Long posting lists with and without positions | `segment_test` (up to 20,000 documents, positionless terms), `verify` on 5M | covered |
| `TestForUtil`, `TestPForUtil`, `TestPostingsUtil` | Block encodings round-trip; integer overflow | bitpack's tests (every width, every position, `encoded_size`) and fuzzing; `segment_test` resealed overflow checks | covered |
| `TestLucene104PostingsFormat` | The final partial block; impact serialization | `segment_test` at 127, 128, 129 documents; sized and compact impact lists | covered |
| `BasePostingsFormatTestCase` | Docs, freqs, positions; random; empty fields and terms; positions asked where absent; deleted documents' terms | `segment_test`, `search_test` (positionless terms in phrases, review G6), `merge_test` (terms only deleted documents held drop out) | covered; payloads and offsets n/a (strata stores neither, as Xapian does not) |
| `BaseStoredFieldsFormatTestCase` | Random records, order, big documents, empty documents, concurrent reads, merges with deletes | `segment_test` (records past one frame, empty records), `concurrency_test` (TSan), `merge_test` (byte-identical merges with deletes) | covered |
| `BaseNormsFormatTestCase` | Norm values over many ranges and sparsity | `segment_test` lengths and norms against the model | covered |
| `TestFSTs`, `TestPackedInts`, `TestDirectMonotonic` | The FST and packed integers | fst's and bitpack's tests; `segment_test` monotone docids | covered |

## Tantivy

| Tantivy module tests | What they check | strata | Status |
|---|---|---|---|
| `postings` (`block_segment_postings`, `skip`, `segment_postings`, `block_search`, `compression`, `loaded_postings`) | Block decoding, skipping, seeking inside blocks, compression round trips | `segment_test` (advance, `block_end`, impact bounds), bitpack's tests | covered |
| `postings::term_info`, `termdict`, `sstable` | Term info round trips; dictionary lookups, streams, merges | `segment_test` (term-info codec in restart runs), termdict's tests, `dictsim` | covered |
| `positions` | Position streams across blocks | `segment_test` positions against the model, counts unlike wdf | covered |
| `fieldnorm` | Norm coding | `segment_test` length quantization | covered |
| `query::term_query`, `query::bm25` | Term queries and BM25 | `search_test` (both presets), `parity` | covered |
| `query::boolean_query`, `union`, `intersection`, `exclude`, `reqopt_scorer` (including property tests) | Boolean evaluation against expected sets | `search_test` brute force over random segments | covered |
| `query::boolean_query::block_wand` | Block-max WAND | strata uses MaxScore | n/a; nested WAND pending Phase 3 |
| `query::phrase_query` | Phrases, positions, scoring | `search_test` kinds 4–6, `parity` | partial: as Lucene's row |
| `query::size_hint`, `query::bitset` | Cost estimates; bitset doc sets | `prefer_windows` exercised through `search_test` counts | pending: cost() checked directly (Phase 3) |
| `collector::top_score_collector`, `count_collector` | Top-k order and ties, offsets, counts | `search_test`, `multisegment_test` | partial: offsets (paging) Phase 7 |
| `store` | Stored documents, blocks, the store index | `segment_test` stored records | covered |
| `query::query_parser` | The query language | Phase 5 (`queryparser`, against Xapian's vectors) | pending, Phase 5 |
| `query::range_query`, `fast_field`, `facet_collector`, `histogram_collector` | Ranges, fast fields, aggregations | Phase 4 | pending, Phase 4 |

## Owed by Phase 3

- Scores against Lucene's own on the gate queries (the `bm25` preset's claim of Lucene
  compatibility).
- An offline index checker over every file of an index.
- `cost()` estimates checked directly.
- Nested WAND or the amended cost model (review C7).
- The Phase 3 rows: Lucene's `TestIndexWriter*`, `TestTieredMergePolicy`, deletes and
  updates, crash and corruption tests; Tantivy's indexer, merge, and delete tests.
