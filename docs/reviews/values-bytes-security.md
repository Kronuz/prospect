> Status: CURRENT (2026-10-27) decoder-safety review of `phase4/values-bytes` (prospect:b9324f6) on `phase4/values-rank` (strata:b644708).

# Security review: values plugin P1 (decoder safety)

Reviewer: security reviewer, fresh context. Subject: the `values` section reader, merge and
writer (`include/prospect/values.hh`, `values_writer.hh`, the `encode_present` split in
`docvalues.hh`), the lazy-open `values_detail::lazy`, and `fuzz/fuzz_values.cc`. Budget ~120k
tokens; spent inside it.

## Verdict: APPROVE WITH CONDITIONS

No blocker. Over about 9.1 million fuzz executions, 66,864 single-bit media faults, and about
24,000 structural mutations (all under ASan+UBSan, the concurrency probe also under TSan), the
values reader never read out of bounds, looped, hit UB, allocated without bound, or returned wrong
data after a media fault. Every damage found throws `strata::corrupt_segment`. Conditions below
(SR1, SR2, the fuzz hand-off) are about stated limits, not memory safety.

Threat model used (the lead's): trusted cluster; a segment file may be damaged on disk (media
faults) or truncated; an open must never read out of bounds, loop, allocate without bound, or
return wrong data silently. Attacker tags below: "media fault" (random damage, checksums not
repaired), "resealed file" (someone with local file access who recomputes checksums; outside the
stated model, probed because the fuzz target reseals), "valid writer" (a client whose documents
reach the writer through stage 1).

## What was run

All probes are in `.scratch/p1-sec/` (sources and scripts kept; build trees and binaries deleted).
Build: Homebrew LLVM 23.1.2, `-fsanitize=address,undefined -fno-sanitize-recover=all -O2`, strata
from a local clone at b644708 (`.scratch/p1-sec/strata-src`), `configure.sh` and `buildprobe.sh`
rebuild it. A TSan tree (`configure-tsan.sh`, `buildprobe-tsan.sh`) built only the probe's libraries.
`ASAN_OPTIONS=max_allocation_size_mb=3000:allocator_may_return_null=0` so a huge allocation fails
loudly.

| Probe | File | What it does | Result |
|---|---|---|---|
| directory | `probe.cc` mode `dir` | Every directory field (slot, kind, flags, reserved, documents, offset, size) of the first, second, middle and last entries set to 0, 1, 2, 3, 2^31-1, 2^31, 2^32-1, 2^32, 2^63-1, 2^63, 2^64-1, 2^64-2; the count to 12 boundary values; duplicate slot, swapped entries, reserved slot first, overlapping columns; every truncation of the section by 0..300 bytes then every 61st, prefixes of 0, 1, 3, 4, 5, 31, 32, 35..., leading cuts, appended garbage, zero gaps before the count, all-zero and all-0xFF sections. Checksums recomputed and the file laid out canonically (`relayout.hh`, so the strata core accepts the shape and the values reader sees the bytes) | 11,821 corrupt_segment, 117 identity, 23 accepted; the 23 are slot numbers rewritten to another strictly increasing value (no redundancy to detect; SR5). 0 other exceptions, 0 slow (worst 0.33 s), 0 crashes |
| column headers | mode `hdr` | The first 96 and last 64 bytes of a sample of columns in all three bases set to 0, 1, 2, 0x7F, 0x80, 0xFF, resealed | 11,803 corrupt_segment, 1,257 accepted (dictionary bytes, offsets that stay monotone, packed value bits: SR5), 0 other, worst 0.72 s |
| exhaustive | mode `ex0` | Every byte of the 21,756-byte section of base 0 set to 0, 0xFF, +1, -1, ^0x80, resealed (104,685 files) | 35,821 corrupt_segment, 68,864 accepted, 0 other, 0 crashes, worst 0.027 s |
| media faults | mode `crc` | One bit (0 or 7) flipped in every byte of base 0's section and every 7th byte of base 1's, checksums untouched; every slot read | 66,864 flips, 0 undetected, all `corrupt_segment` |
| concurrency | mode `conc` | 8 threads x 4,000 calls (value, get, each_value, slot_count) on a damaged column (checksum), a damaged directory, a resealed column with a bad kind, and an undamaged control; ASan and TSan | damaged: 32,000 of 32,000 calls threw `corrupt_segment`, 0 returned, 0 other exceptions; control: 0 threw; TSan: no report |
| merge | mode `merge` | Merge of two segments with the second damaged four ways (column checksum, directory kind, column kind, control) | the three damaged merges throw `corrupt_segment` with the name of the failing check; control merges |
| caps | mode `cap` | One document of 40 MiB + (40 MiB + 1) in two slots through the builder, opened, checked, read, merged | accepted at every step (SR1) |
| resources | mode `res` | 100,000 empty-value slots on one document; a slot on 2 of 3,000,000 documents | SR3, SR4 |
| fuzz (shipped) | `build/fuzz_values`, 2 runs, 3,300 own seeds aimed at directory fields, count, column starts and ends, plus the 275 checked-in inputs | 4,260 + 3,579 runs in about 10 minutes each (about 6 runs/s: the target builds and re-checksums a whole file per input), 0 findings |
| fuzz (section-wholesale) | `fuzz2.cc` (+ `relayout.hh`) | The input is the whole values section, any length, laid out into one of three bases with checksums computed; 4 runs, different seeds, `-max_len` 400 to 3000, one with value profile | 2,589,774 + 1,996,547 + 2,387,757 + 2,116,667 = 9,090,745 runs in 30 CPU-minutes (about 5,000 runs/s), 0 findings |
| mutants of the probes | `mutants.sh`, `mutants-tsan.sh`, `mut/` | Seed a bug in a copy of `values.hh` and see that the probe finds it | below |

