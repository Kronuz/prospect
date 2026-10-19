# strata code review: the inverter's follow-ups (R26 to R32, C11, CR14 to CR20)

- Date: 2026-10-19. Tier: fix (the buffer is in memory, stage 1 takes split text past 4 GiB,
  and nothing on disk changes).
- Reviewed: `phase4/inverter-followups` at `9cb5e1c`, equal to `origin`, in a detached worktree
  (`strata-wt/review-followups`). The scope is `git log origin/main..HEAD`: 11 commits from
  `2b598dd` (`37ee946`, `4b15eeb`, `5db24ad`, `9ce600a`, `7bca610`, `2518aae`, `807dff9`,
  `e9d6f65`, `d195eba`, `8cf61a2`, and `9cb5e1c`). `origin/main` is `69a3f8c`, which adds only
  `docs/reviews/inverter-main.md` to the branch's base.
- Checked against:
  - the architect's re-check on main (`layers-design.md:1208-1543`, R26 to R33, C11);
  - the code review of main (`inverter-main.md` at `69a3f8c`: CR14 to CR20, its C1 to C6, and
    its D10/D17 question);
  - the ledger (`docs/status/inverter-followups.md`, which has no done criteria set),
    `limits.md`, and `calibration.md`'s ten checks.
- Checked by:
  - A Release build (Apple clang 17, `-O3 -DNDEBUG`, macOS x86_64), with no warnings. ctest
    passed 35 of 35 in 76.5 s.
  - ASan+UBSan with CI's flags (Homebrew clang 23, Debug, `-O1`,
    `-fno-sanitize-recover=undefined`, plus libc++'s extensive hardening), `detect_leaks=1`.
    `tuple_buffer_test` (21 s), `index_test` (104 s), `multisegment_test` (116 s), and
    `segment_test` (335 s) pass with no reports.
  - CI: run 39211275577 at `8cf61a2` is green on all eight jobs. Its code equals the head's;
    `9cb5e1c` is docs-only, and CI skips docs-only pushes (`ci.yml:5`). Its ASan+UBSan job
    passed 35 of 35 (`tuple_buffer` 24 s, `index` 83 s, `segment` 255 s), and linux-tsan
    finished green at 07:06Z.
  - Byte identity: `ident.sh`, the head's `strata_engine` against main's (built from `69a3f8c`).
  - Probes, in `.scratch/probes/`, with their outputs in `.scratch/out/`:
    - the architect's `probe_floor` and `probe_preconditions`, unchanged;
    - `probe_floor_index` (the charge through `index::apply`, on main and the head);
    - `probe_scratch` (the one-thread charge around the first full-batch drain, from the
      corpus);
    - `probe_side_wrap`, through `index::apply`, against the head and against
      `.scratch/model`, a copy of the headers whose side index is 12 bits (`max_side` 4,096,
      both casts masked);
    - `deque_alloc`, run with GCC 16's libstdc++ and with libc++.
  - Mutants: `mutate.sh` and `mutants.sh`, m1 to m10, each reverting or weakening one fix.
  - Each commit of the series: `percommit.sh` and `seg_syntax.sh`. Every CI run before the
    head's was cancelled.
  - CR15's cells against Lucene 10.4.0's and tantivy-stacker 0.7.0's sources.
  - Not run: see the last line.

## Verdict

**APPROVE WITH CONDITIONS. Nothing blocks landing.** Every item this branch owns is closed in
code, tests, or text, with one exception: R29's listing of the core's reads (IF3), a ledger line.
The checks asked for reproduce:

- CR18's cap holds, and the 65th page is reached under UBSan.
- CR16 takes 2^32 − 1 words, and its row is at the largest.
- CR14's cap is a test hook.
- R29's key is a view, and both duplicates are gone.
- C11's floor is 1,002 B, from 12,522 B.
- Segments are identical to main's at 20,000 and 100,000 documents wherever flush points are
  equal.

Two things need work after landing:

- The ledger's "related edge" is real, and it is not in the memory-fence class it is filed
  under. One side entry past the index commits a corrupt segment (IF1). It is pre-existing,
  out of a replicated document's reach, and needs about 300 GB resident for one standalone
  document, so it is a condition.
