# P6a: bulk top-k for conjunctions of terms

`TOP_1000` intersection was the gate's worst class (1.45×). With k = 1,000 the
threshold stays low, so most candidates are evaluated, and each paid virtual calls
(`approx_advance`, `two_phase`, `skip_blocks`, `score`). A conjunction whose children
are all terms now collects top-k on the postings directly: the lead's blocks are
windows skipped whole when the summed bounds cannot compete; inside one, a document
whose lead score plus the other terms' window bounds cannot compete is skipped before
they move. Bounds are computed only once a threshold exists, so a query with fewer
than k matches pays nothing for them (computing them always cost rare-lead queries
150–400 µs). Scores sum in `score()`'s order, so results are bit-identical.

Commit `03645e7`. Measured on the 300 intersection queries:

| Command | Before | After | Geometric mean |
|---|---:|---:|---:|
| `TOP_10` | 274.2 ms | 242.6 ms | 0.898 |
| `TOP_100` | 363.3 ms | 317.6 ms | 0.893 (before lazy bounds) |
| `TOP_1000` | 471.0 ms | 396.1 ms | 0.882 |

Re-verified with C14's `verify` at `c8f932a` (ordinals with ties as sets, Lucene and Xapian
BM25): 962 + 200 queries, 0 mismatches.
