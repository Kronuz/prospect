> Status: CURRENT (2026-10-28) decoder-safety review of `phase8/fields` (prospect head 194e125, code of 8588afe) on `phase8/fields-f2` (strata:32b6800).

# Security review: F2 field dictionaries (decoder safety)

Reviewer: security reviewer, fresh context. Subject: `include/prospect/fielddict.hh` (D10 varint, builder, reader, sampled directory, cursor, `add_plan`, `check`), its callers in `reader.hh` and `merge.hh`, and `fuzz/fuzz_fielddict.cc`. The strata side (32b6800) is plumbing for stage 1 (`field_resolution::key`, the version bump) and adds no decoder; it was read, not probed. Budget ~100k tokens; this review went past it (about 220k) because the first finding needed a proof of attribution and the fuzz oracle needed its own harness.

## Verdict: REJECT until SR1 closes

One blocker, SR1: F2's cursor reads an own dictionary's blocks without the checksum verifier that prospect main's cursor used. A bit flipped in an own dictionary's region is read as data by `scan_terms`, `term_cursor`, the merge and the reorder walk. A merge then writes the damage into a new segment with fresh checksums (silent wrong data), or its builder throws a non-corrupt exception and the index is fenced. The fix is about six lines and does not change a byte of the format; `.scratch/f2-sec/sr1-fix-proof.patch` is a working version and turns every probe row below to `corrupt`. The directory, the sampled lookups, the pool's varint and the cursor's field boundaries are otherwise sound: over roughly 76,000 single-byte directory mutations, every truncation, 3,000 random dictionaries compared with a reference model, 4.4 million varint cases and an 8-thread lazy open (ASan and TSan), nothing read out of bounds, looped, allocated without bound, or escaped as a non-`corrupt_segment` exception. The fuzz target cannot see silent wrong data (SR2), which is why SR1 survived 57k + 52k executions.

Threat model used (the lead's): trusted cluster; segment bytes may be damaged on disk or truncated; an open must never read out of bounds, loop, allocate without bound or return wrong data silently. Attacker tags: "media fault" (bit rot or a torn write, checksums not repaired: in model), "resealed file" (someone with local write access who recomputes the checksums; outside the model, probed because the fuzz target reseals), "valid writer" (a client whose documents reach stage 1). The `term_index` section is pinned and checked at open, so every directory, FST and alphabet byte is covered against a media fault; the `terms` section is checked per 4 KiB region on first read.

## What was run

Build: Homebrew LLVM 23.1.2, `-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -g`, deps fetched fresh into the review's own tree (`.scratch/f2-sec/configure.sh`; a TSan tree with `configure-tsan.sh` for the probe's libraries). Build trees and binaries are deleted; sources and logs are kept. `ASAN_OPTIONS=detect_leaks=0`.

All files are in `.scratch/f2-sec/`:

| File | What it is |
|---|---|
| `configure.sh`, `buildprobe.sh`, `configure-tsan.sh`, `buildprobe-tsan.sh` | Rebuild: configure, then `buildprobe.sh SRC OUT` (with `PRE_INC=dir` to put a mutant header first). |
| `probe.cc` | Segment-level probes: `media` (one bit flipped in `terms`, checksums not repaired; per flip four fresh opens: point lookups, `scan_terms`, merge with a clean second segment, `check_segment`), `media-ti`, `resealed`, `threads`, `damaged-threads`. |
| `oracle.cc` | The reference-oracle harness: `valid` (a `std::map` model against every read path), `damage` (index/blocks mutated; when `check()` accepts, cursor, scan, `find_run`, `entry_of` and `add_plan` must agree), `repro`. Logs: `oracle-valid.log`, `oracle-damage.log`. |
| `varint.cc` | `get_field` / `put_field` against an independent reference decoder. |
| `poollost.cc` | SR3's repro. |
| `mutants.py`, `runmut.sh`, `runmut.log`, `mut/` | Four seeded decoder bugs, each run against the shipped fuzz target (150 s) and the oracle. `mut/fix1/` is SR1's fix; `sr1-fix-proof.patch` its diff. |
| `fz-baseline.log`, `fz-corpus/`, `fz-art/` | The shipped `fuzz_fielddict`, 421 s, from its 751-file corpus. |

