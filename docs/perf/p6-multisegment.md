# P6: search over many segments

Phase 3's query trials serve a 16-segment index (built by `tools/strata_engine.cc`
through `strata::index`, 256 MB buffer, tiered merges) against Lucene, Lucene with
reordering, and Tantivy, each force-merged to one segment. strata's `TOP_1000` grew
60% over its Phase 2 single-segment number. This note reads the multi-segment query
path, lists what each segment costs each query, compares it with Lucene and Tantivy,
and ranks the P6 changes. Nothing here is measured yet; every cost is a code reading,
and each P6 item names the measurement that confirms or refutes it.

## The measurement

Global class, three trials:

| Command | strata Phase 2 (1 segment) | strata now (16) | Ratio | Tantivy Phase 2 → now | Ratio | strata over Tantivy's drift |
|---|---:|---:|---:|---:|---:|---:|
| `TOP_10` | 1,310 µs | 1,629 µs | 1.24 | 1,267 → 1,528 | 1.21 | 1.03 |
| `TOP_100` | 1,736 | 2,115 | 1.22 | 1,614 → 1,662 | 1.03 | 1.18 |
| `TOP_1000` | 2,373 | 3,790 | 1.60 | 2,496 → 2,526 | 1.01 | 1.58 |
| `COUNT` | 1,042 | 1,274 | 1.22 | 1,326 → 1,377 | 1.04 | 1.18 |

Tantivy's index did not change between phases, so its ratio is the VM's drift between
the two runs. Divided out, strata's excess grows with k: 3% at `TOP_10`, 18% at
`TOP_100`, 58% at `TOP_1000`. `COUNT` grows 18% with no k at all. Two costs, then: one
that scales with k (the top-k collection across segments) and one per segment and term
that `COUNT` exposes because its queries are cheap. Floor-subtracted against the
fastest baseline, `TOP_1000` is 2.2× (intersection 2.4×, negated 3.3×, term 2.4×),
`TOP_10` 1.64×, `COUNT` 1.78×.

## How a query runs over segments

```
serve (strata_engine.cc:351)
  run(segs) :339        parse once; snapshot_stats once (doc_count, total_length)
  search() searcher.hh:108, for each segment in index order:
    build(seg)          merge_repeats, clause_scorer per clause -> seg.term() per term
                        first use of a term: snapshot_df -> seg.term() in all 16
    floor = nextafter(k-th of best)                                   :116
    top_k(*q, k, floor) own collector, own threshold                  search.hh:1144
    best += hits (docid per hit); std::sort(best); resize(k)          :120-122
```

The design is sound: statistics are the snapshot's (D13), each segment after the first
starts from the k-th score found so far, and ties resolve by docid at the merge, so
results match a single segment exactly (C14's `verify` over segments,
`strata_engine.cc:540`). The costs are in how each segment is set up and how its hits
are collected.

### Per-segment costs, by what they scale with

