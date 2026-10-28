# fields: F2, prospect onto first-class fields

> Status: CURRENT (2026-10-28) in progress on `phase8/fields`.

- Owner ruling (2026-10-28, strata `docs/design/xapiand-on-strata.md` R-P2): no term lists in strata or prospect; P2 (`phase4/term-lists`) is abandoned. No F2 criterion depends on term lists (D10's term-list varint use is dropped; the varint stays for the pool's keys and the internal term key).
- Task (the lead's, 2026-10-28): step F2 of strata's `docs/design/fields-and-keys.md` (revision 1, `docs/reviews/fields-and-keys-design.md`
  with its re-check): a term is (field id, bytes). D3, D4, D5, D7, D9, D10, D14; T1, T3b, T5b, T6; C5; FK-R12, FK-R13.
- Branch / worktree: `phase8/fields` at `/Users/kronuz/code/Kronuz/prospect-wt/fields`, rebased onto prospect main `becfe3c` (the strata pin landed).
- How it works: `docs/mechanisms/fields.md` (to write).

## State

IN PROGRESS (2026-10-28), second developer. Library, callers, tools and the whole suite are migrated: Release ctest 47 of 47.
Proof gathered (details in Done criteria and Log): T1 sizes, T3b parity, T5b shards, search identity with main, a mutant per claim,
a decoder fuzz target, the how-it-works note. Sanitizer builds and CI: see Next.

## Done criteria

From the design's F2 row and the lead's task. A criterion is ticked only with evidence in the log.

- [x] F1. `fielddict.hh`: D10 varint, builder, reader, cursor. `fielddict_test` ok; mutants A (threshold), B (non-canonical form), C (buffer varint order), I (plan) die.
- [~] F2. T1 on the real builder, directory included (`prospect_engine fields`, `.scratch/t1/t1.txt`; targets are the design's rounded-up ratios).
  Bank 0.861 one segment, 0.863 five shards (targets 0.88, 0.88: met); sparse-kw 1.128 and 1.084 (targets 1.11, 1.08: **missed** by 0.018 and 0.004);
  sparse-num 0.929 and 0.861 (0.94, 0.89: met); Wikipedia 100k 0.922 (0.93: met). Bytes: see the Log.
- [x] F3. Field parts, stage 1 (`fields_test` rejections); mutants D (key field), E (reference by number) die.
- [x] F4. Writer, tuple buffer, merge, reader, check, digest on (field, bytes); `fields_test` flush/merge/digest agreement; the whole suite.
- [~] F5. D5 host length: `fields_test` (length 9 for parts of 4 + 4), mutant L dies. Search identity with main: `prospect_engine dump` of 962 queries
  x both schemes x k 10/100/1000 on 30,000 documents is byte-identical (exact scores) to prospect main's engine for one segment, 30 segments, 30 segments
  with 4,286 deletes, and a merge into one (`.scratch/t1/ident.txt`).
- [x] F6. D7: the key field routes to `keys` (`find_term`, mutant G dies); prospect reads no `key_prefix` in the inverted plugin. Terms starting like a key are ordinary terms (index_test, format13_test updated).
- [x] F7. D9/D14: `term_table`, `snapshot_df`, `multi_snapshot::df` by name; T5b in `shards_test` (`fields_by_name`: two shards declaring [title, body] and [extra, title, body], 720 comparisons incl. the key and descending paths, counts equal, scores at 1e-12); mutant F (sum by id) dies.
- [x] F8. T3b on Xapiand's bank (1,000 documents, 49 fields, Q and V wdf in the length): 240 term/OR/AND queries within and across fields, counts and top-10 equal Xapian's, worst relative error 4.1e-16, 1 and 5 segments (`parity_terms`; vectors `strata-oracle:8e6611a`, branch `phase8/rank-terms`, local). Phrase dropped (FK-R12).
- [ ] F9. C5/FK-R9: the varint and empty-term decoder tests are in; per-field min/max through merges, per-field document counts at merge, live statistics: **not built** (norms and per-field statistics are D4, whose directory flag bits are reserved and refused).
- [ ] F10. T6 open cost for the 49- and 2,100-field segments: not run.
- [x] F11. Strata bump 0x8000000C (`strata:32b6800` repins `flush_memory`, `merge_stream`, `format14`; its CI is green on every job); prospect fixtures re-pinned (`test/fixtures`, `reorder_test`, `format13_test`).
  `fuzz/fuzz_fielddict.cc` (two modes: the reader over raw built bytes, and a whole segment with resealed checksums; seeds plus a 7 minute ASan+UBSan run, 57,321 executions, minimized corpus of 751 files, no crash after the fixes below).
  Mutants (`tools/mutants_fields.py`, each applied alone, the named tests must fail): A pool threshold, B non-canonical varint, C buffer varint order, D key field in a part, E reference by number, F sum by id (T5b), G key terms in the dictionary, H reorder by bytes only, I plan counts no terms, L host length ignored, M entry term count ignored: all die. Three of them (C, E, L) first survived and got tests.
- [x] F12. ASan+UBSan (`build-asan`) and TSan (`build-tsan`) in separate dirs with their own deps (Homebrew LLVM clang, `-O1`): ctest 47 of 47 in Release, ASan+UBSan and TSan on the final code. CI green on `prospect:8588afe` (every job; macos-arm64 got no runner on the first try and passed on rerun) and on `strata:32b6800` (every job). `docs/mechanisms/fields.md` written.

## Decisions (shapes the design leaves open)

1. **Internal key.** Below the dictionary every component keeps one opaque, sorted term key: the D10 varint of the field id followed by the term bytes.
2. **Dictionary layout** (`fielddict.hh`): see the header's comment; entries only for own-dictionary fields until norms arrive (D4).
3. **Plan before write.** A dictionary needs its alphabet at construction, so the writer gets, per field, the term count and the byte set before the first term. A merge plans from its inputs, an upper bound.
4. **Norms.** The segment-level `norms` section is the document-length row of D5's table. Per-field norms and statistics (D4) come after the dictionary.
5. **Segment version** `0x8000000C` (strata `phase8/fields-f2`).
6. **A snapshot without a field table throws** (`logic_error`), never answers empty: a missing table was a silent zero.
7. **Tests declare `text`** (`test/text_field.hh`): `text_fields()` for single-field tests, `keyed_fields()` adds the key field `_id`.

## Bugs the proof found

- UBSan: the buffer's hash shifted by 64 for the empty term (`tuple_buffer.hh`); in Release it read arena garbage, so an empty term could split into several entries. Fixed; `tuple_buffer_test` now has the empty term (segfault / mismatched bytes with the fix reverted).
- Reorder grouped graph terms by bytes only (below).
- The fuzz oracle (cursor reads in order) found a dictionary that read out of order; the directory check fenced that input already, but `check()` now also reads the terms against the entries' counts and order (the count is unit-tested, the order only by the fuzz oracle).

## Findings for the lead

- Reorder grouped graph terms by bytes only (a term in two fields merged into one graph term). Fixed (`reorder.hh`); `reorder_test` now puts the same words in a second field; the revert (mutant H) dies.
- The field table's file has no checksum on its header: a changed version byte reads as `unsupported_index` (strata `fields_test` pins that), so `media_test` skips bytes 8 to 12 of `fields-*.kv`. Strata's decision to revisit, not mine.
- A merge may give a field near 256 terms a dictionary of its own that a flush of the same live documents would pool (the plan counts the inputs' terms). Same answers, different bytes.

## Log

- 2026-10-28: read the design, the review and its re-check, `field-table.md`; prospect's reader, writer, tuple buffer. Branch made from the pin branch.
- 2026-10-28 (second developer): rebased onto `becfe3c`; migrated the callers; fixtures repinned; T1, T3b, T5b, identity, mutants, fuzz target, mechanism note.
  T1 (terms + term_index bytes, flat vs per field): bank 133,520 vs 115,005 one segment, 265,141 vs 228,883 five shards; sparse-kw 101,344 vs 114,363 and 150,461 vs 163,137;
  sparse-num 643,215 vs 597,230 and 799,883 vs 688,549; Wikipedia 100k 8,444,293 vs 7,788,531. The design's flat figures are higher because its probe stored 4-byte zero values;
  here the values are real term infos. For sparse-kw the pool's keys carry a 2- or 3-byte field id (2,000 fields) and a full-byte alphabet, which a flat dictionary's 30-symbol alphabet and shared prefixes beat.
  The mutants (`.scratch/mutants.log`, `tools/mutants_fields.py`): A to L all die after three tests were added (buffer varint, reference by id, host length).

- 2026-10-28 (final): Release, ASan+UBSan and TSan builds each in a directory of their own, ctest 47 of 47 on every one. The same code on CI: `prospect:8588afe`, 8 of 8 jobs
  (freebsd-x86_64, linux-arm64-gcc, linux-x86_64-clang, linux-x86_64-gcc, macos-x86_64, macos-arm64, linux-tsan, linux-asan-ubsan); `strata:32b6800` green on all jobs.
  The strata-oracle branch `phase8/rank-terms` (`strata-oracle:8e6611a`, local, not pushed) holds the `rank-terms` mode and the recorded T3b vectors.

## Security round (2026-10-28, `docs/reviews/fields-security.md`)

- SR1 fixed: `fielddict.hh` `dictionary()` takes the verifier and maps `termdict::corrupt` / FST `invalid_argument` to `corrupt_segment`. `fielddict_media_test`: 129 bit flips in the `terms` section of a segment with two large own dictionaries; `scan_terms`, a `term_cursor` walk and a merge each throw `corrupt_segment` on all 129. Mutant R (verifier dropped) dies.
- SR2: `test/fielddict_oracle.hh` (the reviewer's oracle, as a header), `fielddict_oracle_test` (300 valid dictionaries, a share of them wide: 17 to 76 own dictionaries; 6 dictionaries with every directory byte set to 8 values), and the same oracle in `fuzz_fielddict` (unmutated inputs against the reference; mutated ones that `check()` accepts must read the same by every path, unless a termdict byte changed). Mutants N to Q (the reviewer's four) and R to U die (`tools/mutants_fields.py`).
- Proof: Release and ASan+UBSan ctest 49 of 49 (dirs of their own, deleted). `fuzz_fielddict` with the oracle: 23,720 executions in 6 minutes, no finding (it first aborted on a blocks-only mutation: a changed term in an unchecksummed block is not the directory's, so the agreement rule now applies to directory mutations only). The fuzz target built against each of mutants N to Q aborts within 150 s with the oracle's violation.
- SR3 fixed in the reader's constructor (`pool_index_.empty() != (terms.size() == own_blocks_)`); `fielddict_test` `pool_pairing_tests`; mutant S dies.
- SR5: `reader::check_fields(table_size, key_field)` and its test (mutants T, U die). **Not wired**: see Blocked on.
- SR4 hand-off to the termdict library (not changed here): require strictly increasing alphabet symbols (a repeated symbol makes the cursor decode two keys to the same bytes, so a pooled key's field id can read as another field), and cheaply cross-check the FST against the blocks (the floor of a block's first key lands on that block). Evidence: `.scratch/f2-sec/oracle-damage.log` (24,926 accepted dictionaries whose lookups disagree with the cursor, all in the alphabet or FST bytes). A media fault is covered by the section checksum; a resealed file is outside the threat model.

## Next

1. Landed: `strata:5e6af07` (F2 on strata main); prospect pins it.
2. F9 (per-field min/max through merges, per-field document counts at merge, live statistics) and D4's norms: not built; the directory's flag bits are reserved and refused.
3. F10 / T6: the 49- and 2,100-field protected-pages open, and strata `bench/idle_index`.
4. T1's lead measurement on `main` (the sizes above are functional counts of bytes, not timings); sparse-kw misses its target by 0.018 and 0.004: an owner/architect call whether D3's threshold or D10's id width should move.
5. T4 (Lucene parity with norms) and T7 wait for D4 and SF.

## Blocked on

SR5's call site. The segment's open context (`strata:32b6800` `core/reader.hh` `open_context`), the merge context and `check_segment` carry no field table, so prospect cannot compare a dictionary's ids with the table where a segment is opened. Options for the owner/architect: (1) `open_context` (and the merge and check contexts) carry the `field_table`, prospect calls `check_fields` in `inverted_plugin::reader`'s constructor or in `inverted_plugin::check` (an O(pool) pass, so check only); (2) strata's `index::check` calls a plugin hook with the table. Needs a strata change; the threat is a resealed file only.