Mutants (each of the probe's own teeth, run on a copy in `.scratch/p1-sec/mut/`):

1. Drop the directory count bound (`n > (size - 4) / 32`): `fuzz2` finds an ASan allocation-size-too-big within 90 s.
2. Drop the column size bound and the leftover-bytes check: `fuzz2` finds UBSan undefined behavior in `docvalues.hh:278` within 90 s.
3. Drop only the column size bound: `fuzz2` finds nothing in 470,000 runs. Expected: the leftover-bytes check (`next != directory`) is the second line and catches the same shape; the bound's own job (the u64 wrap of `next`) needs two crafted sizes whose sum wraps past 2^64: not reachable by byte mutation, and no test or fuzz input in the branch exercises it (the developer's `size = ~0` case is caught by the leftover-bytes check too). Low value to chase: the bound is in the code and reads correctly.
4. Remove the lock in `lazy::run`: the TSan probe reports data races and a SEGV.
5. Set the done flag before running the initializer: the TSan probe reports races and a BUS error, and 31,999 of 32,000 reads of the damaged column return instead of throwing.

## Answers to the five asks

1. **Slot directory and column headers on arbitrary bytes: sound.** The directory is read in
   `read_directory` in this order: size >= 4, count bounded by `(size - 4) / 32` before any
   multiplication that could wrap, the directory's region checksum before anything is reserved or
   allocated, then per entry: kind, flags and both reserved fields zero, slot strictly increasing
   and not 0xFFFFFFFF, documents in 1..segment's, offset equal to the running total, size nonzero
   and no larger than what remains (so `next += size` cannot wrap), and finally the columns must
   fill the area exactly. The `cell` array is allocated only after all of that. Counts that imply
   huge allocations were probed (count 0xFFFFFFFF, 2^31, 2^27 and neighbours): all rejected by the
   bound, none allocated. Overlap, duplicates, out-of-order, reserved slot, zero-size and
   all-ones sizes: all `corrupt_segment`. Column headers: `column_view::open` bounds every count
   against the bytes left before multiplying (`need`, block count, group count, dictionary
   `n + 1` offsets), checks every offset and width of every block, the group directory against
   the presence set, and the dictionary offsets and memcmp order, in O(column).
2. **Lazy open: holds, across threads.** A damaged region reads as bad on every later call:
   `verified_regions` stores `bad` per region and returns false every time; the structural
   failures rerun the same deterministic checks. See the concurrency row: 96,000 calls on three
   damage shapes, every one threw `corrupt_segment`. A neighbour sharing a region with the damage
   also fences (region granularity, SR6). The error is `corrupt_segment` in every probe; the only
   other type reachable is `std::out_of_range` from `reader::at(i)` with i past the slot count (a
   caller bug, not damage).