| # | Cost | Where | Scales with |
|---|---|---|---|
| 1 | A seeded floor does not filter hits at entry: `collect` drops a hit only once the collector has truncated, so a segment buffers every hit its scorer emits, up to 2k, and returns k of them, most below the floor | `search.hh:188-189` (`truncated && score <= threshold`), `:1161` | k × segments |
| 2 | The generic loop fills its heap with the first k matches regardless of the floor, scoring each; exclusion, phrase, and required-optional scorers take this path (only term, conjunction, and disjunction override `collect_top_k`) | `search.hh:1166-1176`; `exclusion_scorer` `:800` has no `collect_top_k` | k × segments, plus the exclusion check per candidate |
| 3 | The merge appends each segment's hits, maps each to a docid, and re-sorts the whole buffer (up to 2k) with `std::sort`; each segment's own `finish()` already ran `nth_element` and `sort` | `searcher.hh:120-122`, `search.hh:194-207` | k log k × segments |
| 4 | The threshold inside a segment is that segment's: it starts at the floor and rises only when the segment alone fills k hits above it. A later segment rarely contributes k new hits (about k/i for the i-th), so it prunes against the stale floor for its whole length, where one collector's threshold would rise with every competitive hit | `search.hh:180-211`, `searcher.hh:116` | k, earlier segments most |
| 5 | Each term is looked up twice per segment: `snapshot_df` walks the FST in all 16 segments for the df, then `clause_scorer` walks it again in the segment being built | `searcher.hh:94-100`, `strata_engine.cc:226-229`, `:239` | terms × segments |
| 6 | `segment::term` decodes every posting of a term with df < 128 to compute its competitive pairs, into a fresh vector. A term's df divides by the segment count, so terms with df up to about 2,000 in the corpus fall below 128 per segment and pay this; `snapshot_df` pays it too and uses only `df` | `reader.hh:285-303`, `:1026-1031` | terms × segments × min(df, 127) |
| 7 | `term_info` carries `impacts` as a `std::vector`, copied into `infos` and again into the postings iterator | `strata_engine.cc:241`, `reader.hh:439`, `:991` | terms × segments (allocations) |
| 8 | Scorer construction per segment: postings header and skip-header reads with region checks, the whole-list bound over the impacts, `conjunction_scorer`'s sort, `dynamic_cast` per child, and three vectors | `reader.hh:439-490`, `search.hh:222-225`, `:353-362` | terms × segments |
| 9 | `merge_repeats` copies every clause (a `vector<string>`) into a new vector, per group, per segment | `strata_engine.cc:268-279`, `:283-292` | clauses × segments (allocations) |
| 10 | Per query: `snapshot_stats` is an `unordered_map<std::string, …>`, one node per term; `top_collector` reserves 2k + 1 hits per segment; `best` grows by push_back | `strata_engine.cc:221-231`, `search.hh:180`, `searcher.hh:120` | terms; k × segments |
| 11 | Every segment is entered even when the query's whole-list bound there cannot beat the floor | `searcher.hh:112-118` | segments |
| 12 | Segments run in index order, not size order, so a small segment can set the first floor | `strata_engine.cc:360`, `index.hh:539-543` | k |

Costs 1–4 explain the k scaling. At `TOP_1000` a 16-segment query collects up to
16 × 1,000 hits where a single segment keeps 1,000, maps each to a docid
(`reader.hh:144`; constant time when the segment's docids are contiguous, up to 127
LEB decodes when not), and runs about 30 sorts or selections of 1,000–2,000 hits
instead of one. The negated class (3.3×) pays cost 2 on top: 16,000 candidates scored
and checked against the excluded term instead of about 1,000.

Costs 5–10 scale with terms × segments and explain `COUNT`'s 18%: a three-term query
does 96 dictionary walks instead of 3, and terms under 128 postings per segment decode
their postings twice over. Intersection (2.4×) and negated (3.3×) queries have the most
terms per query and so pay most. `TOP_10`'s small excess suggests these are smaller
than costs 1–4 in absolute terms; the A/B in each item below decides it.

Not a cost: `any_deleted()` scans its bitmap per query (`searcher.hh:22-28`), but
`index::segments()` passes null for a segment with no deletes (`index.hh:541`), so
the benchmark index skips it. With deletes it is a linear scan of the bitmap per
segment per query (78,000 words over 5M documents) and should become the segment's
`dead` count.

## Lucene and Tantivy

**Lucene 10** (the baseline's `DoQuery.java` uses `IndexSearcher` without an executor
and `TopScoreDocCollectorManager(k, null, k, false)`):

- One `Weight` per query. `TermQuery.createWeight` builds `TermStates` (`TermStates.build`),
  which seeks the term once in each leaf and keeps the `TermState` per leaf; the leaf's
  scorer reuses it, so a term costs one dictionary seek per leaf, not two. The
  collection and term statistics (`IndexSearcher.collectionStatistics`,
  `termStatistics`) and the BM25 `SimScorer` are computed once.
- One collector across leaves. Without an executor, all leaves form one slice, so one
  `TopScoreDocCollector` and one priority queue see every leaf in docid order
  (`docBase`). Its leaf collector calls `updateMinCompetitiveScore` in `setScorer`, so
  each new leaf starts pruning at the global k-th score, set as
  `Math.nextUp(pqTop.score)`: leaves arrive in increasing docid, so a later tie loses.
  With an executor, `MaxScoreAccumulator` shares the threshold across slices and
  breaks ties by `docBase`.
- A hit enters the queue only if it beats the queue's top (`score <= pqTop.score`
  returns early), so nothing below the threshold is buffered, and no merge step exists.

**Tantivy 0.26** (`Searcher::search` → `search_with_executor`):

- One `Weight` per query: `Bm25Weight::for_terms` reads `Searcher::doc_freq`, which
  sums each segment reader's df (a dictionary lookup per segment), and the total
  documents and tokens. `TermWeight::specialized_scorer` then looks the term up again
  per segment to open postings: two lookups per segment, as strata does today.
