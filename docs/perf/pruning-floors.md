# Pruning floors: the optional side's minimum and the seed floor for counted unions

Branch `perf/pruning-floors` from main `cb36499`. Finishes `explore/req-opt` (bcf0796) and
`explore/topk-count` (3d8da0f). Both change which documents are scored, so the branch gets one
review before it lands.

## Changes

1. `required_optional_scorer::set_min_competitive_score` gives the optional side its own minimum
   (`s` minus the required side's bound) once the required side cannot compete alone, so a union
   there prunes its cheap clauses. The explorer's margin (`s * 2^-52`) did not cover a required
   side of several leaves: its score sums in the given order and its bound in cost order, so a
   score can exceed the bound by about `n` units in the last place. The margin is now
   `(n + 2) 2^-52 s` for `n` leaf scores (`scorer::score_terms`), rounded down once more, and no
   minimum past 2^20 leaves.
2. A counted `top_k` keeps the seed or cross-segment floor when the scorer counts apart from
   scoring (`scorer::counts_apart`, true for `disjunction_scorer`), as `search()` already does for
   several segments.

## Why the margin cannot drop a competing document

A document competes when `fl(r + o) > s` (the scores are `r` required, `o` optional, `s` a
double). Rounding is monotone and `s` is representable, so that needs `r + o > s` exactly. With
`R` the bound (the floating-point sum of `n` leaf bounds, `R < s` in this state), `r` sums the
same leaves in another order: `r <= R (1 + 2 (n-1) 2^-53 + O((n 2^-53)^2)) <= R + n 2^-52 s`. The
computed `fl(s - R)` is off by at most half an ulp of `s` (at most `2^-53 s`), and so is the
second subtraction. `(n + 2) 2^-52 s` covers all three with `2^-52 s` to spare, and `nextafter`
makes the result strictly lower, so `o <= minimum` implies `r + o < s`. Both presets use the same
summation, so the argument holds for each. `test/pruning_test.cc` builds the case where
`r > R` by one ulp and `o` sits just below `fl(s - R)`: a bare step below `s - R` (the old rounding)
or no margin drops a competing document; it also sweeps `o` across the boundary and runs 20,000
random trials against a brute-force sum.

(The existing required-side minimum, `s - opt max` rounded down once, has the same exposure to the
optional side's summation order; not changed here.)

## Identity

`prospect_engine dump` (962 gate queries, k 10/100/1000, counted and `PROSPECT_DUMP_COUNT=0`,
5,772 lines) is byte-identical to main for each change alone and both together, and `verify`
reports 0 mismatches of 962, on 100k docid-merged, 100k reordered, 100k flushed (4 segments), 1M
merged and 1M reordered.

## Time

1M reordered, one thread, strict hold, 12 rounds with the sides rotated each round, best of 3 per
query, best over rounds, geometric mean over the class, bootstrap 95% over queries. A/A is a copy of
the main binary run in the same rotation. Ratios are variant over main (below 1 is faster).

| Row | bm25 req | bm25 topk | bm25 both | xapian req | xapian topk | xapian both | A/A |
|---|---|---|---|---|---|---|---|
| `TOP_10` iu >3 (10) | 0.627 | 1.017 | 0.639 | 0.591 | 1.007 | 0.590 | 1.00 |
| `TOP_100` iu >3 | 0.803 | 1.005 | 0.790 | 0.760 | 1.007 | 0.764 | 0.98-1.01 |
| `TOP_100_COUNT` iu >3 | 0.790 | 1.005 | 0.804 | 0.772 | 1.006 | 0.764 | 1.01 |
| `TOP_100_COUNT` union 3 (83) | 1.010 | 0.881 | 0.885 | 1.011 | 0.850 | 0.852 | 1.00 |
| `TOP_100_COUNT` union >3 (20) | 1.006 | 0.904 | 0.909 | 1.006 | 0.864 | 0.862 | 1.00 |
| `TOP_10` iu 3 (20) | 0.943 | 1.005 | 0.957 | 0.923 | 1.002 | 0.924 | 1.00 |

Both changes hold on their own numbers (the intervals exclude 1; `TOP_100_COUNT` all-union reads
0.972 and 0.957 with topk). No result mismatches in any row.

Caveat: classes neither change reaches (plain unions at `TOP_10`/`TOP_100`/`TOP_1000`, iu 2 terms)
read 1.01-1.03 slower in every variant containing either change, against 1.00 for the A/A
copy and 1.00-1.01 for a control build of main with only the two new virtual slots. The
sources of the shift are not identified (code layout of the single header is the likely one);
the lead's measurement on main decides whether it is real. Host: macOS, i9-9980HK x86_64, Apple
clang Release; Linux, FreeBSD and arm64 not measured.

## Checks

Full ctest passes in Release and under ASan+UBSan (clang 23, 39 of 39). Each new test fails with
its change reverted; a mutant without the margin and a mutant that drops the collector seed both
fail.
