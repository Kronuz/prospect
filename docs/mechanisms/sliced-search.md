> Status: CURRENT (2026-10-30) describes `phase7c/l4-sliced-search` (not landed); numbers are in `docs/status/phase7c-l4.md`.

# Sliced search

A search is a coroutine that awaits a time slice at window boundaries, so a long query shares its reactor with the rest of the loop instead of running to completion (Xapiand `docs/design/phase7c.md` D2 and D12). With no budget the same coroutine runs to completion on the calling thread, which is how the synchronous entry points (`search`, `top_k`, `count`, `search_count`) work.

## How it works

```
search_pinned(pin, snapshot, builder, options, budget)      owns pin + inputs in its frame
  search_task(snapshot, ...)                                 shards, then segments (checkpoint between)
    top_k_task / search_task's collector loop                one segment
      scorer::collect_top_k(c, slice)                        bulk loops: one checkpoint per window
      generic loop: window_gate.due(d) -> checkpoint         scorers with no bulk loop
    count_task                                               counting windows
  exhaustive_search::run                                     gate per window; visitor and key run inside
```

- `slice` (`include/prospect/slice.hh`) is a pointer to a `task::budget` or null. `co_await slice.checkpoint()` is the budget's checkpoint: free while the slice lasts, requeued as demoted when it is spent, throwing `task::cancelled` when the budget's token is set. A null slice never suspends.
- `window_gate` turns "once per 4,096 ordinals" into one compare per match for loops that visit matches in ordinal order.
- Suspension points: every window of the disjunction's block-max MaxScore loop (counting copy included); every lead block of the conjunction's bulk loop and every block, or group of 32 blocks in a reordered segment, of a term's; every window of `count_task` and of an intersection count (`exact_count` is itself a task); the generic `next()` loops of `top_k_task`, the multi-segment collector and the exhaustive scan; between segments and between shards.
- Aggregations and doc-value sort keys have no loop in prospect: the host's `match_visitor` and `host_key` run per match inside the exhaustive scan, so the scan's gate slices them, and their cost counts against the slice they run in. `aggregate::partial::merge` and `render` walk buckets, bounded by the host's byte budget (`partial::bytes`); they are not suspension points.
- State across a suspension is the coroutine frames: the scorer tree, the collector, the per-window buffers, the segment cursor. Nothing is thread-bound (`recycled`'s per-thread blocks accept a free from another thread), so a search may resume on a different thread; `sliced_test` does so under TSan.
- Pinning. `search_pinned(Pin pin, multi_snapshot, scorer_builder, search_options, task::budget&)` moves the host's view handle (any movable owner; strata's `index::view`) into a frame local and keeps the snapshot, builder and options by value, so everything the scorers point into lives until the task completes, throws, or is destroyed while suspended. The by-reference entry points (`search_task`, `top_k_task`, ...) need the caller's view and arguments to outlive the awaited task; the `budget` must outlive it in both forms. A builder must own what it captures.
- Cancellation. The token is read at every checkpoint, spent slice or not. A token set while the search waits demoted throws `task::cancelled` at the resume; the frames unwind, releasing scorers and the pin, and no further host callback runs. A callback already running finishes.
- A single `next()` that scans a long stretch without a match cannot yield inside the scorer; the gate yields at the next match. A window's `fill` is bounded by the window.

## Why this mechanism

- Coroutine drivers over rewriting every scorer as a stepper: the bulk loops keep their locals and shape, and a checkpoint is one line each.
- One implementation: the synchronous API is the same coroutines driven with a null slice (`run_unsliced`), not a second path. A caller on a reactor awaits `search_task` or `search_pinned`.
- Yielding per window, not per document: a window is 4,096 ordinals, which keeps a checkpoint (one TSC read and a compare, in `task::budget`) well under 1% of the window's work.

## Invariants

| Invariant | Guard |
|---|---|
| Sliced and unsliced give the same hits, score bits, order, counts, aggregates | `sliced_test` identity at 0 ns, 3 µs and one hour; mutants "a resume drops the match" |
| Reading every posting resumes about once per window, per scorer kind | `sliced_test` `big()` and `reordered()`; one mutant per suspension point |
| A cancelled search stops at its next checkpoint and frees its view | `sliced_test` `cancellation()`; mutants on `slice` and `search_pinned` |
| The view is pinned while the search is suspended | same test, `use_count() >= 2` while waiting |
| A heavy search does not hold a light one back | `sliced_test` `sharing()` |
| Resuming on another thread is safe | `sliced_test` `threads()` under TSan |

## Numbers

See the ledger (`docs/status/phase7c-l4.md`): the no-yield A/B on this host.

## Code

`include/prospect/slice.hh`; `search.hh` (`collect_top_k`, `exact_count`, `top_k_task`, `count_task`); `searcher.hh` (`search_task`, `search_count_task`); `shards.hh` (`search_task`, `search_pinned`, `exhaustive_search::run`); tests `test/sliced_test.cc`, `test/sliced_mutants.py`.
