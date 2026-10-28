# fields: F2, prospect onto first-class fields

> Status: CURRENT (2026-10-28) in progress on `phase8/fields`.

- Task (the lead's, 2026-10-28): step F2 of strata's `docs/design/fields-and-keys.md` (revision 1, `docs/reviews/fields-and-keys-design.md`
  with its re-check): a term is (field id, bytes). D3, D4, D5, D7, D9, D10, D14; T1, T3b, T5b, T6; C5; FK-R12, FK-R13.
- Branch / worktree: `phase8/fields` at `/Users/kronuz/code/Kronuz/prospect-wt/fields`, from `fix/strata-main-pin` `eecd03a` plus
  that branch's uncommitted pin diff (strata `c4ea4ce`); rebase onto prospect main once the pin lands and drop the first commit.
- How it works: `docs/mechanisms/fields.md` (to write).

## State

Slice 1 in progress: the field dictionary component (`include/prospect/fielddict.hh`): D10's field varint, the builder
(own dictionaries for fields with at least 256 terms, one pool for the rest, the sampled directory) and the reader
(lazy per-field dictionaries), with tests and the T1 sizes measured through the real builder. Slice 2, wiring it through the
writer, tuple buffer, merge, reader, searcher and shards, is not started.

## Done criteria

From the design's F2 row and the lead's task. A criterion is ticked only with evidence in the log.

- [ ] F1. `fielddict.hh`: D10 varint (canonical, lead bytes 0xF5-0xFF refused), builder, reader; tests incl. the empty term.
- [ ] F2. T1: bank 0.875x, sparse-kw 1.110x, sparse-num 0.936x, Wikipedia 0.921x or better, on the real builder, directory included.
- [ ] F3. `inverted_plugin::part` is a list of field parts (field ref, terms, text/tokens/split_text, length) and the document's length; stage 1 resolves refs, refuses the key field, accepts the empty term.
- [ ] F4. Writer, tuple buffer, merge, reader, check, digest on (field, bytes); `find_term(field, bytes)`, `scan_dictionary(field, ...)`.
- [ ] F5. D5: document length = host sum (id term included); identity of search results with the pre-F2 index where scoring is unchanged.
- [ ] F6. D7: key field routes to `keys`; prospect stops reading `key_prefix` (T5).
- [ ] F7. D9/D14 + FK-R13: term table, `snapshot_df`, expansions, scorer builder and exhaustive path take names/ids per shard; T5b (two shards, different declaration order; once on the key or descending path), a sum-by-id mutant dies.
- [ ] F8. T3b multi-field Xapian parity (phrase: FK-R12: positions in the dump, or phrase dropped to T4/T7).
- [ ] F9. C5/FK-R9: varint + empty-term decoder tests; per-field min/max through merges as bounds; per-field doc count at merge from surviving postings; live reads expose statistics.
- [ ] F10. T6 open cost (directory header only, no FST read at open).
- [ ] F11. Pre-release bump (strata `pre_freeze` next after P1/P2), fixtures re-pinned; decoder fuzz targets; a mutant per claim.
- [ ] F12. ASan+UBSan and TSan in separate dirs with their own deps; CI green; `docs/mechanisms/fields.md`.

## Decisions (shapes the design leaves open)

1. **Internal key.** Below the dictionary every component (tuple buffer runs, merge, `flush_from`) keeps one opaque, sorted
   term key: the D10 varint of the field id followed by the term bytes. It sorts by field then bytes and is prefix-free
   in the field, so no component learns about fields except where the dictionary is written and read.
2. **Dictionary layout** (`fielddict.hh`): see the header's comment; entries only for own-dictionary fields until norms
   arrive (D4), the pool's keys are the same composite key.
3. **Plan before write.** A dictionary needs its alphabet at construction, so the writer gets, per field, the term count
   and the byte set before the first term (a keys-only pass over the merged terms; a run of 256 entries proves "own"
   without the pass for one field).
4. **Norms.** The segment-level `norms` section (one byte per document, derived from the lengths when not built) is the
   document-length row of D5's table. Per-field norms and statistics (D4) come after the dictionary: the directory's flag
   byte reserves their bits, and the decoder refuses them until then.
5. **Segment version.** The bump is strata's constant; a strata branch bumps it (as P1's `phase4/values-rank` did) and
   prospect pins it. Next after P1/P2: `0x8000000D`.

## Log

- 2026-10-28: read the design, the review and its re-check, `field-table.md`; prospect's reader, writer, tuple buffer. Branch
  made from the pin branch with its working-tree diff applied.

## Next

Slice 1 as in State; then slice 2.

## Blocked on

Nothing.
