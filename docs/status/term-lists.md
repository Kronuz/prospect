> Status: CURRENT (2026-10-27) P2 of xapiand-on-strata: the term-lists plugin. Branch `phase4/term-lists`, worktree `prospect-wt/term-lists`; core change on strata branch `phase4/term-lists-rank` (worktree `strata-wt/term-lists-rank`).

# phase4/term-lists ledger

Workstream: prospect term-lists plugin (xapiand-on-strata P2, strata `docs/design/xapiand-on-strata.md`
step table; format-13 F17). Developer agent.

## State

Resumed 2026-10-28. Built and proven; waiting on the decoder-safety review, then landing. Strata
`phase4/term-lists-rank` is rebased onto strata main (two commits; digests re-pinned at pre-freeze
0x8000000C). prospect `phase4/term-lists` is rebased onto prospect main (P1 values merged) and pins
strata `f6e49bf`, the same change stacked on values-rank (branch `phase4/term-lists-rank-on-values`):
prospect main still pins strata at `b644708`, before strata main's `deleted_bits`/`seg()` API, so
prospect cannot build against the rebased branch until its own strata adoption (f14-strata-pin, K7)
lands. The two strata commits write identical bytes for the same input (flush_memory and
merge_stream digests pass unchanged at 0x8000000B), so prospect's fixtures hold for either pin; the
adoption moves the pin to the landed commit.

**Done:** 46/46 prospect ctest; 14/14 prospect mutants and 4/4 strata mutants killed; fuzz 5 min
(45k runs, no crash, corpus merged to 396 files); ASan+UBSan and TSan clean (below); size measured.

**Next:** decoder-safety review; land strata then prospect; move prospect's pin on adoption.

## Evidence (2026-10-28)

- Re-pin is the version alone: with the frame refactor and opt-in plumbing at pre_freeze 0x8000000B,
  strata's flush_memory and merge_stream digests (main's) pass; at 0x8000000C only they change. Every
  prospect fixture that exists on main differs from main's in exactly 4 bytes (version, header CRC;
  offsets 8-15), same sizes.
- Mutants: prospect `.scratch/mutants.py`, 14/14 killed (the merge-backstop mutant is gone with the
  backstop); strata `.scratch/mutants_strata.py` on the fetched strata: merge_frames output reversed,
  opt-in default on in writer.hh, opt-in default on in index.hh, merge opt-in rule dropped: 4/4 killed
  by term_lists_test.
- Fuzz: fuzz_term_lists, libFuzzer+ASan+UBSan, 300 s, 44,973 runs, no crash or timeout; 385 new
  files merged into `fuzz/corpus/term_lists`.
- Size, 100k-document corpus (`.scratch/corpus/corpus-100000.json`), index bytes without / with
  `PROSPECT_TERM_LISTS=1`: default buffer (1 segment) 43,122,634 / 73,646,086 (+30.5 MB, +71%; the
  term_lists section is 30,493,642 B, 305 B per document, against 15,965,758 B of postings and
  22,446,072 B of positions); `PROSPECT_BUFFER_MB=16` (8 segments) 53,121,568 / 105,515,948
  (+98%: each segment repeats the vocabulary in its lists). Terms are spelled out in every list;
  field ids (F2) or term ordinals would shrink it.
- Sanitizers: prospect ASan+UBSan (term_lists, merge, index, segment, format13, both fuzz replays,
  background_merge, reorder) and TSan (term_lists, index, background_merge, concurrency, merge) pass.

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

## Blocked on

Nothing.

## Log

- 2026-10-27: worktrees created (prospect from `9de01a1`, strata from `phase4/values-rank` b644708,
  stacked because both change `pre_freeze`). Code written: strata frame refactor, opt-in, link hook,
  rank; prospect plugin, check, fuzz target, test.
- 2026-10-28: rebased both branches; see State and Evidence.
