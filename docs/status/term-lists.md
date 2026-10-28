> Status: CURRENT (2026-10-27) P2 of xapiand-on-strata: the term-lists plugin. Branch `phase4/term-lists`, worktree `prospect-wt/term-lists`; core change on strata branch `phase4/term-lists-rank` (worktree `strata-wt/term-lists-rank`).

# phase4/term-lists ledger

Workstream: prospect term-lists plugin (xapiand-on-strata P2, strata `docs/design/xapiand-on-strata.md`
step table; format-13 F17). Developer agent.

## State

Implementation written; building and testing in progress (see Log). Format change: a decoder-safety
review follows before landing.

## Done criteria

From the lead:

1. Each document's terms read back exactly, by ordinal and by prefix, in order.
2. Merge keeps live documents' lists and drops deleted ones.
3. Decoder safety on arbitrary bytes: a fuzz target, run for minutes.
4. Size overhead measured on the search-benchmark-game corpus against the index without term lists.
5. A mutant per claim.
6. ASan+UBSan and TSan, in separate build dirs with their own deps.
7. CI green.
8. How-it-works note `docs/mechanisms/term-lists.md`.

## Decisions (shapes the design leaves open)

1. **Derived, not an input.** F17's lists are the transposition of the postings, so the plugin has
   no document part and no digest records; a document's terms come from the inverted builder, which
   reports each term it writes (`observe`), so terms from `terms`, tokens and split text are all
   covered. `check_segment` (prospect's, `check.hh`) cross-checks lists against postings with an
   order-independent hash; strata's `Index::check` runs the plugin's own check only (frames cover
   the ordinals, every list well-formed).
2. **Frames, not a second record store.** The section uses strata's frame layout (F19): `stored`'s
   builder, reader and merge are factored into `frame_builder`, `detail::frame_reader` and
   `detail::merge_frames`, which `stored` and `term_lists` share. A record is the F17 list without
   positions: `[count LEB][per term: shared LEB, suffix length LEB, suffix, wdf LEB]`; no terms is
   the empty record.
3. **No positions.** F17 allows them "when configured"; no index configures them. They stay in
   `positions` (by term and ordinal through the inverted reader); a flag can add them later.
4. **Opt-in.** The bit is off unless the index asks (`index::options::opt_in`), and a
   `segment_writer` leaves it off unless `structures` has it. strata: `opt_in = true` on a plugin
   leaves its bit out of the default configured set; a merge builds an opt-in section only when
   every source holds it. Requires `inverted` (`requires_of`).
5. **Rank 8** (after `values` 7), as format-13's table says; strata `pre_freeze` is bumped (below).
6. **Flush memory.** The transposition holds 16 bytes per posting plus the distinct terms, charged
   to the flush's budget, then the encoded records until the section is written. A merge holds one
   decoded frame per input.
7. **Reading.** `reader.list(ordinal, frame_cache)` returns a `term_list` view; `begin()`,
   `prefixed(prefix)`, `cursor::skip_to(target)`, `size()`. A prefix read decodes the list from its
   start up to the prefix's end (front coding is sequential); lists are short in Xapiand (75 terms
   on average).

## Done

(see Log)

## Evidence

(fills in as items land)

## Next

1. Build and run under `quiet light`; fix.
2. Fuzz for minutes; size run on the search-benchmark-game corpus.
3. Mechanisms note; push both branches.

## Blocked on

Nothing.

## Log

- 2026-10-27: worktrees created (prospect from `9de01a1`, strata from `phase4/values-rank` b644708,
  stacked because both change `pre_freeze`). Code written: strata frame refactor, opt-in, link hook,
  rank; prospect plugin, check, fuzz target, test.