| Probe | Result |
|---|---|
| Shipped `fuzz_fielddict`, 421 s | 52,355 executions, no crash, no timeout (as the ledger: 57,321). It never finds SR1: see SR2. |
| `probe media` stride 3 (11,016 flips in the 33 KiB `terms` section, own dictionaries of 3,000 and 2,500 terms, six pooled fields, checksums untouched) | `check_segment` 11,016 corrupt; point lookups 11,016 corrupt; **`scan_terms` 1,692 DIFFERENT (silent), 5,370 corrupt, 3,954 identical (flip in padding or undecoded bits); merge 385 DIFFERENT (damage written into a new valid segment), 1,810 non-corrupt exception, 5,539 corrupt, 3,282 identical.** SR1. |
| Same with `mut/fix1` (stride 211) | 157 of 157 flips corrupt on every operation. |
| `probe media-ti` (121 flips of `term_index`) | 121 of 121 corrupt on every operation: the directory is covered against media faults. |
| `oracle valid` (300 configurations; then 2,500 with the wide generator: 17 to 76 own dictionaries so the directory has several samples, pooled fields between them) | 0 violations. Field ids at every varint width boundary, 0xFFFFFFFF, term sets of 1 to 700, 255/256/257, empty terms, full-byte alphabets. |
| `oracle damage` (20 configurations; exhaustive single-byte sets over the first 400 bytes of the index, ten values each, plus +1/-1; every truncation of index and blocks; 400 random 1 to 3 byte changes each) | About 76,000 mutated dictionaries: 63,349 fenced, 12,896 accepted by `check()`; 0 crashes, 0 hangs (slowest under 2 s), 0 exceptions other than `corrupt_segment`. Accepted dictionaries whose read paths disagree with each other: **0 from the header, samples and entries**; all from the termdict alphabet and FST bytes (SR4) or from truncations (SR3). |
| `varint` | 4,425,088 inputs (every lead byte with every second byte and sampled tails, truncations, 2,000,000 random): `get_field` agrees with an independent decoder on all; every accepted form re-encodes to its own bytes; order and prefix freedom hold on 20,000 random ids. |
| `probe threads`, `damaged-threads` (8 threads, 400 mixed lookups/scans/cursors each, lazy opens racing) | ASan+UBSan and TSan: 3,200 of 3,200 correct; damaged: every call corrupt or exact; no TSan report. |
| Mutants vs the shipped fuzz target and vs the oracle | See SR2. |

## Findings

### SR1 (high, blocking): an own dictionary's blocks are read unchecked by every cursor

Attacker: media fault (or anyone who changes bytes of a segment file without recomputing the section CRCs). Gain: silent wrong data in `scan_terms`; the merge launders the damage into a new segment whose checksums are valid, so no later check can find it; or one flipped bit fences the whole index.

`fielddict::reader::dictionary(entry)` (fielddict.hh:614) builds the termdict reader for the cursor with `termdict::reader(blocks, index)`: no verifier. The lookups (`find_run`, `scan`) go through `open_at`, which passes `verify_in`, and the pool's cursor uses `pool()`, which does too; the own-dictionary cursor path alone does not. Prospect main's cursor was one termdict reader with the verifier (`git show main:include/prospect/reader.hh`, line 285), so this is a regression of F2. `inverted_plugin::reader::scan_terms` and `term_cursor` (reader.hh:375-417) reach it, and through them the merge (merge.hh:33), the reorder walk (reorder.hh:104) and `check`'s term walk. The merge's `add_plan` reads only the own dictionaries' alphabets, so nothing else in a merge touches their blocks.

