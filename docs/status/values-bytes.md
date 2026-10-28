> Status: CURRENT (2026-10-27) P1 of xapiand-on-strata: the values plugin. Branch `phase4/values-bytes`, worktree `prospect-wt/values-bytes`.

# phase4/values-bytes ledger

Workstream: prospect values plugin (xapiand-on-strata P1, strata `docs/design/xapiand-on-strata.md`
D3 and the step table; Q2 (a): opaque `bytes` slots, typed later). Developer agent.

## State

Built, proven locally (Release, ASan+UBSan, TSan, mutants, fuzz), pushed. Waiting on CI for the
last push and the decoder-safety review. Strata prerequisite pushed as `strata:b644708`
(`phase4/values-rank`, CI green); prospect pins it.

## Done criteria

From the step row: `bytes` column per slot, slot directory per segment, flush, merge, open, check,
digest record, reader (`value(ord, slot)`, presence iteration), fuzz target, fixtures, pre-freeze
bump. From the lead: decoder safety on arbitrary bytes (fuzz target); a merge keeps every value of
live documents and drops deleted ones; a revert check per bug fixed; sanitizers (ASan+UBSan, TSan);
`docs/mechanisms/values.md`; CI green on every job; a screen A/B that indexing without values is
unchanged.

## Decisions (shapes the design leaves open)

1. **Column layout.** One docvalues `bytes` column per slot (kind `bytes`, single-valued): a sorted
   dictionary of the slot's distinct values (memcmp order, as D3 says) and per-document dictionary
   ordinals, with presence, skip groups and statistics from `docvalues.hh`. No new codec. Typed
   and multi-valued columns reuse the same directory later (kind and flags bytes are reserved and
   must be 3 and 0 today).
2. **Slot directory.** In the section's tail, after the columns: 32 bytes per slot (slot, kind,
   flags, documents with a value, column offset and size), then the slot count. Columns are
   contiguous in slot order, and the directory must account for every byte; a reader checks all
   of that at open (cost O(slots), no column opened). Recommendation taken: the directory is a
   tail like `stored`'s index, not a separate pinned section, so a segment with values has
   one new section and a segment without them (flag set, no values) a 4-byte one.
3. **Structure bit.** `values` (bit 16) is configured by default like the other readable bits, so
   every segment of a default index carries a values section, 4 bytes when empty. An index
   configured without the bit refuses documents with values at the flush (`refuse`), as `stored`.
4. **Reserved slot.** 0xFFFFFFFF is `index_sort`'s key component (`format::key_slot`) and is
   refused. Slot 0 is an ordinary slot.
5. **Empty value is a value.** A document that sets a slot to empty bytes has it present. The
   seam decides whether Xapian's "empty means unset" applies, before it reaches the plugin.
6. **Limits.** A value, and a document's values together, are at most 64 MiB
   (`format::max_stored_record`): the flush's per-document working memory is sized for a stored
   record of that size. Xapian bounds values by memory only; Lucene's binary doc values stop near
   2 GB per value and its sorted ones at 32 KiB, so this is above Lucene's sorted-set limit and
   below its binary one. Revisit if Xapiand's geo or StringList values exceed it (census: values
   are tens of bytes).
7. **Document part.** `values_plugin::part` is a vector of `{slot, bytes}` strictly increasing by
   slot; stage 1 rejects anything else. `part::set(slot, bytes)` keeps the order and replaces
   (Xapian `add_value`).
8. **Presence iteration.** `column::cursor` walks documents with a value in ordinal order with
   `next()` and `advance(target)`, over the skip groups and the presence set. docset's
   `frozen_view` has `contains` and `rank` but no select or next, so the cursor tests ordinals
   one by one inside a group's span: a very sparse slot in a very large segment costs the span.
   A `next(doc)` on `frozen_view` would remove it (docset follow-up, not done here).
9. **Merge.** Rebuilds a slot at a time from the inputs' columns (live values keyed by new
   ordinal, sorted) and streams the section; it does not copy a column verbatim. A copy path for
   a single source without deletes is a later optimization.
10. **Core change (strata).** `values` had no file rank (`unranked`), so a streamed values section
    was refused. Strata branch `phase4/values-rank` (from the pinned 75d1e7e) gives it rank 7
    (after `stored`, before `docids`) and bumps `pre_freeze` to 0x8000000B. Prospect repins to
    that commit; the strata branch must merge before this one.

## Done

- `strata:b644708` (branch `phase4/values-rank`, from the pin 75d1e7e): rank 7 for `values`,
  `pre_freeze` 0x8000000B, format 14 fixtures re-pinned; strata ctest 23 of 23 after the
  fixture re-pin.
- `prospect:ca67b32` the plugin: `values.hh` (part, stage 1, reader, check, digest records),
  `values_writer.hh` (builder, merge), `docvalues.hh` `encode_present`, the inverter keeps values
  beside the tuple buffer, `tuple_buffer::flush` takes an `extra` hook.
