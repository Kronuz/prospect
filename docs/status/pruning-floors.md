# pruning-floors

Branch `perf/pruning-floors` from main `cb36499`. Two explorer findings that both change
which documents top-k scores, finished together: the optional side's own minimum in
`required_optional_scorer` (explore/req-opt, bcf0796) and the seed floor kept for counted
disjunctions (explore/topk-count, 3d8da0f). Needs one review before landing (it changes which
documents are scored).

## State

Code and tests written, not yet built (strict host hold by explore-retention until 23:41).

## Done

- Both changes ported; the optional minimum now carries a margin that scales with the
  required side's leaf count (`scorer::score_terms`), argued in `optional_minimum`.
- `test/pruning_test.cc` (crafted boundary, ulp sweep, 20k randomized trials) and a counted-floor
  case in `test/search_test.cc`.

## Next

Build main and variants under `quiet run`; identity (dump, verify), multi-segment run, ctest,
ASan+UBSan, mutants, A/A then A/B timing at 1M reordered.

## Blocked on

Nothing.
