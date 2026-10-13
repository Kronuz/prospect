# Test-parity inventory

TESTING.md, "Test parity": one row per test area of the Lucene and Tantivy suites, the
strata tests covering it, and its status. Lucene's and Tantivy's tests are read for what
they check and re-implemented; Xapian's are never read (clean room) and enter as oracle
vectors (`tools/parity.cc`, the oracle repository). This first version covers the Phase 2
scope (postings, term dictionary, stored fields, norms, scorers, collectors, counting,
weighting, integrity), read from Lucene 10.4.0 (`releases/lucene/10.4.0`) and Tantivy
0.26.0. The Phase 3 rows (index writers, merges, deletes, crashes) follow each table.

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
| `TestAllFilesDetectMismatchedChecksum`, `TestCheckIndex` | Every file's checksum is checked; an offline index checker | `segment_test` (every changed byte detected, every truncation); `files_test` (every bit flip, every truncation); `index_test` (fail-closed open) | covered: `index::check()` and `strata_engine check` verify every byte and decode every structure; `index_test` checks a changed byte the open does not read |
| `TestLongPostings`, `TestBagOfPostings` | Long posting lists with and without positions | `segment_test` (up to 20,000 documents, positionless terms), `verify` on 5M | covered |
| `TestForUtil`, `TestPForUtil`, `TestPostingsUtil` | Block encodings round-trip; integer overflow | bitpack's tests (every width, every position, `encoded_size`) and fuzzing; `segment_test` resealed overflow checks | covered |
| `TestLucene104PostingsFormat` | The final partial block; impact serialization | `segment_test` at 127, 128, 129 documents; sized and compact impact lists | covered |
| `BasePostingsFormatTestCase` | Docs, freqs, positions; random; empty fields and terms; positions asked where absent; deleted documents' terms | `segment_test`, `search_test` (positionless terms in phrases, review G6), `merge_test` (terms only deleted documents held drop out) | covered; payloads and offsets n/a (strata stores neither, as Xapian does not) |
| `BaseStoredFieldsFormatTestCase` | Random records, order, big documents, empty documents, concurrent reads, merges with deletes | `segment_test` (records past one frame, empty records), `concurrency_test` (TSan), `merge_test` (byte-identical merges with deletes) | covered |
| `BaseNormsFormatTestCase` | Norm values over many ranges and sparsity | `segment_test` lengths and norms against the model | covered |
| `TestFSTs`, `TestPackedInts`, `TestDirectMonotonic` | The FST and packed integers | fst's and bitpack's tests; `segment_test` monotone docids | covered |

### Lucene, Phase 3