- Flush points moved at both thread counts, and nothing lists the move (IF2).

## The seven checks

| Check | Met | Evidence |
|---|---|---|
| 1. CR18: the shift capped, the 65-page test reaching it | yes | `tuple_detail::doubled` caps the exponent at the first size's leading zeros (`tuple_buffer.hh:109-111`). The byte log (`:120`) and the arena (`:731`) take their sizes from it, and `chunked_spill` already capped its own (`section_io.hh:137`). The test appends 70 pieces of 1 MiB + 1 (`tuple_buffer_test.cc:466-479`). m1 restores the byte log's `4096 << pages_.size()`: under UBSan the test aborts with "shift exponent 64 is too large" at `tuple_buffer.hh:120:73`. Without the sanitizer it passes (x86 masks the count), so CI's ASan job is the gate. m2 (the helper uncapped) fails `tuple_buffer_test.cc:471` in a plain build |
| 2. CR16: split text up to 2^32 − 1 words, and its row | yes, with IF4 and IF5 | Stage 1 refuses a split text only past 2^32 − 1 words or with a word over the term limit (`reader.hh:139-149`, `:556`). In the tuple buffer, a word at byte 2^32 or later takes a side entry (`tuple_buffer.hh:190-192`, `:262-276`). At more than one thread the document stays split, and the writer splits it at flush (`index.hh:1812`, `:2008-2012`; `writer.hh:330-367`, `:887-892`), so no 32-bit `text_token` offset is built. The digest splits it too (`index.hh:711-716`). Row `limits.md:18`: 2^32 − 1 words, mechanism named, at or above Tantivy's and Xapian's 2^32 − 1. Identity at two threads, where the new `add_split` runs, is in check 6 |
| 3. CR14: the run cap is a test hook | yes | `index_options::max_run_bytes` is gone. The cap is the private `max_run_bytes_` (`index.hh:2447`), copied into each new buffer (`:1866`) and lowered only by `index_test_access` (`index_test.cc:14-19`, a friend at `index.hh:840`). `drain` asserts it is at most `run_cap` (`tuple_buffer.hh:199-200`, `:770`). No public field reaches the marks |
| 4. R29: `key_of` returns a view, the duplicates gone | yes for the key; the listing is open (IF3) | `keys_plugin::key_of` returns `std::optional<std::string_view>` (`reader.hh:581`). Stage 2 keeps a view into the operation, which outlives `invert_pending` (`index.hh:1745`, `:1809`; both run inside `apply`, `:420-433`). `key_term` and `index_detail::key_hash` are deleted, and the version map calls `wordwise::hash` (`index.hh:1662`, `:1677`). The prefix scans left are stage 1's refusals (`reader.hh:314`, `:338`, `:561`). m9 drops the pending key and fails `index_test.cc:181` |
| 5. C11: one buffered document's floor | yes: 12,522 → 1,002 B | The architect's `probe_floor` at the head gives 1,002, 5,194, and 51,571 B charged at 1, 10, and 100 documents. Main gave 12,522, 19,382, and 96,663 (`recheck-inverter-main/.scratch/probe_floor.log`). `sizeof(tuple_buffer)` is 1,312 B, from 7,392. Through `index::apply`, with the version map and the charged object (`index.hh:948-951`), `probe_floor_index` gives 2,898, 7,602, and 61,147 B, against main's 13,034, 21,206, and 101,355. At 100 documents of 300 words the head charges more, 1,095,955 B against 1,081,635, since its object and scratch are counted. m6 (spread drops the one bucket) and m7 (the first chunk grows without its copy) both fail `tuple_buffer_test` |
| 6. Byte identity with main at 20k and 100k | yes, at equal flush points | See the table below. Flush points moved where a charge changed (IF2) |
| 7. The "related edge": a lone document past the 32-bit side index | real; not blocking; a condition | IF1 |

Byte identity (`ident.sh`, corpus documents, head against main):

