> Status: CURRENT (2026-10-30) in progress on `phase7c/l4-sliced-search`.

# phase7c-l4: sliced search

- Design: Xapiand `docs/design/phase7c.md` D2 (budget and slice), D12 (search yields at window boundaries), section 5 L4, gate rows T4, T5, T9, T11; review `docs/reviews/phase7c-design.md` names no L4 finding.
- Branch / worktree: `phase7c/l4-sliced-search` at `/Users/kronuz/code/Kronuz/prospect-wt/l4-sliced-search`, from main `prospect:b348da5`; task pinned at `task:9a85e89` (per-resume slice).
- How it works: `docs/mechanisms/sliced-search.md`.
- State: implementing
- Done criteria (the lead's brief, 2026-10-30; contributors tick, never edit):
  - [ ] S1 Identity: sliced and unsliced agree (hits, scores, order, counts, aggregations, sort keys) over the query oracle and test corpora at slice sizes from one window to unbounded.
  - [ ] S2 A long query yields at least N times at a small budget (N stated per scorer kind).
  - [ ] S3 A cancellation between slices stops the search and frees its view.
  - [ ] S4 A mutant per claim dies.
  - [ ] S5 ASan+UBSan and TSan clean, each in its own build directory.
  - [ ] S6 No-yield overhead measured under `quiet strict`, before and after, honest magnitude.
  - [ ] S7 `cpp-libraries/tools/preflight.sh` clean; CI jobs recorded.
- Done: (nothing yet)
- Doing: design points below, then the code.
- Next: slice.hh and the coroutine drivers; scorers' bulk loops; searcher and shards; tests; mutants; sanitizers; A/B; note.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: not yet built.

## Design points (settled before coding)

1. Where a search suspends. One mechanism, `co_await slice.checkpoint()` (a `task::budget` checkpoint; free while the
   slice lasts, throws `task::cancelled` when the token is set), at:
   - every window of `disjunction_scorer::collect_top_k` (4,096 ordinals), counting copies included;
   - every lead block of `conjunction_scorer::collect_top_k` and every block or group of `term_scorer::collect_top_k`,
     gated to one checkpoint per 4,096-ordinal window of the scan (`window_gate`);
   - every window of `count()`;
   - the generic `next()` loops of `top_k`, `search` (multi-segment) and the exhaustive scan, once per 4,096-ordinal
     window of the matches (so phrase, exclusion, required/optional, operators and host-source scorers, which have no
     bulk loop, yield there);
   - between segments and between shards.
   Aggregations and sort by doc values have no loop of their own in prospect: the host's `match_visitor` and `host_key`
   run per match inside the exhaustive scan, so they are sliced by its gate (a visitor's cost is charged to the slice it
   runs in). `aggregate::partial::merge` and `render` walk buckets bounded by the host's byte budget (`partial::bytes`);
   they stay synchronous, and are the one place a long non-yielding run remains. A single `next()` that scans a long
   stretch without a match cannot yield inside the scorer; the gate yields at the next match.
2. State that survives. The coroutine frames: the scorer tree, the collector (heap, counts, threshold), per-window
   buffers (`acc`, `bits`, `order`), the segment cursor. Nothing is thread-bound (the per-thread caches in `recycled` and
   the frame pool accept a block freed elsewhere), so a resume may run on another thread.
3. The pinned view. `search_task` takes the host's view handle by value (`Pin`, any movable owner, such as strata's
   `index::view`), plus the snapshot, builder and options by value, and moves the handle into a frame local. The frame
   owns everything the scorers point into until the search completes, throws or its task is destroyed while suspended.
   The by-reference entry points (`search`, `top_k`, `count`, the `*_impl` coroutines) are for callers whose view lives
   across the call.
4. Cancellation. The budget's `cancel_token` is read at every checkpoint, including when the slice is not spent; a token
   set while the task waits demoted throws `task::cancelled` at the resume, unwinding the frames, which release the
   scorers and the pin. No work runs after the throw. A visitor already running finishes its call.
5. Cost when nothing yields. The sync entry points are the same coroutines run with no budget (`slice{}`): a checkpoint
   is one null test, and the driver adds one pooled frame per call (task's frame pool) and per segment bulk call.
   Measured below; the claim is the measurement, not this paragraph.

## Log

- 2026-10-30: read D2/D12/L4, the scorers' window loops, task `budget`; ledger written.
