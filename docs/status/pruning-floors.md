# pruning-floors

Branch `perf/pruning-floors` from main `cb36499`. Two explorer findings that both change
which documents top-k scores, finished together: the optional side's own minimum in
`required_optional_scorer` (explore/req-opt, bcf0796) and the seed floor kept for counted
disjunctions (explore/topk-count, 3d8da0f). Needs one review before landing (it changes which
documents are scored).

## State

Built, verified, measured; waiting for the review (it changes which documents are scored).
Ledger of numbers: `docs/perf/pruning-floors.md`.

## Done

- 337f1ce both changes plus tests; eaa7c0d fake scorer fix.
- Identity (dump counted/uncounted, verify) for req, topk and both on 100k merged, reordered, 4-segment
  flushed and 1M merged, reordered: identical, 0 mismatches.
- ctest Release 39/39; ASan+UBSan 39/39; reverts and two mutants caught.
- Timing at 1M reordered, both presets, 12 rounds: both changes hold.
- Required-side minimum takes the same margin; mirrored crafted test, identity, ctest, timing in the perf doc.

- Review (docs/reviews/pruning-floors.md, approve with conditions): CR1 margin test at n = 2..1024
  (fails on the quarter-margin mutant); CR3 `score_terms` pure virtual; CR5 documented at `top_k` and
  pinned by a counted-floor-above-best test; CR2 comment reworded.

## Next

Review, then land. Open: a 1-3% slowdown on classes neither change reaches (layout suspected).

## Blocked on

Nothing.