| Run | Segments, main / head | Files |
|---|---|---|
| 20k, 1 thread, `STRATA_BUDGET_MB=2056` | 1 / 1 | all 5 equal |
| 20k, 1 thread, `STRATA_DELETE_EVERY=7` (2,858 deleted) | 1 / 1 | all 5 equal |
| 100k, 1 thread, `STRATA_BUDGET_MB=2056` | 1 / 1 | all 5 equal |
| 100k, 1 thread, 256 MiB share (the default), merges off | 1 / 1 | all 5 equal |
| 20k, 1 thread, 32 MiB share, merges off | 4 / 4 | all 8 equal |
| 20k, 2 threads, 8 GiB budget (`add_split` at flush) | 2 / 2 | all 6 equal |
| 20k, 2 threads, `STRATA_DELETE_EVERY=7` | 2 / 2 | all 6 equal |
| 100k, 2 threads, 8 GiB budget | 2 / 2 | all 6 equal |
| 100k, 1 thread, 64 MiB share, merges off | 4 / 6 | differ: flush points moved (IF2) |
| 20k, 2 threads, 32 MiB share, merges off | 4 / 2 | differ: flush points moved (IF2) |

## Status of the architect's re-check (R26 to R33, C11)

- **R26: closed.** The ledger names `2ec8c36` and `1e2d89b` as bisect skips
  (`inverter-followups.md:10-12`, `inverter.md:16`). The series has no such gap:
  - all 11 commits compile `tuple_buffer_test`, `index_test`, `segment_test`,
    `multisegment_test`, and `strata_engine` (`-fsyntax-only`, `percommit.sh`);
  - CI runs at `2518aae` and `e9d6f65` were cancelled before any job finished. At both,
    `tuple_buffer_test` and `index_test` build and pass with asserts on;
  - the other code commits' cancelled runs each had one to six fast jobs green.
- **R27: closed.** `add_with`'s debug block asserts that each terms entry's positions strictly
  increase (`tuple_buffer.hh:255`) and that terms are unique, by a sorted copy (`:252-259`).
  `probe_preconditions` with asserts on aborts in all three cases:
  - case 0 at `:264`;
  - case 1 (`x` given twice) at `:259`;
  - case 2 (positions 7, 3) at `:255`.
- **R28: closed.**
  1. The run cap is a test hook (check 3).
  2. `valid_words` is private (`reader.hh:549-576`).
  3. `inverted_builder::add_text` is private, reached through `segment_writer` and
     `writer_access` only (`writer.hh:328-367`). No test or tool calls either one directly.
  4. The ledger has one public list, with `flush_charge` and `section_stream`
     (`inverter.md:18-31`).
- **R29: partially closed.** The key half is closed (check 4). The listing half is open (IF3).
- **R30: closed as text.** The pin's comment and message are at `reader.hh:860-862`, and the
  three causes are at `inverter.md:33-38`. Two new causes from this branch are unlisted
  (IF2).
- **R31: closed.** L10's part, both buffers, `doc_bytes` at more than one thread, byte
  identity at equal flush points, and buffered reads are at `layers.md:742-800`. D13 and D16
  are at `phase-4.md:394-421`.
- **R32.1: closed.** `key_hash` is deleted for `wordwise::hash` (`index.hh:21`, `:1662`,
  `:1677`). The hash is in memory only, and the identity runs confirm it.
- **R32.2 to R32.5, R33, and R24.5's `tuple_detail::hash`: open, not in this branch.** They are
  still owed a code-review ruling.
  - `add_text_with` (`writer.hh:344-367`) now serves both text adds, so CR16 added no fifth
    copy. The terms half still repeats `add_terms` (`:163-171`).
  - `file_sink::put` and `write_all` (`index.hh:176-196`) and the `thread_local` scratch
    (`reader.hh:303`) are unchanged.
- **C11: closed for the code.** The floor and the object's charge are in check 5. The
  many-index rows on the gate VM and FreeBSD are the lead's. IF6 lists what stays uncharged
  on libstdc++.

## Status of the code review of main (CR14 to CR20)

- **CR14: closed**, as R28.1 (check 3). With the option gone, there is no public value to
  clamp or refuse, so the test CR14 asked for has nothing to check.
