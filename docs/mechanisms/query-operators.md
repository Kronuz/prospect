> Status: CURRENT (2026-10-27) the operators of `prospect/operators.hh` on branch `phase4/xapian-ops`; workstream `xapian-ops` (ledger `docs/status/xapian-ops.md`).

# Query operators beyond AND and OR

Xapiand's query language reaches six operators that `prospect/search.hh` did not have: SYNONYM, MAX,
XOR, SCALE_WEIGHT, ELITE_SET, and a posting source it hands to the engine. Each is a `prospect::scorer`
in `include/prospect/operators.hh`, so a parent (conjunction, disjunction, required-plus-optional,
exclusion) and the collectors take them without change. Their behavior is Xapian's, pinned by
recorded vectors (`strata-oracle`, command `ops`), never by reading Xapian.

```
 engine::Query node          scorer                       matches                    score
 ------------------          ------                       -------                    -----
 SYNONYM(t1..tn)             synonym_scorer               any term                   one weight, tf = sum of wdfs capped at length,
                                                                                     df = round(N (1 - prod (1 - df_i/N)))
 MAX(q1..qn)                 max_scorer                   any child                  the best matching child's score
 XOR(q1..qn)                 xor_scorer                   an odd number of children  the matching children's scores, summed
 SCALE_WEIGHT(q, f)          scale_scorer                 q                          f * q's score (f >= 0)
 ELITE_SET(n; q1..qm)        elite_set -> disjunction     the n heaviest children    their sum
 PostingSource(slot)         source_scorer<Slot, Source>  accept(value) holds        Source::weight(value)
```

## SYNONYM

The terms count as one: a document holds the synonym when it holds any term, its wdf is the terms'
wdfs summed (a repeated term counts twice) and capped at the document's length, and the weight is one
term's BM25 weight at the union's size. The size is the independence estimate of a union,
`round(N (1 - prod (1 - df_i / N)))`, with one term keeping its own df; a term the segment lacks
contributes df 0. The caller passes the weight, built from the snapshot's statistics and
`synonym_df(dfs, N)`, so segments of one index score a document alike. The query's own wqf is 1, whatever
the terms' wqf. This is the combiner of a wildcard, partial-word or edit-distance expansion and of a text
node with the synonym default operator, which is every use the census saw.

Bound: the weight rises with wdf and falls with length, and wdf is capped at the length, so the best case
is the summed best wdfs at a length equal to them (or the shortest length, when that is longer), widened
by 2^-40 for the Lucene preset's length buckets. It is one bound for the whole list: `max_score(from, upto)`
returns it. `block_end` is the earliest block end among the terms.

## MAX and XOR

Both merge their children in ordinal order with a linear scan of the children (these operators carry a
handful) and differ in which documents they keep and how they combine scores. A nested XOR is the same
operator as its parent: Xapian scores a document in all of `(a XOR b) XOR c` as the sum of three, so the
caller flattens nested XOR before building. MAX forwards the minimum competitive score to its children (a
child's score at most equals the document's); XOR cannot, because a skipped child changes the parity.

## SCALE_WEIGHT

Multiplies the child's score and bound by a non-negative factor; a negative or NaN factor throws
`std::invalid_argument`, as Xapian refuses it. A zero factor keeps the child's matches at weight 0 and a
bound of 0, which is FILTER: `FILTER(a, b)` is `AND(a, SCALE_WEIGHT(b, 0))` (the vectors show the two equal).
It forwards two-phase matching, fill, counting and `fresh`, so a scaled phrase stays two-phase.

## ELITE_SET

`elite_set(children, size)` keeps the `size` children with the greatest keys and ORs them with the existing
`disjunction_scorer`; a size of 0 is 10 and a size at or above the count keeps all. The key of a term is its
weight's `idf` (wqf included); a term the segment lacks has no child and so takes no slot. Equal keys keep
the earlier child: Xapian's pick among equal weights follows no order of the query, so the vectors with a tie
at the edge of the set are not asserted. For a child that is not a term the key is the caller's choice; the
vectors do not pin one.

Known difference from Xapian: when the cut falls among children of equal document frequency (and wqf), the
terms kept can differ. In the 4 tied rows of the 87 term-only recorded rows (about 5%), Xapian's pick was
neither the earlier nor the later children (tied {0,4,9} kept {0,4}; {0,1,2} kept {0,2}; {0,1,3} kept {1});
ranking by idf fits 85 of the 87 rows. Result sets for such a query can differ from Xapiand's today.

## The host source

`source_scorer<Slot, Source>` runs a Xapiand `PostingSource` as a column scan. `Slot` (concept `slot_values`)
is a column of opaque values by ordinal: `next_present(from)`, `value(ordinal)`, `count()`; the values
plugin's reader fills it. `Source` (concept `value_source`) is `accept(value)`, `weight(value)` and
`max_weight()`, as `engine::PostingSource` has them, const and thread-safe, shared by every scorer of a
query. The scorer steps over present ordinals, drops those the source rejects (so a value that does not
decode drops its document), and scores with `weight`. Its cost is the slot's count. `max_weight()` of 0 means
"not said" (Xapiand's `MultipleValueRange` weighs 1.0 and says nothing), so the bound is unbounded and nothing
is pruned on it; a parent still prunes its other children. It has no block structure: `block_end` is the end.

## Tests

`test/operators_test.cc`: Xapian's recorded rows (669 compared; the counts per operator are printed), random
operator trees against a brute-force model on 300, 5,000 and 20,000 documents (iteration, advance, top-k
counted and pruned, count, fresh copies, every bound), and the host source alone, in four tree shapes, and
shared by threads. `test/ops_mutants.py` applies one mutant per operator rule and expects a failure.

## Not built

See the ledger: SYNONYM over a non-term child, ELITE_SET keys for composite children, the match estimates
of the new operators, and a cursor-style posting source.
