# fields: F2, prospect onto first-class fields

> Status: CURRENT (2026-10-28) in progress on `phase8/fields`.

- Owner ruling (2026-10-28, strata `docs/design/xapiand-on-strata.md` R-P2): no term lists in strata or prospect; P2 (`phase4/term-lists`) is abandoned. No F2 criterion depends on term lists (D10's term-list varint use is dropped; the varint stays for the pool's keys and the internal term key).
- Task (the lead's, 2026-10-28): step F2 of strata's `docs/design/fields-and-keys.md` (revision 1, `docs/reviews/fields-and-keys-design.md`
  with its re-check): a term is (field id, bytes). D3, D4, D5, D7, D9, D10, D14; T1, T3b, T5b, T6; C5; FK-R12, FK-R13.
- Branch / worktree: `phase8/fields` at `/Users/kronuz/code/Kronuz/prospect-wt/fields`, from `fix/strata-main-pin` `eecd03a` plus
  that branch's uncommitted pin diff (strata `c4ea4ce`); rebase onto prospect main once the pin lands and drop the first commit.
- How it works: `docs/mechanisms/fields.md` (to write).

## State

Handed off at ~400k tokens, mid-way: the library is migrated and works end to end; the old tests and tools are not.

- Library (compiles, `fielddict_test` and `fields_test` pass in Release): `fielddict.hh` (D10 varint, builder, reader, cursor, `add_plan`),
  `inverted_plugin::part` as field parts + host length, `validate`/`resolve`, writer/tuple buffer/merge/reader/digest/check on (field, term),
  `segment_snapshot::fields` and name-keyed `find_term`/`snapshot_df`/`term_table` (D14; the scorer builder needs no change because the
  term table resolves per segment, so FK-R13's exhaustive path is covered by the same resolution).
- Strata branch `phase8/fields-f2` (`strata:6b9b4de`, pushed, CI not watched): passes the transaction's field resolution to an inverter
  whose `add` takes it, `validate_context::is_key_field`, `pre_freeze` 0x8000000C. This branch pins it. It must land on strata main
  (a small-fix tier plus the decoder-safety review that follows) before this branch can.
- Tests and tools: `build-all.log` (not committed) lists what fails. A scripted pass (`~/.scratch/f2/fix.py`, applies compiler-reported
  `term(x)` -> `term(0, x)` and `d.terms` -> `d.field().terms`) was run twice and committed in the WIP commit. About 100 errors remain, all in
  callers: `tools/engine_query.hh` (20, names a field "text" via a table), `test/tuple_buffer_test.cc` (19, `add_text`/`add_split` -> `add` with
  `text_input`), `test/index_test.cc` (19), `test/segment_test.cc` (17), `test/engine_query_test.cc` (9), `scan_terms` lambdas now take
  `(field, term, info)` (5), `term_table(segs)` and `df(field, term)` (16), `segment_snapshot{seg, deleted, dead, &fields}` needs the table
  (`index::fields()`; for a bare segment a one-field `strata::field_table` with "text" at id 0), `d.length +=` -> `d.field().length +=`.
- Nothing from T1, T3b, T5b, mutants, sanitizers, fuzz, or CI has been run.

## Done criteria

From the design's F2 row and the lead's task. A criterion is ticked only with evidence in the log.

- [x] F1. `fielddict.hh`: D10 varint (canonical, lead bytes 0xF5-0xFF refused), builder, reader; tests incl. the empty term. Evidence: `test/fielddict_test.cc` ok (varint order/prefix-free/canonical, model compare with thresholds 255/256, empty term, large ids, cursor order, damaged directory and section bytes, rejecting verifier). No mutant yet.
- [ ] F2. T1: bank 0.875x, sparse-kw 1.110x, sparse-num 0.936x, Wikipedia 0.921x or better, on the real builder, directory included.
- [~] F3. (built, `fields_test` rejections; no mutant) `inverted_plugin::part` is a list of field parts (field ref, terms, text/tokens/split_text, length) and the document's length; stage 1 resolves refs, refuses the key field, accepts the empty term.
- [~] F4. (built, `fields_test` flush/merge/digest agreement; old tests unmigrated) Writer, tuple buffer, merge, reader, check, digest on (field, bytes); `find_term(field, bytes)`, `scan_dictionary(field, ...)`.
- [ ] F5. D5: document length = host sum (id term included); identity of search results with the pre-F2 index where scoring is unchanged.
- [ ] F6. D7: key field routes to `keys`; prospect stops reading `key_prefix` (T5).
- [ ] F7. D9/D14 + FK-R13: term table, `snapshot_df`, expansions, scorer builder and exhaustive path take names/ids per shard; T5b (two shards, different declaration order; once on the key or descending path), a sum-by-id mutant dies.
- [ ] F8. T3b multi-field Xapian parity. Phrase (FK-R12): the dump has no positions; drop phrase from T3b and keep it in T4/T7 unless the dump is extended.
- [ ] F9. C5/FK-R9: varint + empty-term decoder tests; per-field min/max through merges as bounds; per-field doc count at merge from surviving postings; live reads expose statistics.
- [ ] F10. T6 open cost (directory header only, no FST read at open).
- [~] F11. Bump to 0x8000000C done on the strata branch; fixtures (`test/fixtures/pre_freeze.txt`, `reorder_test` pinned bytes) to re-pin; decoder fuzz targets; a mutant per claim.
- [ ] F12. ASan+UBSan and TSan in separate dirs with their own deps; CI green; `docs/mechanisms/fields.md`.

## Decisions (shapes the design leaves open)

1. **Internal key.** Below the dictionary every component (tuple buffer runs, merge, `flush_from`) keeps one opaque, sorted
   term key: the D10 varint of the field id followed by the term bytes. It sorts by field then bytes and is prefix-free
   in the field, so no component learns about fields except where the dictionary is written and read.
2. **Dictionary layout** (`fielddict.hh`): see the header's comment; entries only for own-dictionary fields until norms
   arrive (D4), the pool's keys are the same composite key.
3. **Plan before write.** (The plan is an upper bound for a merge, whose inputs overlap; a flush plans exactly.) A dictionary needs its alphabet at construction, so the writer gets, per field, the term count
   and the byte set before the first term (a keys-only pass over the merged terms; a run of 256 entries proves "own"
   without the pass for one field).
4. **Norms.** The segment-level `norms` section (one byte per document, derived from the lengths when not built) is the
   document-length row of D5's table. Per-field norms and statistics (D4) come after the dictionary: the directory's flag
   byte reserves their bits, and the decoder refuses them until then.
5. **Segment version.** The bump is strata's constant; a strata branch bumps it (as P1's `phase4/values-rank` did) and
   prospect pins it. With P2 abandoned, the next after P1's `0x8000000B` is `0x8000000C`.

## Log

- 2026-10-28: read the design, the review and its re-check, `field-table.md`; prospect's reader, writer, tuple buffer. Branch
  made from the pin branch with its working-tree diff applied.

## Next

1. Finish the caller migration above until `cmake --build build` is clean; run the whole suite (Release); fix what the old tests find
   (they were never run against the new layout: expect bugs in `tuple_buffer` sets, merge plan bounds, reorder/values paths).
2. T1 tooling: `prospect_engine index` of a `.terms` file per field and flat (corpora in `strata-wt/fields-and-keys/.scratch/fk/{bank,sparsekw,sparsenum}.terms`,
   split rule `strata/docs/design/fields-and-keys/fieldsize.py`), then `sections`; measure against 0.875/1.110/0.936/0.921. Values in the real
   builder are term infos, not the probe's 4 zero bytes; the pool's infos are non-contiguous across own dictionaries, so ratios may differ: report honestly.
3. T5b in `test/shards_test.cc` (two indexes, fields declared in different orders; once with `search_options::key`), with a sum-by-id mutant.
4. T3b tool (`parity` terms mode), then per-field norms and statistics (D4/FK-R9; the directory flags are reserved, the decoder refuses them), the T6 open-cost row,
   mutants per claim (`tools/mutants`-style: pool threshold, composite key order, plan bound, key-field refusal, resolve), `fuzz_fielddict` target, ASan+UBSan and TSan in separate dirs, the doc `docs/mechanisms/fields.md`, CHANGELOG-free.
5. Land `strata:phase8/fields-f2`, then re-pin; rebase onto prospect main once `fix/strata-main-pin` lands (already rebased onto its `e49453d`).

## Blocked on

Nothing.