| Lucene test | What it checks | strata | Status |
|---|---|---|---|
| `TestTieredMergePolicy`, `BaseMergePolicyTestCase` | The segment-count budget after every flush and merge in simulated append-only and update workloads; the deleted fraction; write amplification under log(flushes)/log(1.5); equal segments merging equally (`testUnbalancedMergeSelection`); maximum-size segments left out; floor-sized merges; singleton merges reclaiming deletes; forced merges to an exact count within 1.5 times the size limit; forced deletes merges | `merge_policy_test` (each of these, with Lucene's budget formula; every merge of two or more checked balanced, since Lucene's write-amplification bound alone passes an unbalanced policy) | covered; merges pending while others run (`testForcedMergeWithPending`) and merges on commit (`testFullFlushMerges`) n/a until merges run concurrently (Phase 3R) |
| `TestIndexWriterForceMerge`, `TestIndexWriterMerging` (`forceMergeDeletes`) | forceMerge to N segments, forceMergeDeletes thresholds | `merge_test` (`force_merge_deletes` rewrites only segments past the threshold, `force_merge(1)` leaves one segment without deletes, the digest unchanged); `merge_policy_test` forced merges | partial: temporary disk space of a forced merge is unmeasured (pending, Phase 3R disk row) |
| `TestSegmentMerger`, `TestIndexWriterMerging` (`testLucene`) | Merged segments hold every live document and answer the same | `merge_test` (merges byte-identical to a flush of the live documents; the result answers as its sources) | covered |
| `TestCrash`, `TestCrashCausesCorruptIndex`, `TestIndexWriterOnJRECrash` | Crashes while indexing and after close leave the last commit openable | `lifecycle_test` (power loss at 10,000 points of seeded workloads, sector-granular), `index_test` (crash during commit opens the old or the new commit) | covered |
| `TestIndexWriterOnDiskFull`, `TestIndexWriterDelete` (`testDeletesOnDiskFull`, `testUpdatesOnDiskFull`) | Disk full during flush, commit, and merge; the index stays consistent and usable | `index_test` (ENOSPC while flushing keeps the buffer and the commit succeeds once space returns) | partial: ENOSPC during a merge pending (Phase 3) |
| `TestIndexWriterExceptions`, `TestIndexWriterOnError` | Exceptions at every stage of indexing and commit; failed syncs | `index_test` (a failed sync fences and reopening recovers), faultfs faults before and after their effect; `lifecycle_test` | partial: faults injected at every write of a merge pending (Phase 3) |
| `TestIndexWriterCommit`, `TestTransactions`, `TestRollback`, `TestAtomicUpdate` | Commit on close, uncommitted work invisible after a crash, two-phase commit, atomic multi-document updates | `index_test` (uncommitted work lost and nothing half-applied, rejected transactions leave no trace) | partial: an explicit rollback to the last commit and commit user data are APIs of Phase 7 (metadata files exist) |
| `TestDeletionPolicy`, `TestSnapshotDeletionPolicy`, `TestIndexFileDeleter`, `TestNeverDelete` | Which commits are kept, snapshots, opening older commits, leftover files | `index_test` retention (current, two predecessors, pinned checkpoints; orphans removed at open), `lifecycle_test` checkpoints and install | covered; pluggable deletion policies n/a (retention is fixed by design D1) |
| `TestIndexWriterDelete`, `TestPendingDeletes` | Deletes by term, applied on flush; deleted documents invisible; whole-segment deletes | `index_test` (delete and replace by key and docid), `multisegment_test` (search with deletes), `merge_test` (deleted documents and their terms dropped) | covered; deletes by query are Phase 4 |
| `TestIndexingSequenceNumbers` | Sequence numbers order operations across commits | `index_test` (replayed positions, the applied position (index, applied)) | partial: concurrent writers n/a (one writer per index, design D1) |
| `TestFlushByRamOrCountsPolicy`, `TestDocumentsWriterStallControl` | Flushes by memory, stalls under pressure | `index_test` (the memory budget: permits, pressure, two indexes sharing one budget) | partial: stalling writers under pressure pending (Phase 3R) |
| `TestIndexWriterMaxDocs` | Document limits per index and segment | none | pending (Phase 3): ordinals are 32-bit; the limit and its errors are untested |
| `TestStressIndexing`, `TestStressIndexing2`, `TestIndexWriterWithThreads`, `TestNRTThreads` | Concurrent indexing and searching | `concurrency_test` (TSan, concurrent reads of a segment) | partial: reads while one writer commits pending (Phase 3R) |
| `TestAllFilesDetectTruncation`, `TestAllFilesHaveChecksumFooter` | Every file truncated is detected; every file has a checksum | `segment_test`, `files_test` (every truncation of every file) | covered |

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

### Tantivy, Phase 3

| Tantivy module tests | What they check | strata | Status |
|---|---|---|---|
| `indexer::index_writer` (commit and rollback, operation groups, lock files, deletes then merges) | Commits, rollbacks, atomic groups, the writer lock, merges after deletes | `index_test` (transactions, commits across crashes, the owner lock in `faultfs_test`), `merge_test` | partial: rollback and delete-all APIs as Lucene's rows |
| `indexer::merger`, `indexer::segment_updater`, `indexer::merge_index_test` | Merges with and without deletes; merges keep block-max data; deleting all documents | `merge_test` (byte-identical to a flush, so impacts are the flush's); `index_test` | covered; a merge whose documents are all deleted writes no segment (`index::merge`) |
| `indexer::log_merge_policy` | Tantivy's log merge policy | strata has one merge policy, the tiered one (ARCHITECTURE, "One implementation per mechanism") | n/a |
| `indexer::delete_queue`, `stamper`, `doc_opstamp_mapping` | Delete ordering by operation stamp | `index_test` (versions, replay) | covered |

## Owed by Phase 3

- Scores against Lucene's own on the gate queries (the `bm25` preset's claim of Lucene
  compatibility).
- `cost()` estimates checked directly.
- Nested WAND or the amended cost model (review C7).
- From the Phase 3 rows: ENOSPC and injected faults during merges, document limits, and
  reads while a writer commits.
