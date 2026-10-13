# P7: membership probes in counting windows

A conjunction of terms counts in windows of 4,096 ordinals by ANDing each term's bits.
A dense term (`the`, `of`) filled its whole window, about 3,400 postings, even when
the rarer terms left a dozen candidates. Now a later term with few candidates left
tests each by advancing to it; a bitset block answers from its words. Candidates are
counted only up to the point where probing stops paying: the term's estimated
postings in the window over 128, about one candidate per block.

Commit `814fc71`. Measured on the 300 intersection queries, `COUNT`:

| | Before | After |
|---|---:|---:|
| Total, best per query | 224.9 ms | 206.1 ms |
| Geometric mean of after over before | | 0.964 |
| `+jesus +as +a +child` | | 0.54× |
| `+walk +the +line` | | 0.57× |
| `+medal +of +honor` | | 0.60× |

Thresholds of 32, 64, and 128 candidates per estimated posting were measured: 32
slowed queries with many candidates per window (`+the +book +of +life` 1.25×), where
probing touches nearly every block anyway. Counting candidates over the whole window
cost two-term sparse queries 30–40 µs; the count now stops at the limit.

Not applied: exclusion counting. The excluded terms of the negated class are only
moderately dense (`country` in `+jordan -country -basketball`), so nearly every block
holds a candidate and both probing and filling decode the whole list; the remaining
cost there is filling and block validation (`check_increasing`, 6%).

Re-verified with C14's `verify` at `c8f932a` (ordinals with ties as sets, Lucene and Xapian
BM25): 962 + 200 queries, 0 mismatches.
