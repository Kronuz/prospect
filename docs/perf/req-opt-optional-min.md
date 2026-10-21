# Required plus optional: the optional side gets a minimum when it becomes required

Branch `explore/req-opt` from main `cb36499`. Prototype, not for landing.

## Finding

The hypothesis in the task (required candidates are not checked against the optional side's
block-max bound before scoring) is already implemented on main: `required_optional_scorer::prune`
is the Lucene `ReqOptSumScorer` port (window skip when both bounds cannot compete, optional made
required when the required bound alone cannot, a post-score check against the optional bound).
`format13-loss-attribution.md` loss 3's profile ("0 rejected by a bound") was the pre-port code.
Counters on main (100k reordered, 40 intersection_union queries, TOP_10): 452 windows, 0 skipped
by both bounds, the optional side made required 22,214 times, 18,463 scored, 316 rejected after
scoring. So the common state is "the required side cannot compete alone": a conjunction of the
required term with the optional union.

## Change (experiment 1)

In that state the optional side was never told its own minimum. `set_min_competitive_score(s)` now
also passes `s - req max_score_total` (rounded down, with a 2^-52 relative margin) to the optional
side once `sum_upper(req max_score_total) <= s`. When the optional side is a union of several terms,
its MaxScore split then drops the cheap clauses from candidate generation: a document matching only
those cannot compete. Single-line change in `include/prospect/search.hh`.

## Identity

`prospect_engine dump` (962 gate queries, k 10/100/1000, both presets, counted and
`PROSPECT_DUMP_COUNT=0`), byte-identical to main at 100k and 1M, docid-merged and reordered
(5,772 lines each); `verify` 962 queries, 0 mismatches at 100k merged and 1M reordered.

## Time

1M reordered, one thread, strict hold (`quiet run`, no light hold of mine), `ab2.sh` 12 rounds,
best of 3, A/A first per preset, scored by `ab.py --aa` (query-exp's version). B/A, below 1 is
faster; [95% over runs]; A/A in parentheses.

| Row | bm25 | bm25_xapian |
|---|---|---|
| `TOP_10` intersection_union (40) | 0.996 [0.989-1.003], neither (A/A 0.998) | 0.958 [0.931-0.986], win (A/A 1.023) |
| `TOP_10` num_tokens_>3 (10) | 0.692 [0.673-0.711], win (1.000) | 0.551 [0.532-0.572], win (1.023) |
| `TOP_100` num_tokens_>3 | 0.936 [0.912-0.960], win | 0.860 [0.836-0.885], win |
| `TOP_100_COUNT` num_tokens_>3 | neither | 0.872 [0.839-0.907], win |
| `TOP_1000`, `COUNT`, all other intersection_union rows | neither (0.98-1.01) | neither |

No row loses under either preset; no row outside intersection_union moved (the changed code is
reached only by that scorer). Wins sit in the strata whose optional side has three or more terms.
The two-term and three-term strata (optional of one or two terms) barely move, so this closes
only part of the gap for the class; the 1.11-1.41 ratios were measured at 5M against Lucene and
Tantivy and are not re-measured here. Host: macOS, i9-9980HK x86_64, Apple clang Release; Linux,
FreeBSD and arm64 not measured. Earlier 100k, light-hold timings were discarded.

## Verdict

Worth finishing: small, format-free, exact. The remaining cost for short optional sides is the
leapfrog itself (two cursors, required scored per match); not explored.