- **CR15: closed.**
  - The Term bytes row drops the 2^32 − 67 clause (`limits.md:15`).
  - The run row's Lucene cell matches Lucene 10.4.0 line for line:
    - `DocumentsWriterFlushControl.java:237-241` sets flush pending after a document;
    - `Math.addExact` is at `ByteBlockPool.java:199` and `IntBlockPool.java:157`;
    - the aborting exception is at `IndexingChain.java:1319-1321`;
    - the tragic event is at `DocumentsWriterPerThread.java:556-566`.
  - The Tantivy cell matches `memory_arena.rs:204` (`assert!(page_id < 4096)`) in
    tantivy-stacker 0.7.0.
  - The mechanism cell (`limits.md:21`) and the comments (`tuple_buffer.hh:196-198`,
    `:309-311`) credit the cut, not the early drain.
  - IF1 makes the cell's "bounded only by the position, wdf, length, and split-text rows" false
    at 2^32 side entries.
- **CR16: closed** (check 2). The row is at `limits.md:18`. IF4 and IF5 follow from it.
- **CR17: closed.**
  - `index_test`'s CR12 block now has 70,000-occurrence entries, past the 64 KiB cap
    (`index_test.cc:2155-2176`). m3, the lone run back to a `logic_error` (MU2), aborts
    `index_test` with "entry past the run cap".
  - The LEB128 and `view()` checks at 2^32 are at `tuple_buffer_test.cc:438-465`, in a reserved
    32 GiB range. m4, 32-bit lengths (MU3), fails `:461-462`.
  - The CR12 block is also compared with `segment_writer`'s bytes (`:372-380`).
- **CR18: closed** (check 1).
- **CR19: closed on the branch.** The bisect skips are named (R26). The calibration row is the
  lead's (that review's C5), and `calibration.md` doesn't have it yet.
- **CR20: closed, with one stale clause.** `inverter.md` has one State (`:13`), Doing (`:162`),
  Next (`:221`), and Last verified (`:224`). `:214` still says "`options::max_run_bytes`
  stays for tests" (IF7).

## Findings

- **IF1 (medium, limit; pre-existing; for the architect): a lone document past 2^32 side
  entries wraps the buffer's side index and commits a corrupt segment. It does not fence.**
  - Where:
    - `push(tuple(..., std::uint32_t(side_.size() - 1), long_term))` truncates
      (`tuple_buffer.hh:274`, `:288`).
    - `max_side` (`:505`) is read only by `fits` (`:325-327`).
    - The index's `fits` returns true without an active buffer (`index.hh:1889`), and `apply`
      only refreshes when it fails (`index.hh:405`). It never refuses.
    - Stage 1 counts no entries (`reader.hh:303-342`).
    - A side entry is taken by every terms entry, and by every token of 255 bytes or more, at
      position 2^24 or later, or at byte 2^32 or later (`tuple_buffer.hh:269`).
  - Reach: one standalone document. A replicated one stays under the 64 MiB command ceiling.
    - The cheapest form is 2^32 tokens over one 255-byte word, plus one term. Stage 1 lets the
      first token sit at position 0, and tokens may share bytes (`reader.hh:328-331`).
    - That is about 300 GB resident: 48 GiB of `text_token`s, and 64 bytes a side entry (its
      entry, position, and tuple).
    - The ledger's form, more than 2^24 terms beside 2^32 − 1 words (`:131-133`), costs about
      the same. So the ledger's minimum is one form of two.
  - Probe (`probe_side_wrap`, through `index::apply` at one flush thread):
    - Against the head: 4,000 to 9,000 terms, alone or after a small document, are all found
      with the right positions.
    - Against the 12-bit model:
      - 4,096 terms plus the key, one side entry past the index: accepted, refreshed, and
        committed, with one term missing from the segment;
      - 5,000 and 9,000 terms, and 5,000 after a small document (where `fits` refreshes and
        then takes the document anyway): accepted and committed; reading the segment throws
        `corrupt_segment: document frequency`.
    - Nothing refuses the document and nothing fences: the commit publishes the damage. That
      is worse than the memory edge the ledger files it with (`:126-133`), which fences.
  - The same structure has a second 32-bit edge. `group_run`'s counting sort keeps `uint32`
    counts (`tuple_buffer.hh:1035-1040`), so 2^32 tuples in one bucket (2^32 tokens of one
    word) wrap its prefix sums. The reach is the same.
  - Pre-existing: the side index, `fits`, and `group_run` are unchanged from main. CR16 adds a
    split-text route of the same cost.
  - Fix:
    - A stage-1 refusal of a document with more than 2^32 − 1 terms plus tokens (terms plus
      words, for split text) in `inverted_plugin::validate`. It is a document-only rule, so
      replicas agree, and it bounds both structures.
    - A limits row for it, at the largest of the fixed caps: Xapian's document length,
      2^32 − 1 (`limits.md:19`). Lucene and Tantivy fail such a document far earlier (CR15).
    - Or, at the least, a `length_error` before the cast, which turns the corruption into
      D10's fence.
