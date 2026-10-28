> Status: CURRENT (2026-10-27) P2 of xapiand-on-strata: the term-lists plugin. Branch `phase4/term-lists`, worktree `prospect-wt/term-lists`; core change on strata branch `phase4/term-lists-rank` (worktree `strata-wt/term-lists-rank`).

# phase4/term-lists ledger

Workstream: prospect term-lists plugin (xapiand-on-strata P2, strata `docs/design/xapiand-on-strata.md`
step table; format-13 F17). Developer agent.

## State

Paused by the coordinator (quota throttle) mid-evidence. The plugin is implemented; strata
`phase4/term-lists-rank` (f6e49bf, pushed) and prospect `phase4/term-lists` are pushed. All 42 ctests
passed at prospect b299cec plus the vocabulary fix; ASan+UBSan (separate `.scratch/build-asan`) and TSan
(`.scratch/build-tsan`) passed term_lists, merge, index, segment, format13, reorder, background_merge,
concurrency and both fuzz replays. Format change: a decoder-safety review follows before landing.

**Doing (stopped):** closing mutant survivors. The last commit is NOT built or run: it removes a redundant
trailing-bytes check in the cursor, drops the unreachable merge backstop, and adds two tests
(`rec({}, 0)` zero count alone; frame-contiguity mutation with 2048-byte frames). Build and run
`term_lists_test` first.

**Next:**
1. Build `.scratch/build`, run term_lists_test; then `python3 .scratch/mut-tree/mutants.py` (copy the three changed files into `.scratch/mut-tree` first; builds `build` there). Wanted: all KILLED. Done so far: 11 of 15 killed; survivors trailing-bytes, zero-count, check-skips-coverage had tests added (unverified).
2. Strata-side mutants (reverse `m.order` in `merge_frames`; opt-in default on in `index.hh`/`writer.hh`; drop the merge opt-in rule): copy strata include to a scratch dir, `-DFETCHCONTENT_SOURCE_DIR_STRATA`.
3. Fuzz: `.scratch/fuzzrun.sh 900` (libFuzzer build with LLVM clang in `.scratch/build-fuzz`); run minutes, add finds to `fuzz/corpus/term_lists`.
4. Size: corpus 100k docs at `.scratch/corpus/corpus-100000.json` (from the strata Linux VM; 1M at `~/bench/search-benchmark-game/corpus-1000000.json` there). `PROSPECT_TERM_LISTS=1 build/prospect_engine index DIR < corpus` against without, default and `PROSPECT_BUFFER_MB=16`; `sections DIR` for the section bytes.
5. Delete `.scratch/build*` dirs and strata `.scratch/build` when done; CI status of both branches.
6. Lead: prospect fixtures and pinned hashes (format13_test CRC, reorder_test hash) conflict with `phase4/values-bytes` (both bump pre_freeze; strata branches stack: values-rank b644708 then term-lists-rank f6e49bf); re-pin after the second lands. Layout order must be inverted, keys, stored, values, term_lists.

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

- Tests: term_lists_test (records, writer flush, index buffer flush, merges incl. reordering and spilled records, off by default, damage: every one-byte change of the section detected, raw and zstd); 42/42 ctest at the re-pinned fixtures.
- Sanitizers: see State.
- Mutants killed (11/15): observe wdf off by one, skipped first posting, reversed term order, prefix not skipping smaller terms, prefix always matching, strict skip_to, unordered/oversized-wdf/empty-suffix accepted, cross-check ignoring wdf or term.

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
