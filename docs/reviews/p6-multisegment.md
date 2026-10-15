# strata review: P6, search over many segments

- Date: 2026-10-15.
- Reviewed: `docs/perf/p6-multisegment.md` at `c65f717` (branch
  `phase3/p6-multisegment`, worktree `strata-wt/p6`).
- Checked against the code it cites at `c65f717`: `include/strata/search.hh`
  (`top_collector` `170-211`, `top_k` `1144-1185`), `searcher.hh` (`19-28`, `84-126`),
  `reader.hh` (`term` `285-303`, `docid` `144`), and `format.hh` (docids kinds).
- Also checked against: PLAN (Phase 3's gate; Track P's P6, which PLAN commits to Phase
  3; the Ratchets' passing rule and memory rows); BENCHMARKS ("Ratchet rows", "Equal
  memory", "before merging"); `phase-3.md` (D13, D14, R2's equal-bound rule); my
  layers review's ≤ 1.03 decision rule.
- Checked by reading. Nothing was built or run. P6-1 is being implemented in parallel;
  this review covers the analysis, not that code.

## Verdict

**APPROVE WITH CONDITIONS.** The analysis is right where it matters, and it explains
the data's shape. Its claims hold against the code:

- **Cost 1.** `collect` drops a hit only `truncated && score <= threshold`
  (`search.hh:188`). `seed` sets the threshold but not `truncated` (`186`), so a seeded
  segment buffers what its scorer emits until it fills 2k.
- **Cost 2.** The generic loop heaps the first k candidates whatever the floor
  (`1166-1176`), and only three scorers override `collect_top_k` (`243`, `441`, `679`).
- **Cost 3.** `search` re-sorts the whole buffer after every segment
  (`searcher.hh:120-122`).
- **Cost 4.** A segment's threshold rises only at its own `truncate` (`205-207`).
- **Cost 5.** `snapshot_df` calls `seg->term` in every segment (`searcher.hh:94-100`).
- **Cost 6.** `term` computes tail impacts whenever df < 128 (`reader.hh:302`).

Costs 1–4 scale with k × segments. Costs 5–6 and the setup costs scale with
terms × segments. That split fits the measured pattern: the excess grows with k, and
`COUNT` carries an excess of its own.

The Lucene description is accurate where it matters:
- one slice without an executor;
- `TermStates` reused per leaf;
- `nextUp` of the queue's top as the minimum competitive score, valid because leaves
  arrive in increasing docid.

Lucene is the right model.

Three points need fixing before the items they govern land:
- the tie rule for P6-1's merge and P6-2's shared threshold assumes docid-ordered,
  docid-disjoint segments, and neither assumption is guaranteed (R1, R2);
- the cross-phase attribution rests on one baseline's ratio across sessions (R3);
- the "before merging" target must stay PLAN's ratio, not an overhead comparison (R4).

P6-1 proceeds now, under R1's rule.

## Required changes

1. **R1. Ties compare docids, never ordinals, across segments** (P6-1's merge, P6-2's
   collector).
   - P6-1 turns the merge into a `std::merge` of two sorted runs. A segment's hits come
     out of `finish()` sorted by `(score, ordinal)` (`search.hh:182, 196`). That equals
     `(score, docid)` only while ordinal order is docid order.
   - Today every segment's docids kind is contiguous or monotone (`format.hh:79-80`).
     Phase 3 R2's permuted kind (reordering at merge, P1) and D14's index sorting break
     that: within a permuted segment, ordinal order is not docid order. The collector's
     own `truncate` then picks the k-th tie by ordinal too.
   - Required:
     - Hits leave a segment as `(score, docid)` before any cross-segment comparison.
     - For a permuted segment, ties at equal score re-sort by docid before the merge.
     - The in-segment collector compares by docid at the truncation boundary.
     - P6-1 writes the merge against `(score, docid)` now, so the rule exists before
       permuted segments do.
   - **Check:** `verify` adds a case with equal scores straddling the k-th position
     across segments (a synthetic index with duplicated documents, as Phase 3's D14
     test builds), and, once P1 lands, the same with a permuted segment.

2. **R2. P6-2's shared threshold, stated against the scorer contract.**
   - Scorers skip only what cannot reach the threshold: blocks and windows whose bound is
     strictly below it (`phase-3.md`, Statistics and ranking). Let the global k-th hit be
     `(s_k, d_k)`. A candidate `(s, d)` enters when `s > s_k`, or `s == s_k` and
     `d < d_k`.
   - The value handed to a segment's scorer is therefore:
     - `nextUp(s_k)` when every docid the segment can hold exceeds `d_k`, judged by the
       footer's `min_docid` (`format.hh:175`). Then a tie cannot enter, which is
       Lucene's rule.
     - `s_k` otherwise. Ties can enter, and the strict-below skip already keeps them.
   - The draft's `nextafter(s_k, −∞)` is correct but looser than it needs to be.
   - The draft's condition, "segments in docid order with disjoint ranges (flush
     order)", is not a property the index guarantees. Replace-by-docid and Xapiand's
     explicit numeric docids put low docids in late segments, and merged segments
     interleave. The test is per segment, `min_docid > d_k`, re-evaluated each time the
     k-th hit changes. It holds in any segment order, so P6-5's size order needs no
     separate rule.
   - Phase 3 R2's in-segment equal-bound skip compares against the k-th hit's docid.
     With one collector, that is the global k-th, which may come from another segment.
     The skip compares the segment's remaining docid range against `d_k`.
   - **Check:**
     - `verify` at k = 1, 10, and 100 on indexes built with explicit, interleaving
       docids (replace-by-docid across flushes);
     - segments visited in index order and in size order;
     - results identical to a brute-force sort by `(score, docid)`.