3. **64 MiB caps on read: not enforced (SR1).** Writer side only.
4. **Merge on a damaged input column: fails cleanly.** The merge throws `corrupt_segment` on the
   first read of the damaged slot; the sink holds a partial output (96 bytes in the probe), which
   is the caller's to discard. strata's merge driver catches it and calls `mark_damaged`, which
   runs `verify_all` on each source (the values reader's `verify_all` verifies the whole section)
   and flags the damaged one (`strata/core/index.hh:1453`); a merge whose source verifies yet
   fails to decode fences the index. Neither loops.
5. **`lazy` (mutex plus flag): no race, no deadlock.** Fast path acquire-loads `done_`; the slow
   path takes the mutex, re-checks, runs the initializer, and release-stores `done_` only after it
   returns, so a throw leaves it unset and releases the mutex through `lock_guard`; the next
   caller retries and gets the same error. Lock order: `entries()` runs and releases the
   directory's lock before a cell's lock is taken, and `open_column` never calls `entries()`
   under a cell lock, so there is no cycle. Evidence: the TSan row, plus mutants 4 and 5 (the probe
   catches both a missing lock and an early flag). The Linux `pthread_once` hang the developer
   found cannot occur here because there is no `call_once`; I could not reproduce that hang on
   macOS either (as the ledger says).

## Findings

IDs are stable across re-checks.

### SR1 (low): the 64 MiB caps are enforced when writing, not when reading

Attacker: a resealed file (local file access, or any producer that is not this writer); the
ledger and brief say a value and a document's values are "capped at 64 MiB".

Probe: `probe cap` builds one document with 40 MiB in slot 1 and 40 MiB + 1 in slot 2 (83,886,081
bytes, above the 64 MiB document cap) through `values_builder::add_value`, then opens it, runs
`check_segment`, reads every value and merges it: all succeed. By inspection: `values_builder::add_value`
checks each value against the cap but not a document's total (only stage 1, `values_plugin::validate`,
does); `reader::open_column`, `column_view::open` (dictionary term lengths) and `values_plugin::check`
check neither. Merge copies whatever it reads. A value over the cap would also pass (the probe
shows that the builder refuses cap + 1, but a resealed file need not come from the builder).