It is easy to miss because the pool is read first: while the whole `terms` section fits one 4 KiB region (`default_region_shift` 12), the pool's verified read covers the own blocks too. Past 4 KiB of `terms` the own dictionary sits in a region the cursor never verifies. A segment with a 256-term field is a few KiB, so every real segment is past it.

Probe: `probe media` (table above). The two bad outcomes:

1. `scan_terms` returns a different term list without throwing (1,692 of 11,016 flips); the merge of the same segment succeeds with wrong terms (385). The merged segment is internally consistent and checksummed.
2. The merge's builder throws `std::invalid_argument` ("strata: terms must increase", 1,810 flips). Strata's merge handling (`index.hh:3082-3095`) catches `corrupt_segment` and quarantines the source with `mark_damaged`; any other exception reaches `fence("strata: merge failure: ...")`, which sets `fenced_` and throws `index_fenced`. One flipped bit in one own dictionary can therefore make the index refuse service, where a verified read would have marked the segment damaged for repair.

Fix: give the cursor's dictionaries the same verifier as `open_at` (the cursor holds one `dict_context` per open dictionary; `.scratch/f2-sec/sr1-fix-proof.patch`, 57 lines of diff, applies to `mut/fix1/prospect/fielddict.hh`). With it `probe media` is 157 of 157 corrupt on all four operations. Also have `dictionary()` go through the same `try/catch` as `open_at` (it lets `std::invalid_argument` from the FST open escape; the cursor's `try` catches only `termdict::corrupt`; the oracle never provoked it, so this is hardening).

Required test: a media-fault test in `fields_test` or `segment_test`: a segment with two own dictionaries and `terms` past 4 KiB, one bit flipped in the first dictionary's region (not the last), checksums untouched; `scan_terms`, a `term_cursor` walk and a merge must all throw `corrupt_segment`. `probe.cc` `media` is the model; revert the fix and it must fail.

### SR2 (medium, condition): `fuzz_fielddict` cannot catch silent wrong data

The target asserts "no crash and no OOB", plus "if the cursor read out of order, `check()` must throw". It compares no read with any expected value, so a decoder that returns the wrong field's terms, loses a field or misfiles a term passes it. The ledger's 57k executions and my 52k found no issue while SR1 stood, which is the proof that the oracle is blind to the rule the lead named ("a term filed under the wrong field is silent wrong data"). Its media-fault mode (one input in eight leaves checksums unrepaired) runs `exercise()` on the dictionary, which has no expected value either.

Seeded bugs (`mutants.py`, copies of `fielddict.hh`; `runmut.log`), each against the shipped fuzz target for 150 s from its corpus and against `oracle valid`:

