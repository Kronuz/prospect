> Status: CURRENT (2026-10-30) in progress on `phase7c/l4-sliced-search`.

# phase7c-l4: sliced search

- Design: Xapiand `docs/design/phase7c.md` D2 (budget and slice), D12 (search yields at window boundaries), section 5 L4, gate rows T4, T5, T9, T11; review `docs/reviews/phase7c-design.md` names no L4 finding.
- Branch / worktree: `phase7c/l4-sliced-search` at `/Users/kronuz/code/Kronuz/prospect-wt/l4-sliced-search`, from main `prospect:b348da5`; task pinned at `task:9a85e89` (per-resume slice).
- How it works: `docs/mechanisms/sliced-search.md`.
- State: implementing (A/B pending; sanitizers rerun on the final test)
- Done criteria (the lead's brief, 2026-10-30; contributors tick, never edit):
  - [x] S1 Identity: `sliced_test` compares the sliced search (slice 0 ns = every checkpoint, 3 us, one hour) with the synchronous search and with the scoring model: hits, score bits, order, counts, a visitor-fed aggregation (terms, stats, histogram, avg), key sort, descending docids; 4,536 random-corpus comparisons plus 2 x 567 on corpora of 6 to 17 windows; `ctest` 50 of 51 on the first run, the one failure a threshold in the new test (fixed).
  - [x] S2 Yields: at a 0 ns slice, reading every posting resumes >= 0.9 of the windows for TERM, OR, AND, AND_MAYBE, AND_NOT, PHRASE (k above the corpus, no count; exhaustive too); a counted OR or AND_NOT >= 1.8 of them (count and scoring are separate passes); `search_count_task` >= 0.9; a reordered segment's term groups >= 3.
  - [x] S3 Cancellation: after 3 resumes the token is set; the search ends at the next checkpoint with `task::cancelled`, no visitor call after the request, `use_count()` back to 1; a pre-set token ends at the first checkpoint; destroying the suspended task frees the view.
  - [x] S4 Mutants: 17 in `test/sliced_mutants.py`, one per suspension point, the resume-drops-a-match identity breaks, the cancel and pin claims; first run 12 died and 5 survived (the checks were count-only and compared the sliced search with a synchronous one sharing the mutated code); the tests were strengthened (model comparison, no-deletion corpus, separate count assertions) and all 17 die.
  - [x] S5 Sanitizers: ASan+UBSan (Homebrew clang 23, -O1) clean on sliced, shards, search, multisegment, pruning, operators, engine_query, concurrency, multi_phrase; TSan clean on sliced (searches resumed on three worker threads), shards, search, concurrency. Rerun on the final test: pending.
  - [ ] S6 No-yield overhead under `quiet strict`: pending.
  - [ ] S7 `cpp-libraries/tools/preflight.sh` clean; CI jobs recorded.
- Done: `prospect:4dc38ad` slice.hh, coroutine drivers, scorers, searcher, shards, sliced_test, fixture split; `prospect:906c630` strengthened test, synthetic corpus generator, mechanism note.
- Doing: the A/B (queued behind other agents' light holds), then the sanitizer rerun, preflight, CI record.
- Next: fill S6, S7; hand to review.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-30 release build, `sliced_test` ok; full `ctest` 51 of 51 apart from the threshold fixed in `prospect:906c630`.

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
