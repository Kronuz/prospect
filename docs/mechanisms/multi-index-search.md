> Status: CURRENT (2026-10-27) `include/prospect/shards.hh` on `phase4/host-collectors`; not landed.

# Multi-index search

Xapiand splits an index into shards (five by default), each a prospect index with its own docids from 1,
and searches them as one database. `shards.hh` does the same for prospect: one snapshot over the shards,
Xapian's docid numbering, the statistics of all shards together, and two ways to search it. The pruned
way is the existing top-k, merged across shards. The exhaustive way visits every match, which is what
a host sort key (key, then relevance) and a match spy (aggregations) need.

## What it guarantees

A search over `n` shards answers exactly as a search over one index that holds the same documents at the
interleaved docids: the same documents, in the same order, with the same scores and the same count.
The oracle's vectors (`test/fixtures/xapian-shards`, from `strata-oracle:25be4a3`) pin that to Xapian's
own answers for 1, 2, 3, 5 (one with two empty shards) and 7 shards.

## The snapshot

```
shard 0: seg a, seg b      shard 1: (empty)      shard 2: seg c
        \_______ multi_snapshot: segments() = [a, b, c], shard s = a sub-span ______/
                 stats() = sum of every segment's documents and lengths
                 df(term) = sum of every segment's document frequency
```

`multi_snapshot` takes each shard's segments (what `index::segments()` gives) and keeps them shard by
shard in one flat list. Statistics are the sum over the flat list (`snapshot_statistics`,
`snapshot_df`, which already take any list of segments), so a weight built from `stats()` and `df()` is
the weight a single index would give. Deleted documents count, as they do in one index (phase-3 D13).
A shard may be empty; it keeps its slot in the numbering.

Document-length bounds are the smallest minimum and largest maximum length over the segments that hold
documents (`doc_length_bounds()`). The writer already stored them in the term index's statistics; the
reader now exposes them (`min_length()`, `max_length()`). They are exact for a fresh segment and
count deleted documents, as a bound may. Xapian's glass backend answers the same (the vectors'
`lower_length`, `upper_length`), and empty shards do not lower them.

## The docid map

Shard `s` (from 0) of `n`, document `d` (from 1) is document `(d - 1) * n + s + 1` across the shards.
Within a shard the map is increasing, so a shard's docid order is also its order in the interleaved
numbering. `interleave_docid` throws on 0, on a shard outside the range, and on a number that does
not fit in 64 bits; `split_docid` inverts it. `last_docid()` is the largest interleaved last docid of a
shard (a shard that never gave one counts for nothing), which is what Xapian reports.

## Pruned search

Without a key, a visitor or descending docids, each shard runs the existing `search()` (block-max
pruning, counting shortcuts) with the global weights, and the lists merge by score descending, then
interleaved docid ascending, truncated to `k`. This is exact: a document in the top `k` of the whole is in
the top `k` of its own shard, since the shard's ranking is the whole's ranking restricted to it (the
order within a shard is the same in both numberings). Counts add. A threshold shared between shards would
prune more; it is not built.

## Exhaustive search

`exhaustive_search` walks every live match of every segment, shard by shard, once. For each match it
builds a `match` (shard, segment, ordinal, the shard's docid, the interleaved docid, and the score, which
the scorer computes only if asked), calls the visitor, and offers the match to a heap of the `k` best.

Ranking, as Xapian's `set_sort_by_key_then_relevance(key maker, reverse)` with `set_docid_order`:

1. Key ascending by memcmp (descending when `reverse_key`); the key comes from the host callback.
2. Relevance descending, whatever `reverse_key` says.
3. Docid ascending, or descending for `docid_order::descending` (`dont_care` is ascending).

Without a key, only the last two apply. The key is called for every match when `k > 0`; the score
only when the key ties the worst kept or the heap is not full.
The pruned path serves everything that has no key, no visitor and ascending docids.

Deleted documents are skipped by wrapping the scorer in `live_scorer`, as `search()` does. The visitor's
order is shard, segment, ordinal; an exception from the key or the visitor leaves the search with the
exception and nothing else changed.

## Why aggregations need the visitor

An aggregation folds in every match, whatever `check_at_least` the request carried
(Xapiand `docs/mechanisms/aggregation-scope.md`). With the visitor, the host's spy sees each match
exactly once and reads its values through the match's segment and ordinal (or its docid). The same pass
also fills the hit list, so a request with hits and aggregations scans once.

## Not built

1. Collapse, percent and weight thresholds, and `get_eset`: the census saw none of them.
2. `_percent` of a hit (relative to the global best hit's weight): computed by the caller from the first hit.
3. Match estimates at `check_at_least` 0: strata counts exactly when asked (`count`), which is Xapian's
   estimate when it checks everything (tier 1). Xapian stops after `check_at_least` documents, so its
   bounds meet only below that.
4. A threshold shared between shards in the pruned path; shards in parallel (the snapshot is read-only,
   `shards_test` runs searches from four threads).

## Where to look

- `include/prospect/shards.hh`: all of it.
- `test/shards_test.cc`: the oracle rows, the identity property over random splits, the visitor, threads.
- `test/shards_mutants.py`: one mutant per claim above.