- **IF2 (low, text; bears on the lead's indexing rows): flush points moved at both thread
  counts, and nothing lists the move.**
  - One flush thread (C11): the drain's kept scratch is charged now (`tuple_buffer.hh:755-765`,
    `:831`).
    - `probe_scratch`, on the corpus: at the first full-batch drain (document 6,371), main's
      charge grows 10.8 MB and the head's 23.8 MB.
    - At 25,000 documents the head charges 77.8 MB and main 61.9 MB (+26%).
    - At 100,000 documents and a 64 MiB share, main writes 4 segments and the head 6.
    - The ledger says "Past a bucket's share the buffer is main's" (`inverter-followups.md:62-63`).
  - More than one flush thread (CR16): a split document is no longer rewritten into tokens
    before it is charged, so `doc_bytes` drops 12 bytes a word (`index.hh:1812`).
    - At 20,000 documents, two threads, and a 32 MiB share, main writes 4 segments and the
      head 2.
    - `probe_floor`'s whole-document column (433 B a document, quoted at `:62`) is main's
      charge. The head charges 313 B.
  - Both charges are correct (D17), and segments are identical at equal flush points (check 6).
    But R30's three causes (`layers.md:785-789`, `inverter.md:33-38`) name neither move, and
    C10's indexing rows taken at `5b7787e` will not show them.
  - Fix:
    - Name both moves in the ledger, L10's paragraph, and R24.3's line.
    - Correct `:62-63`.
    - Take C10's indexing rows after this branch lands, with the moves stated.