- One collector per segment: `TopDocs::collect_segment` runs
  `weight.for_each_pruning(Score::MIN, …)` with its own `TopNComputer`, then
  `merge_fruits` merges the per-segment top-k. The threshold is not shared across
  segments. strata's design already does better (the floor); its execution of it is
  what costs.
- In this benchmark Tantivy is force-merged to one segment, so none of this is in its
  numbers.

Summary: Lucene shares the threshold and the term lookups; Tantivy shares neither;
both compute statistics once per query, as strata does. Lucene is the model.

## P6 items, ranked

Ranked by expected effect on the gap per line changed. Every item keeps results
identical; C14's `verify` over the multi-segment index, the merged index, and the index
with deletes (`STRATA_DELETE_EVERY`) must stay at zero mismatches.

| Rank | Item | Touches | Expected effect | Axis |
|---|---|---|---|---|
| P6-1 | A seeded floor filters at entry: `collect` drops `score <= threshold` once the threshold is non-zero, seeded or truncated; the generic loop skips a candidate whose `current_bound()` or score is at or below the floor before it scores or heaps it; the merge becomes a linear `std::merge` of two sorted runs | `search.hh:187-189`, `:1166-1180`; `searcher.hh:120-122` | Removes costs 1–3: per-segment hits fall from k to the few that beat the floor. Most of `TOP_1000`'s excess; negated `TOP_K` most | query latency, k-scaled |
| P6-2 | One collector across segments, as Lucene's: hits keyed by docid, the threshold rising within a segment, each segment's scorer handed the live threshold; no per-segment `finish()` or merge | `search.hh:174-212` (collector keyed by docid or segment base), `searcher.hh:108-126`, the `collect_top_k` overrides | Removes cost 4 and what P6-1 leaves of 3; prunes later segments from the first competitive hit | query latency, `TOP_100`/`TOP_1000` |
| P6-3 | Resolve each term once per segment per query: a per-query table, segments × terms, filled by one FST walk each; df sums from it; `clause_scorer` reads it instead of calling `seg.term` again. A df-only lookup that never computes impacts, and impacts for df < 128 computed lazily, only when the scorer needs a bound | `strata_engine.cc:221-262`, `searcher.hh:94-100`, `reader.hh:285-303` | Halves dictionary walks (2S → S per term) and removes the df-only tail decodes (cost 5, 6) | all classes, scales with terms × segments; `COUNT`, intersection, negated |
| P6-4 | No per-segment allocations: `merge_repeats` once per query; `term_info::impacts` as a span or small inline array, not a copied vector; df table as a flat array indexed by term position; `conjunction_scorer`'s vectors and the collector buffer from a per-query arena reused across segments | `strata_engine.cc:236-292`, `format.hh:286-299`, `reader.hh:991`, `search.hh:353-362` | Costs 7, 9, 10; the allocation property row (PLAN.md:89–90) stops growing with segments | allocations per query; latency, small |
| P6-5 | Segments largest first (by live documents), so the first floor comes from the most documents | `searcher.hh:108` (order the span), `strata_engine.cc:360` | Higher early floors (cost 12); helps P6-1 and P6-2 more with uneven tiers | `TOP_K` latency |
| P6-6 | Skip a segment whose query bound cannot beat the floor, from the whole-list bounds already in `term_info::impacts` (sum for conjunctions and disjunctions, the include side for exclusions), before opening postings | `searcher.hh:112-118`, a bound pass beside `build` | Cost 11 and the construction in cost 8 for segments that cannot compete | `TOP_10` latency, many small segments |
| P6-7 | `COUNT` of one term over segments without deletes is the snapshot df (from P6-3's table), as the single-segment path already does | `strata_engine.cc:339-343` beside `:317-321` | No scorer or postings opened for term `COUNT` | `COUNT` term class |

Status. R5, P6-3 (`strata::term_table`, `segment::find_term`), and P6-7 (single-word
`COUNT` from the table, `tools/engine_query.hh`) are implemented, not yet measured; the
engine's query layer lives in `tools/engine_query.hh`, tested by `engine_query_test`.
P6-1 is implemented, not yet measured: `top_collector::collect` and the
generic loop drop hits at or below a seeded floor, and `search` turns each segment's
hits into (score, docid), re-sorting them if a segment's ordinals are not in docid
order, then merges the two sorted runs linearly up to k (review R1). Owed before it
lands: the `bench/ab.sh` rows and the allocation rows (R6). Owed with P1: a permuted-
segment tie case in `verify` and the tests, and the in-segment collector comparing
docids at its truncation boundary.

Determinism. P6-1 keeps the floor strictly below the k-th score
(`searcher.hh:116`), so a document tying it with a smaller docid still enters. P6-2
needs the same rule per segment: a tie at the threshold loses only if the new
document's docid is larger than the k-th hit's. When segments run in docid order with
disjoint ranges (flush order), the threshold handed to a scorer can be exact, Lucene's
`nextUp`; otherwise (P6-5's size order, or merged segments whose ranges interleave) it
must be `nextafter(threshold, −∞)`, and the collector compares (score, docid). Within a
segment, ordinal order is docid order (validated at open), so the existing in-segment
tie rule holds. The output order is (score descending, docid ascending) in every case.

Not recommended: a term cache shared across queries of one snapshot. The benchmark
repeats queries, so it would measure cache hits, and neither baseline does it (Lucene's
query cache holds filter results, not term states). Searching segments in parallel is
out of scope: both baselines serve single-threaded here.

Also noted: the `churn` mode builds scorers without snapshot statistics
(`strata_engine.cc:769`), so its scores are per-segment ones; it should pass
`snapshot_stats` as `serve` does.

### How each item is measured

1. `bench/ab.sh` with five trials, A the branch head, B the item, both on the
   16-segment index; compare with `gate.py`. Report by command and class.
2. The same A/B on the force-merged index: an item that only removes per-segment cost
   must show no change there (no regression on the single-segment path).
3. In-process, `strata_engine bench DIR COMMAND RUNS` on the gate VM per class, for
   the per-query cost without the client.
4. `verify` on the 16-segment index, the merged index, and an index built with
   `STRATA_DELETE_EVERY`, before landing each item.
5. For P6-4, the allocation-counting build: allocations per query by class at 16
   segments and at 1.

## The merged-index rerun

The rerun planned next serves strata force-merged to one segment
(`strata_engine merge`, `strata_engine.cc:173-182`) against the same three baselines.

It tells us:

1. Whether the Phase 3 code (keys, versions, P7, P12, P15, the index layer) changed the
   single-segment query path, by comparing with Phase 2's single-segment numbers
   after removing the VM's drift (the baselines' own Phase 2 → Phase 3 ratios).
2. The size of the multi-segment overhead on this build: 16-segment time minus
   1-segment time, by command and class. That is P6's budget; if P6-1 to P6-3 are
   right, most of it sits in `TOP_1000` and in the many-term classes.
3. A like-for-like ratio against the baselines, all at one segment.

It does not tell us:

1. Which per-segment cost dominates; the A/B per item does.
2. How strata compares with engines that also pay for segments. The baselines'
   numbers come from one segment each, so a 16-segment strata against them mixes
   strata's per-segment costs with an advantage they do not have in serving.
3. Pure overhead, either: merging also changes the postings. In one segment, terms
   with df ≥ 128 get skip blocks and block bounds that their per-segment slices lack,
   so pruning gets better data, not just fewer setups.

## The right "before merging" comparison

BENCHMARKS.md defines "before merging" as the index each engine leaves after its own
indexing run, at equal memory, with its segment count recorded. The baselines' build
programs force-merge at the end (`bench/engines/lucene-10.4.0-st/.../BuildIndex.java:120`,
`bench/engines/tantivy-0.26-st/src/bin/build_index.rs:60-64`), and Lucene's RAM
buffer is 2,047 MB, eight times strata's 256 MB. The comparison needs:

1. Each baseline built without its final merge, with its merge policy left on
   (Lucene's `TieredMergePolicy`, Tantivy's `LogMergePolicy`), its segment count
   recorded.
2. Equal memory: strata under the node budget calibrated to the reference engine's
   peak RSS (D17, `STRATA_BUDGET_MB`), and each baseline's buffer at the setting that
   reaches the same peak, stated in the report. Tantivy's writer splits its heap across
   indexing threads, so its thread count is fixed and stated.
3. The four engines then queried at their natural segment counts, each beside its own
   merged result, so the table shows every engine's multi-segment cost, not only
   strata's.

P6's target follows from that table: strata's 16-segment overhead at or below Lucene's
at its natural count.