- `prospect:c592aae` tests, fuzz target, fixtures (`values.sx`, `values-merge.sx`,
  `digest-values.txt`; `digest.txt` unchanged), `format13_test`, `segment_test`, `reorder_test`
  updated for the new structure bit and section.
- `prospect:51236aa` repin strata. `prospect:880a60d` lazy open without `std::call_once`
  (below), `prospect:8a1b148` its regression test; rebased on main 7f1d915.
- Later commits on this branch: the fuzz corpus and the ledger.

## Evidence

Host: Intel Mac, Homebrew LLVM for sanitizers. All counts from this branch's builds.

- **ctest, Release:** 43 of 43 (directory_tmpfs excluded: Linux only). **ASan+UBSan (-O1):** 42 of
  42 before the lazy-open change, 37 of 37 for the non-lifecycle subset after it (own dependency
  tree, `.scratch/build-asan`). **TSan:** values, fixtures, merge, index, concurrency,
  background_merge, fuzz_values_replay clean (own tree, `.scratch/build-tsan`).
- **Digest identity:** `digest.txt` (no values) did not change with the plugin; the values digest
  is the same buffered, flushed and force-merged (`digest-values.txt`).
- **Merge = one flush:** `values_test` compares a merge's values section with the direct flush's
  of the live documents byte for byte (8 seeds, 2,500 operations each, replacements and deletes),
  plus a shuffled order, with no value only deleted documents held left in any dictionary.
- **Bug found and fixed: the open verified a region.** The first reader read and verified its
  directory in the constructor; `segment_test` (open verifies no region) failed on it. The
  directory and columns now open on first use. Revert check: with `(void)entries()` back in the
  constructor `values_test` fails at `verified_regions() == 0` (mutant A).
- **Bug found and fixed: TSan hang.** The first CI run's linux-tsan job hung in
  `fuzz_values_replay` for an hour. Cause (inferred; not reproduced on Linux): `std::call_once`
  retried after a throwing initializer, and TSan's `pthread_once` wrapper does not unwind. The lazy
  open uses a mutex and a flag. `values_test` now reads a damaged column three times. This test
  cannot show the hang on macOS, so the revert check is CI's linux-tsan job on `prospect:8a1b148`
  against `prospect:a2677c6` (see CI below).
- **Mutants** (`.scratch/mutants.py`, Release, `values_test` and `fixtures_test`): A the eager open;
  B merge keeps deleted documents' values; C `advance` ignores its target inside a group; D no
  accounting of the columns' bytes; E the flush adds values at the buffer's ordinal; F buffered
  documents leave no digest record; G a column read without its region checksum; H a slot twice
  per document accepted; I merge numbers by the old ordinal. All nine fail. D and G first
  survived; the directory-only and column-only checks in `values_test` were added for them.
  Under A `fixtures_test` spun for over an hour (no result), killed; a hang counts as caught.
- **Fuzz:** `fuzz_values` (libFuzzer + ASan + UBSan, three bases, byte changes mostly in the values
  section with checksums resealed): 12,228 runs in 10 minutes, 0 findings, corpus of 275 inputs
  checked in and replayed as `fuzz_values_replay` (1.4 s). The first 10-minute run (2,701 runs)
  before the target was lightened also found nothing. Well short of the one CPU-hour the design
  asks of each decoder; the lead schedules the long run (it needs a hold).
- **Screen, indexing without values** (not a measurement of record): 60,000 documents of 40 terms
  through `apply` and `refresh`, base 7f1d915 against this branch, six interleaved rounds under
  `quiet strict`, Intel Mac, Release: base 1.234 1.187 1.169 1.185 1.137 1.270 s (mean 1.197),
  branch 1.174 1.184 1.175 1.201 1.164 1.205 s (mean 1.184). Neutral within the noise.

## CI

- `strata:b644708`: CI green (all jobs).
- `prospect:a2677c6` (first push): all jobs green except linux-tsan, which hit its one-hour limit
  in `fuzz_values_replay` (above). Not yet seen for the later pushes.

## Next

- Read the CI of the latest push; the linux-tsan job is the one to look at.
- Decoder-safety review (security reviewer), then the long fuzz run under a hold.
- Merge `strata:b644708` into strata main before this branch lands, and repin if main moved.
- Perf, for the lead to measure: a verbatim column copy for a single source without deletes; a
  `next(doc)` on docset's `frozen_view` for sparse presence iteration; `value(ord, slot)` cost
  per candidate in the key makers.
- Not in P1 (seam steps): real-time reads of values from the write buffer (the core reads stored
  records from the buffer; values are visible after the flush), typed slots, `declare_slot`.

## Blocked on

Nothing.