3. **R3. Attribution to Phase 3's code uses an A/B, not cross-session drift.**
   Dividing by Tantivy's Phase 2 → Phase 3 ratio removes one engine's drift between two
   sessions. It is a diagnostic, and the draft uses it as one. But the merged rerun's
   first question ("did the Phase 3 code change the single-segment path?") is a
   before/after claim. BENCHMARKS answers those in one session, interleaved:
   - `bench/ab.sh` with A at the Phase 2 gate's commit and B at the branch head;
   - both serving the same force-merged index (the format must allow it, else each
     serving its own build of the same corpus, stated);
   - five trials, decided by the ≤ 1.03 rule.
   The Tantivy-normalized table stays as context.

4. **R4. The "before merging" row's target is PLAN's ratio** ("The right 'before
   merging' comparison").
   - The draft's three steps are right and are what BENCHMARKS already defines:
     - baselines built without their final force-merge, with their own merge policies
       on;
     - equal memory by calibration;
     - every engine at its natural segment count.
     Today's rows compare strata unmerged against baselines that `BuildIndex.java:120`
     and `build_index.rs:60-64` force-merge. That is not BENCHMARKS' "before merging",
     and those rows are not gate rows until rebuilt.
   - But the target it derives, "strata's 16-segment overhead at or below Lucene's at
     its natural count", is a diagnostic. PLAN's Phase 3 gate is every class at ≤ 1.0×
     the fastest engine by the interval's upper bound, both before merging and after.
     Both rows gate. The overhead comparison explains the before-merging row; it does
     not replace it.
   - Equal memory is set at the calibration point BENCHMARKS defines, now the cgroup's
     memory measure (my RocksDB-survey review, R5, applies here too). strata's 16
     segments come from a 256 MB buffer against Lucene's 2,047 MB. At equal memory
     strata's buffer, and so its segment count, changes. The before-merging row is
     measured at that count, not at 16.

5. **R5. Two fixes the draft notes become required,** because Phase 3's rows exercise
   them:
   - **`any_deleted()`** scans the whole bitmap per segment per query
     (`searcher.hh:22-28`). T8 gates query rows "on one with deletes", so it reads the
     segment's dead count instead.
   - **The `churn` mode** builds scorers without the snapshot's statistics
     (`strata_engine.cc:769`). T10's comparative soak compares query time and results
     after churn, so per-segment scores make its rows wrong, not just slow. It passes
     `snapshot_stats` as `serve` does.

6. **R6. Each item's measurement** ("How each item is measured").
   - A/B on the 16-segment index and on the merged one. On the merged one, "no change"
     means the upper bound is at or below 1.03.
   - The allocation-counting build at 1 and 16 segments for every item, not only P6-4.
     P6-1 and P6-2 change the collector's buffers. Allocations per query must not grow
     with matches (PLAN's Phase 3 memory row), and after P6-4 they must not grow with
     segments either.
   - Each item lands with its `docs/perf/` entry on all four axes. Track P's platform
     rule applies: Linux and FreeBSD in the VMs, arm64 when its machine exists.
   - `verify` and `parity` at zero mismatches on the three indexes the draft names, plus
     R1's and R2's tie indexes.

## Rulings

1. **The ranking.** It is right, with two adjustments:
   - **P6-7 moves up beside P6-3.** It is a few lines once P6-3's table exists, and it
     removes `COUNT`'s term-class cost outright. It applies only to segments without
     deletes, as the single-segment path does (C3).
   - **P6-2 subsumes most of P6-1.** P6-1 lands first because it is small and already
     in progress. P6-2 then removes P6-1's merge entirely. P6-1's merge code is
     temporary, and its A/B should not be over-engineered.
   - P6-6's skip uses whole-list bounds that include deleted documents. They are still
     upper bounds, so the skip is safe with deletes, but it is off when counting.
2. **The term cache across queries** stays out, for the draft's reason: it measures
   repeated queries, and neither baseline does it.
3. **Determinism.** No item changes results, counts, or scores. They are execution
   order and bounds only, under R1 and R2. Replicas are unaffected, since nothing here
   touches the write path or the digest.

## Which work starts when

| Item | Starts | Waits for |
|---|---|---|
| P6-1 | Now (in progress) | R1's docid comparison in its merge; its A/B and allocation rows before it lands |
| P6-3, then P6-7 | Now | — |
| P6-2 | After P6-1 | R2's rule written into the collector, with its `verify` cases |
| P6-4, P6-5, P6-6 | After P6-2 | — |
| R5's two fixes | Now | — |
| Baselines rebuilt without the final merge, at equal memory | Now (build changes); runs when the VM is free | R4's calibration |
| The merged-index A/B against the Phase 2 commit | When the VM is free | R3 |