| Seeded bug | Shipped fuzz target | Oracle |
|---|---|---|
| m1: pooled `scan` no longer stops at the next field (terms of the neighbour field leak) | found, by accident: the leaked key is shorter than the slice and `substr` throws `out_of_range` (crash artifact in `mut/m1_scan_leak/art/`) | killed (3 classes) |
| m2: sample binary search `<` for `<=` (a field whose id equals a sample's is lost) | not found, 17,356 executions | killed, but only after the generator gained 17+ own dictionaries; 0 violations on 300 narrow configurations (so a harness needs a wide case) |
| m3: pooled `scan` returns terms with the wrong slice for ids of 240 and up | not found, 16,904 executions | killed |
| m4: pooled lookup under the id `field OR 1` (field 2 finds field 3's terms) | not found, 19,454 executions | killed (3 classes, 127k hits) |

The harness for the field-table review's model, `oracle.cc`, is the fix: the shipped `fielddict_test` has per-feature tests, but nothing compares every path with a model over varied shapes. Condition: land `oracle valid` (or its comparisons folded into `fuzz_fielddict`'s mode 1, with a wide-directory base and the cross-field absent probes) as a test, and keep mutants m1 to m4 in `tools/mutants_fields.py` with the tests that must fail. The oracle's `damage` mode's rule (when `check()` accepts, the read paths agree with each other) is worth keeping in the fuzz target too: it found SR3 and SR4.

### SR3 (low): a directory with no pool index but pool blocks is accepted (resealed)

Attacker: resealed file (a truncated `term_index` with a good CRC). `has_pool()` is `!pool_index_.empty()` (fielddict.hh:300) and `check()` does not compare it with `terms.size() > own_blocks_`. `poollost.cc`: nine pooled terms, index cut to its 4-byte header; the reader opens, `check()` accepts, the cursor sees 0 of 9 terms. Every pooled term vanishes silently. A media fault cannot do this (the section CRC), so it is not a rule violation in the model; the check is one comparison in the constructor: `has_pool() == (terms.size() > own_blocks_)`.

### SR4 (low, termdict's): the dictionary's alphabet and FST are not cross-checked against its blocks (resealed)

Attacker: resealed file. After `oracle damage`, 24,926 accepted dictionaries have lookups that disagree with the cursor, 3,549 `has_field` false for a field the cursor yields, 4,039 `scan(field)` that differ, all in the alphabet and FST bytes (`own-index`, `pool-index` in `oracle-damage.log`), none in the directory. The worst is an alphabet with a repeated symbol: the cursor decodes two different keys to the same bytes, so a pooled key's field id can read as another field. This is termdict's validation (`alphabet::parse`, the FST pass), the same as before F2, but F2 raises the stakes because the pool's keys now carry the field. A media fault is covered (pinned `term_index` CRC). Hand-off to termdict: require strictly increasing alphabet symbols, and consider a cheap per-block check that the FST's floor for a block's first key lands on that block.

### SR5 (low, informational): field ids are not validated against the field table below the index

A pooled key's id and a directory entry's id delta are bare numbers; `oracle damage` shows 182 + 340 accepted dictionaries whose fields were relabelled (an entry's delta changed), and nothing in `check()` or the merge compares ids with the table, so an id beyond the table's size would be carried through merges, and would read as that field's terms if the append-only table later grows to it. This needs a resealed file; a media fault is caught by the CRC. Not probed at the index level (`ix.check()` was not exercised against a damaged segment with an out-of-range id). Suggest: the index-level check refuses any id at or above the table's size and any term of the key field (D7: the key field holds no postings).

## Answers to the asks

1. **Field directory and pooled keys on arbitrary, truncated and adversarial bytes: sound, no OOB, loop or allocation.** The reader's constructor bounds the count, the sample count and the section sizes before any multiplication (`entry_count_ > end - p`, `samples > (end - p) / 20`), refuses `entry_count == 0` with entry bytes, and every LEB comes from `get_leb`, which throws on truncation. `walk_group` bounds a group by `min(16, entry_count - first)`, checks the sample's entry offset against the entry bytes, each entry's blocks and index against the section sizes, rejects a zero or overflowing delta and any flag (flags are refused until D4). `check()` verifies the sum of sizes, ascending ids, every sample (id, entry offset, blocks offset, index offset), term counts per field and ascending (field, term) order. Mutated header, sample and entry bytes (ids out of range or out of order, duplicate fields, huge offsets and lengths, samples pointing past the dictionary): all fenced or consistent. The one disagreement class from the directory is the relabelling of SR5.
2. **Pool field-id varint:** `get_field` rejects 0xF5..0xFF leads, truncation, a long form below 240 and a leading zero byte; 4.4 million cases against an independent decoder, 0 mismatches. A pooled key whose id is not a valid varint throws `corrupt` from the cursor, `scan` and `add_plan` (each calls `get_field` and checks it). Empty terms: valid dictionaries with the empty term in the oracle (a third of the fields), lookups, scans from empty, and mutated; no finding.
3. **Cursor across field boundaries:** the own-versus-pool interleave is by field id; a field present in both throws `corrupt` the moment both are live. `oracle valid` compares the cursor with the model, including 17 to 76 own dictionaries between pooled fields. The cursor's own-field order is trusted from the directory (ascending ids checked by `check()` and per group by `walk_group`, not across a group boundary on lookup); the fuzz and the oracle both exercise a deliberately misordered directory only through `check()`'s refusal.
4. **Lazy open across threads:** `open_at` holds one mutex over the map lookup, construction and FST validation; map nodes never move, so the returned reference and the verifier context stay valid; a failed open erases the node and rethrows (each retry re-validates, a cost, not a hazard). 8 threads x 400 mixed operations under ASan/UBSan and TSan: no report, no wrong data. TSan was run on the probe only, not on the ctest matrix (the ledger ran that).
5. **Merge of a damaged input:** SR1. For a damaged directory or pool the merge throws `corrupt_segment` (from `add_plan` and the cursor) and strata quarantines the source; the `invalid_argument`s of the builder ("more terms than planned", "terms must increase") are reachable only through SR1 or through checksum-valid dictionaries whose counts lie; in 845 resealed `term_index` mutations the merge produced 0 non-corrupt exceptions.
6. **Whether the fuzz oracle catches rule violations:** no (SR2).

## Valid writer (a client with credentials)

Stage 1 resolves every reference to the table, refuses a part on the key field and a field named twice, and `r.n >= known` / `r.n >= declared.size()` refuse out-of-range references (strata `fields.hh:86-98`). The key field's id comes from `fields_.key_field()` after the transaction's declarations. A term of any bytes is accepted (0xF0..0xFF leads are fine: the pool decodes the id first and the varint is prefix free), and the writer's plan gives every pooled field's id bytes and terms' bytes to the pool alphabet (`oracle valid` builds all shapes). Not reached by an in-limit client: nothing found. One boundary for the record: `max_term_bytes` is `termdict::max_key_bytes` (about 4 GiB), and a pooled key is the term plus 1 to 5 bytes, so a term within 5 bytes of the limit passes stage 1 and fails at the FST (`invalid_argument` at flush). It needs a 4 GiB term; the practical limit is the host's request size.

## Not blocking

1. `p.terms += e.terms` in `add_plan` (fielddict.hh:480) can wrap for a directory entry with a count near 2^64; `check()` bounds nothing but equality with the cursor, and the merge does not call `check()`. Reachable only by a resealed file; bound it by `terms_.size()` when reading the entry.
2. `walk_group` trusts a sample's `blocks` and `index` offsets only for range, not against the previous group's totals; `check()` does, lookups do not. A resealed sample can point a lookup into another field's dictionary. Same class as SR5.
3. The builder throws `length_error` when the own indexes pass 4 GiB (samples hold a u32 offset); no real segment is near.
4. Documentation says "a damaged directory fences" (`mechanisms/fields.md`): true of every directory byte, and of the dictionaries once SR1 closes; say the termdict index is covered by the section CRC only.

## Verdict, blockers, conditions

Verdict: REJECT until SR1 closes. Not a format problem: no byte of F2's layout needs to change.

Blocking:

1. SR1: pass the termdict verifier to the cursor's own dictionaries (fielddict.hh:614), and add the media-fault test described above (scan_terms, term_cursor walk, merge).

Conditions (for landing, after SR1):

1. SR2: land the reference oracle (a wide-directory case included) and the four seeded bugs as mutants.
2. SR3: `has_pool() == (terms.size() > own_blocks_)` in the constructor, with a test.
3. SR4: hand the alphabet and FST cross-check to termdict.

Re-check recipe: `configure.sh`, `buildprobe.sh probe.cc probe`, `probe media 211` must print 157 corrupt on all four operations; `buildprobe.sh oracle.cc oracle`, `oracle valid 2500 2` must print 0 violation classes.

Not run: the other platforms and the CI matrix; the ctest suite (the developer's ledger ran it under ASan+UBSan and TSan); a TSan run of the full suite (only the probe); a decoder fuzz of the strata side; `ix.check()` and the reorder walk against a damaged segment at the index level (SR1's reach is read from the code, not run for reorder); the F9/F10 items the ledger lists as unbuilt (per-field statistics, norms, T6 open cost); Xapiand-level query trees; any host but this one.