- **IF3 (low, ledger; R29's second half): the core's reads of the text inputs are still
  unlisted.**
  - `fits` computes the entry bound from `terms`, `split_text`, `text`, and `tokens`
    (`index.hh:1888-1893`).
  - The digest inverts the tokens through a `std::map` (`index.hh:705-717`), and CR16 adds its
    split-words loop (`:711-715`).
  - R29 asked for `fits`' bound to move to `inverted_plugin`, or be listed with 7c as its close,
    and for the other reads to be listed until R24.5. Neither ledger lists them
    (`inverter-followups.md:31-35`, `inverter.md:18-46`). The stage-2 rewrite into tokens is
    gone (`index.hh:1812`).
  - Fix: list them with R24.4's deviations in `inverter.md`, closing at 7c and R24.5.
- **IF4 (low, latent): `splittable` ignores `max_word` for any text of up to 2^32 − 1 bytes.**
  - `reader.hh:142-149` returns true without reading such a text. The contract ("none over the
    term limit") holds only because `format::max_term_bytes` is `fst::max_key_length`,
    2^32 − 1 (`fst.hh:130` at the pin).
  - The test pins the surprise: `splittable(std::string(70000, 'w'), 10)` is true
    (`tuple_buffer_test.cc:436`).
  - A lower term limit, from a preset or a format, would admit over-limit words in every text
    under 4 GiB. The term dictionary would then throw at flush (`termdict.hh:95`), in stage 2,
    which is a fence.
  - Fix: return early only when `text.size() <= std::min(most, max_word)`, which reads nothing
    more at today's limit, or `static_assert(format::max_term_bytes >= most)`. Invert or drop
    the 70,000-byte check.
- **IF5 (low, tests): CR16's tuple-offset test passes without the clause it tests.**
  - m5 deletes `&& offset < tuple_offsets` (`tuple_buffer.hh:269`), and `tuple_buffer_test`
    still passes. With the clause gone, every word takes a direct tuple, whose offset still
    fits 32 bits in the test's short texts (`tuple_buffer_test.cc:427-434`).
  - Without the clause, `std::uint32_t(offset)` truncates every word past 4 GiB, and no test
    reaches one.
  - Fix: have the test see the side path. For example, add a side-entry count to
    `tuple_buffer_test_access` (it exists for `view()`). Then check that `tuple_offsets` 16
    sends the far words to side entries and 2^32 sends none.
- **IF6 (low, D17): no test sees C11's charges, and three allocations stay uncharged.**
  - Tests:
    - m8 removes the object's charge (`index.hh:950`), and `index_test` passes.
    - m10 removes `charge_scratch()` from `drain` (`tuple_buffer.hh:831`), and `index_test`
      passes too.
    - C11's floor rests on the probes alone. A test that one buffered document charges at
      least `sizeof(active_buffer)` and stays under a few KB would hold it.
  - `extra_keys_`, a `std::deque<std::string>` (`tuple_buffer.hh:1198`), is in no charge
    (`:743-765`).
    - Through the index at format 12 it stays empty, since a key is always one of its
      document's terms (`index.hh:1745-1758`).
    - libstdc++'s deque allocates its map and first node at construction. `deque_alloc`
      measures 576 B beyond the object with GCC 16 and 0 with libc++.
    - So each writing index on Linux holds 576 B of uncharged heap, against the 2,898 B one
      buffered document charges on macOS. The ledger's floor counts are macOS heap too.
  - `active_buffer::dead`, a bit a document (`index.hh:954`), is uncharged. It is small.
  - After an ENOSPC flush, the buffer is kept (`index.hh:1948-1958`), but the last drain's run
    pages and its new scratch were charged to the flush's local `run` (`tuple_buffer.hh:377-384`,
    `index.hh:1926`), which is gone. `scratch_charged_` then records them as charged, and they
    stay uncharged until the buffer is freed. The pattern is pre-existing for log pages and
    columns; C11's scratch charge follows it.
  - Fix: create `extra_keys_` on first use behind a `unique_ptr` (a deque keeps its strings in
    place, which the key views need), and charge it. Note the others in C11's rows, or charge
    them.
- **IF7 (low, ledger).**
  - `inverter-followups.md:112-114` says "this ledger commit's push starts the run whose
    asan-ubsan and tsan jobs gate landing". `9cb5e1c` is docs-only, so no run started. The
    gating jobs are run 39211275577's at `8cf61a2`, all green.
  - `inverter.md:214` says "`options::max_run_bytes` stays for tests", against `:23`.
  - The object is 1,312 B at the head, not 1,304 (`:46`), since `scratch_charged_` came after
    the measurement.
  - The edge at `:131-133` is filed as "the same class" as the memory fence. It isn't (IF1).
- **IF8 (nit, one implementation per mechanism): `chunked_spill::chunk_size`
  (`section_io.hh:137`) is `tuple_detail::doubled(64 << 10, c, 1 << 20)`.** CR18's commit
  makes `doubled` the one helper for that series. Fold the third expression in with the next
  change to either file.

## Blocking items

None.

## Conditions

- C1 (IF1): the architect rules on it, as part of the D10 question. Before the next gate, or any
  claim that a standalone document has no per-document cap, the stage-1 entry rule (or the
  `length_error`) lands with its limits row.
- C2 (IF2): the ledger, L10, and R24.3's line name both moves. The lead takes C10's indexing
  rows after this branch lands.
- C3 (IF3, IF7): the ledger lines, with C2.
- C4 (IF4, IF5): with the next change to the split path, or with C1.
- C5 (IF6, IF8): before C11's many-index rows on the gate VM.
- C6: CR19's calibration row (the lead's). R32.2 to R32.5, R33, and R24.5's hash keep their
  owed code-review ruling.

Not run: no benchmark or timing (excluded). No local TSan; CI's linux-tsan is green at
`8cf61a2`. No local Linux or FreeBSD build; CI's six platform jobs are green, and
`deque_alloc`'s libstdc++ number is GCC 16 on macOS. No document past 4 GiB or past 2^32
entries: IF1 comes from the code and the 12-bit model. No fuzzing, gate VM, or many-index
heap count. No full ctest at the intermediate commits, only the syntax checks and the two
commits' touched tests. No ruling on R32.2 to R32.5, R33, or R24.5. No Xapian or Xapiand GPL
source.