Gain: none in memory safety (reads are views; the merge's output is the size of its input). The
risk is a downstream consumer (the seam, Xapiand's per-document buffers) that sizes working memory
from the documented cap. For comparison, `stored`'s reader does check frame sizes against
`max_stored_frame`.

Fix, one of: (a) check each term's length against `max_value_bytes` while `parse_dictionary` already
walks the offsets (free), and add the per-document total to `check()`; or (b) state in
`docs/mechanisms/values.md` that the caps are writer-side and rely on the region checksums on read.
Condition: do (a) or (b) before the seam depends on the cap.

### SR2 (low): presence iteration costs the span, so a merge or check of sparse slots is O(slots x documents)

Attacker: a valid writer (a client that writes sparse slots into large segments), or just a workload.

Probe: `probe res`: a slot on 2 of 3,000,000 documents takes 0.226 s per iteration at ASan -O2
(75 ns per ordinal tested; about 10 ns in Release is likely but not measured). The merge iterates
every slot of every input with `cursor`, and `check()` does too, so 1,000 such slots in a 10M
document segment cost about 750 s under ASan, 100 s likely in Release. Not a hang in the strict
sense, but a merge that cannot keep up. The ledger (decision 8) already names the cure
(`next(doc)` on docset's `frozen_view`). Non-blocking; do it before geo or other sparse slots are
routed here.

### SR3 (low): no cap on the number of values per document or slots per segment

Attacker: a valid writer.

Probe: `probe res`: one document with 100,000 empty-value slots passes stage 1 (the cap is in
bytes, and an empty value costs none), flushes in 0.65 s, and the section is 14.8 MB (148 bytes
per slot). At first use the reader allocates one `cell` (about 330 bytes with `sizeof(column)` =
256, a mutex and flag) plus a 24-byte entry per slot: about 2.4x the section. `reader::each_value`
opens every column. The buffered-document accounting in `bytes(p)` charges `sizeof(slot_value)`
per value, but a document with ten million empty slots (1.5 GB section, 3.5 GB of reader state) is
accepted. The memory notes say a cap names its mechanism and is at least Lucene's, Tantivy's and
Xapian's: Xapian bounds values by memory only, so no cap is also defensible, but then say so.
Condition before the seam: name the mechanism (the memory budget) or cap the count (65,536 per
document is above any real schema).

### SR4 (info): the section writer's transient vectors are not charged

By inspection of `section_writer::column`: `terms` (16 bytes per value), `present` (4), `starts` (8),
`flat` (8), and the dictionary string exist at once with the encoded bytes; only the final `bytes`
(during the write) and the merge's `found` pairs are charged to the flush budget. A slot with 10M
values holds about 360 MB uncharged. `flush_charge` never fails a flush ("a flush cannot fail for
memory"), so the effect is under-reported pressure, not a crash. Not probed.

### SR5 (info, outside the model): payload bytes and slot numbers have no redundancy

Probe: `ex0`, `hdr`, `dir`. With checksums repaired, 68,864 of 104,685 single-byte changes to base
0's section are accepted and return different data (a changed dictionary byte that keeps order,
a packed value bit that still indexes the dictionary, a monotone offset shift, a slot number
renumbered), and 23 of 11,961 directory mutations are accepted (slot numbers). Nothing crashes
and every structural impossibility is caught; this is the expected limit of structural checks. The
media-fault guarantee rests on the region checksums, and the `crc` probe shows it holds: 66,864 of
66,864 unrepaired flips were detected at first use of the slot (directory flips by `slot_count`).

### SR6 (info): checksum granularity and no re-verification

A damaged byte fences every column that shares its CRC region, and the directory when the last
column and the directory share the final region, so one bad byte can fail more slots than it
touched (conservative, not a safety issue). Regions are verified once, at first use; damage that
happens after the first verification of a mapped file is not seen until the file is reopened or
`verify_all` runs. That is the design's trade (open reads nothing).

## Non-blocking notes

- `fuzz_values` runs about 6 inputs/s (it re-checksums a whole file per input), so its 10-minute
  run is about 7,000 inputs. `fuzz2.cc` (the section as the input, any length, laid out by
  `relayout.hh`) does about 5,000/s and also reaches truncation and extension, which in-place byte
  changes cannot. Recommend adding it to `fuzz/` (and a corpus of its minimized inputs) and running
  the one-CPU-hour on it. Both sources are in `.scratch/p1-sec/`; they include `fuzz/fuzz_values.cc`
  for its bases and `exercise`.
- `value_at` throws `std::out_of_range` (not `corrupt_segment`) for an index past the values; the
  cursor cannot reach it on a validated column (presence cardinality equals value count at open),
  but `cursor::value()` before the first `next()` returns the first value instead of failing.
  Caller misuse, no memory error.
- The fuzz target's oracle can catch a crash or a bad answer's shape but not a wrong value (SR5);
  the mutation `slot order check removed` would pass it. The developer's directory tests cover it.
- `segment_test` was not run to completion (its lifecycle cases run for many minutes under ASan);
  `values_test` passes under ASan+UBSan (25 s) and `fuzz_values_replay` replays the 275 inputs.

## Re-check

Nothing to re-check yet. A re-check starts from `.scratch/p1-sec/`: `configure.sh`, `buildprobe.sh`,
`probe.cc` (modes `dir`, `hdr`, `ex0`, `crc`, `conc`, `merge`, `cap`, `res`, `seeds`, `seeds2`),
`fuzz2.cc`, `relayout.hh`, `mutants.sh`, `mutants-tsan.sh`, corpora `corpus3` and `corpus3b`.

## Verdict, blockers, conditions

- **Verdict: APPROVE WITH CONDITIONS.**
- **Blocking items: none.**
- **Conditions:**
  1. SR1: enforce the caps on read (dictionary term length at column open, document total in `check`) or document that they are writer-side, before the seam relies on them.
  2. SR3: name the mechanism that bounds values and slots per document (the memory budget) or cap the count, before user-controlled slots reach the plugin.
  3. Run the lead's one-CPU-hour on a section-wholesale target (`fuzz2.cc`), not only `fuzz_values`; the 30 CPU-minutes here found nothing.
  4. SR2: add `next(doc)` to docset's `frozen_view` before sparse slots (geo) use the plugin.
- **Not run:** Linux (the TSan hang the ledger describes), FreeBSD, arm64, a TSan run of the full `values_test`, `segment_test` to completion, `docvalues_test` (the numeric kinds the `encode_present` refactor touches; the ledger shows the fixtures unchanged), a Release-mode timing of SR2, the one-CPU-hour fuzz, and an end-to-end index merge against a damaged on-disk segment (read from `strata/core/index.hh`, not run).
