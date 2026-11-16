> Status: CURRENT (2026-11-15) `include/prospect/searcher.hh` on `quest/xapiand/prospect-supplied-stats`; not landed.

# Supplied statistics

A weight reads three numbers about the collection: its document count, its total length, and the
document frequency of each term the query names. A search over one index computes them from its own
snapshot. A search over a shard of a larger index must weigh with the larger index's numbers, or
its scores do not compare with the other shards'. `supplied_statistics` is the caller's way to hand
them in: with the sum of every shard's statistics, a shard's scores are the scores one index
holding every shard's documents would give.

## The input

```cpp
struct term_statistics { std::uint64_t df, cf; };        // documents holding the term, sum of its wdf

struct supplied_statistics {
	statistics collection;                               // document count, total length
	std::map<field, std::map<term, term_statistics>> terms;   // by field name, then term
	const term_statistics* find(field, term) const;
	supplied_statistics& operator+=(const supplied_statistics&);
};

supplied_statistics gather_statistics(segments, names);  // a snapshot's own, for the named terms
term_table(segments, const supplied_statistics* = nullptr);
```

One set per index, keyed by field name as queries name fields, so shards that declared their fields
in different orders agree (docs/mechanisms/fields.md). `cf` is carried and returned by
`term_table::cf`; no weight prospect ships reads it (`bm25` and `bm25_xapian` use `df`).

## The flow

```
shard 1 --gather_statistics--\
shard 2 --gather_statistics---+--> sum (+=) --> supplied --> term_table(shard i, &supplied) --> weights
shard 3 --gather_statistics--/                                     (each shard's search)
```

1. The coordinator names the query's terms (field, term).
2. Each shard answers `gather_statistics(its segments, names)`: its document count, total length
   and each term's `df` and `cf`, deleted documents included as in one index (phase-3 D13).
3. The coordinator adds the answers with `+=` and sends the sum to every shard.
4. Each shard builds its query over `term_table(segments, &sum)`; `stats()`, `df(field, term)` and
   `cf(field, term)` then answer from the sum. Scorers, pruning and counting are unchanged.

A caller that supplies nothing passes no pointer (the default) and `term_table` computes from the
snapshot as before.

## What stays local

Per-segment figures are the segment's own whatever the caller supplies: `term_table::df(segment,
field, term)`, the term's postings and its competitive pairs. They size scans and exact counts, not
weights, and a count from statistics (`exact_count`) is the shard's own matches, never the index's.

## Refusals

A term the shard holds but the supplied set lacks throws `std::invalid_argument` ("supplied
statistics lack a term the snapshot holds"): the caller's round trip missed it, and weighing it
by the shard's own frequency would rank wrongly without a sign. The table is left usable. A term
the shard does not hold and the set lacks has frequency zero, as in a collection that lacks it. A
supplied term the shard does not hold is used as supplied: the figure is the caller's.

The set is not checked against the shard (a supplied document count below the shard's own, say):
the caller owns the meaning of the numbers. Statistics gathered before a refresh and a shard read
after it weigh with a stale view, which is the residue the design states for deletes and replaces
(cluster-release D10).

## Not here

The key field's terms are key leaves (D7): their `cf` is 0, and a supplied set may carry any `df`
for them. `multi_snapshot::stats()` and `df()` stay the shards' own; a host with supplied
statistics weighs through `term_table(snap.segments(), &supplied)`.

## Proof

`test/stats_test.cc` (`ctest -R stats`):

1. Identity: the oracle corpora of 3 and 5 shards (`test/fixtures/xapian-shards`, their
   queries: term, OR, AND, AND_MAYBE, AND_NOT, phrase) and random corpora of 3 and 5 shards (skewed,
   with empty shards), each in 1 and 3 segments per shard, without and with deletions. Each shard
   is searched alone with the sum of the shards' gathered statistics, its hits are numbered across the
   shards and merged, and the list equals the merged index's (one segment, the shards' documents at
   the interleaved docids): document ids, order and score bits, at k of 1, 10 and 5000, for `bm25`
   and `bm25_xapian`, with Lucene-mode phrases (`stats` and per-term `df` into `phrase_scorer`) as well.
   The sum equals the merged index's own `gather_statistics`, and each shard's weights equal the
   merged index's (`idf`, `avg`, and scores at sample `tf` and length).
2. Power: the same fan-out without the sum ranks differently from the merged index in many
   queries; a shard's own statistics supplied change nothing.
3. Edges: keying by field, the lacking-term refusal and the table surviving it, sums, and a weight
   from supplied numbers.

`test/stats_mutants.py` applies mutants that ignore the input and expects `stats_test` to fail.
