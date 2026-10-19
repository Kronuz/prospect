# inverter-followups

- Design: the architect's "Re-check on main (inverter rebase)" (`docs/reviews/layers-design.md`,
  R26 to R33, C11); the code reviewer's re-check of CR11 to CR13 (`docs/reviews/inverter-main.md`,
  on `review-main-inverter`) folds in when it lands.
- Branch / worktree: `phase4/inverter-followups` at
  `/Users/kronuz/code/Kronuz/strata-wt/inverter-followups`, from `origin/main` at `2b598dd`.
- State: every follow-up committed and pushed; each commit built and tested before it was
  committed. CI validates the head (each earlier push's run was cancelled by the next push).
- Known bisect gap on main (R26): `2ec8c36` and `1e2d89b` do not build (`index.hh:1832`,
  "use of undeclared identifier 'split_words'"; at `2ec8c36` also `tuple_buffer_test.cc:76`).
  Main's history stays; a bisect over main marks both `git bisect skip`. `888d03c` builds.
  Every commit on this branch builds and passes ctest before it is committed.
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - R28.1, CR9's wrap: `index_options::max_run_bytes` is gone; the run cap is the index's
    private `max_run_bytes_` (`tuple_buffer::run_cap`, 2^32 - 1, by default), which only
    `index_test_access` (defined by `index_test.cc`) lowers, and `drain` asserts the cap is
    at most `run_cap`. No public field can pass it. ctest 35 of 35 (macOS, Apple clang,
    Release); segments, manifests, and meta byte-identical to main's at 20k and 100k
    (`37ee946`).
  - R27 and R28.2-3, R23's rest: `tuple_buffer::add_with`'s debug block asserts all six
    stated preconditions (terms unique, by a sorted copy; each terms entry's positions
    strictly increasing, beside the token clauses). The architect's `probe_preconditions`
    aborts in all three cases with asserts on (case 1 at the uniqueness assert, case 2 at
    the positions assert). `inverted_plugin::valid_words` is private; `inverted_builder::
    add_text` is private, reached only by `segment_writer` (itself through `writer_access`).
    ctest 35 of 35 (Release); `tuple_buffer_test`, `segment_test`, `index_test`, and
    `multisegment_test` pass with asserts on (-O1); bytes identical to main's at 20k, 100k
    (`4b15eeb`).
  - R29 and R32.1, duplication: `keys_plugin::key_of` returns a view into the part
    (`std::optional<std::string_view>`; the operation outlives stage 2), and the core's
    `key_term` is deleted, so format 12's key rule lives in `key_of` alone. The version map
    hashes keys with the hashes library's `wordwise::hash`; `index_detail::key_hash` is
    deleted. ctest 35 of 35; bytes identical to main's at 20k and 100k (`5db24ad`).
  - CR18, undefined shifts: the byte log's pages and the arena's chunks take their sizes from
    one helper, `tuple_detail::doubled(first, k, most)`, whose shift is capped by the first
    size's leading zeros, so the 65th page or chunk no longer shifts by 64.
    `tuple_buffer_test` checks the helper at k = 63 to 1,000 and appends 70 pieces of
    1 MiB + 1 to a `byte_log` (a page each, past the 64th). ctest 35 of 35 (Release); bytes
    identical to main's at 20k and 100k (`9ce600a`).
  - C11, the tuple buffer's floor: a batch's tuples go to one bucket until it holds a
    bucket's share (`batch_tuples / 256`, 4,096), then to the 256 buckets, which are
    allocated then and stay; the side-entry and positions chunks start at 8 entries and
    double to a whole chunk; the text arena starts at 256 B (was 4 KiB). The buckets' array
    leaving the object takes it from 7,392 to 1,304 B, and the index charges the whole
    `active_buffer` (D17), and the drain's kept scratch. Segments are unchanged: a batch
    grouped as one bucket gives the same groups, sorted by term. Measured on macOS (Apple
    clang, Release, heap counted by `malloc_size`, 1,000 indexes open at once, corpus
    documents, `.scratch/floor.cc`, small-indexes' M0 counting; idle as M0's, then 1, 10, and
    100 documents buffered at one flush thread):

    | Per index | main (`2b598dd`) heap / charged | C11 heap / charged |
    |---|---|---|
    | Idle | 3,674 B / 0 | 3,674 B / 0 |
    | 1 buffered | +36,316 B / 27,956 B | +7,929 B / 6,846 B |
    | 10 buffered | +117,983 B / 107,526 B | +69,686 B / 61,199 B |
    | 100 buffered | +682,886 B / 607,411 B | +680,748 B / 612,387 B |

    The architect's `probe_floor` (ten-word documents, the buffer alone): 12,522, 19,382,
    and 96,663 B charged at 1, 10, and 100 documents on main; 1,002, 5,194, and 51,571 B
    now, against the whole-document buffer's 433, 4,330, and 43,570. Past a bucket's share
    the buffer is main's: 20 bytes a token, the inverter's design, about 6 KB a corpus
    document. ctest 35 of 35 (Release); `tuple_buffer_test`, `index_test`,
    `multisegment_test`, `segment_test` pass with asserts on; bytes identical to main's at
    20k and 100k (`7bca610`).
  - CR16, the lead's ruling, the split-text cap widened: a split text is refused only when it
    has more than 2^32 - 1 words or a word over the term limit (`text_detail::splittable`,
    which reads only a text past 2^32 - 1 bytes), not past 2^32 - 1 bytes (2^31 words).
    Mechanism: a word at or past byte 2^32 of its text takes a side entry (a 64-bit
    address) since tuple offsets are 32 bits (`tuple_buffer::tuple_offsets`, a test hook).
    At more than one flush thread a split document stays split in the buffer (its length
    counted at stage 2) and the writer splits it at flush (`writer_access::add_split`,
    `inverted_builder::add_text_with`), so no 32-bit `text_token` offset is built; this also
    removes R29's stage-2 rewrite into tokens. limits.md gains the row, at the largest.
    `tuple_buffer_test`: split text with the tuple offset lowered to 0, 16, and 2^32, and
    the multi-thread split add, give explicit tokens' bytes. ctest 35 of 35 (Release); the
    four inverter tests pass with asserts on; bytes identical to main's at 20k and 100k.
    Not reached by a test: a text past 4 GiB (it needs tens of GB resident) (`2518aae`).
  - CR15, text: limits.md's Term bytes row drops the buffer's 2^32 - 67 clause (CR12 removed
    that refusal). The run row's Lucene cell, read from Lucene 10.4.0's source: the 1,945 MB
    hard limit is checked after each document and only marks a flush
    (`DocumentsWriterFlushControl.java:237-241`); a document past about 2 GiB of int-addressed
    pool (`Math.addExact`, `ByteBlockPool.java:199`, `IntBlockPool.java:157`) aborts the
    thread's buffer and closes the writer (`IndexingChain.java:1319-1321`,
    `DocumentsWriterPerThread.java:556-566`). The Tantivy cell adds that a document past the
    arena panics. The mechanism cell and the two `tuple_buffer.hh` comments say what the
    early drain does now: it keeps an over-cap entry to one document's postings, while the
    cut alone keeps runs correct. Comments and docs only; the headers compile
    (`-fsyntax-only` of `tuple_buffer_test.cc` and `strata_engine.cc`) (`807dff9`).
  - CR17, tests: `index_test`'s CR12 block gives a split and a tokenized document 70,000
    occurrences of one word (about 70 KB an entry, past its 64 KiB cap), so each takes a run
    alone; MU2 (the lone run back to a throw) now fails `index_test` (abort, exit 134).
    `tuple_buffer_test` round-trips LEB128 at 2^32 - 1, 2^32, 2^32 + 1, 2^35, and 2^64 - 1,
    and decodes, through the buffer's own `view()` (`tuple_buffer_test_access`), an entry
    header with section lengths of 2^32, 2^32 + 1, 2^32 - 1, and 2^34 in a reserved 32 GiB
    range; MU3 (32-bit `entry_view::lengths`) fails both checks. Its CR12 block is also
    compared with `segment_writer`'s bytes. ctest 35 of 35 (Release); `tuple_buffer_test`
    and `index_test` pass with asserts on, and `tuple_buffer_test` under ASan+UBSan
    (Homebrew clang, -O1, no reports; it also covers CR18's 65th page); bytes identical to
    main's at 20k and 100k (`e9d6f65`).
  - R30 and R31, text: `layers.md` L10 shows the part's three text inputs, says the buffer
    at one flush thread is the tuple buffer (no `document` kept) and at more the whole
    documents, gives `doc_bytes` as the charge at more than one flush thread only, replaces
    "the sums keep every segment unchanged" with R30's three causes of moved flush points
    (byte identity at equal flush points), and has buffered reads go through the tuple
    buffer at one flush thread. Phase 4's D13 and D16 read the active buffer the same way.
    `reader.hh`'s pin says it bears on `doc_bytes` at more than one flush thread.
  - CR20 and R28.4: main's `docs/status/inverter.md` has one State, Doing, Next, and Last
    verified, all current, one public list (with `flush_charge`'s and `section_stream`'s
    changed behavior), and R24.3's line naming R30's three causes.
- Next:
  1. CI green on all eight jobs at the branch's head; then the architect's re-check and the
     code reviewer's of this branch.
- Code review of main (`docs/reviews/inverter-main.md`, APPROVE WITH CONDITIONS), folded
  in here, one commit each: CR14 is R28.1 (`37ee946`: the option is gone, so there is no
  public cap to refuse or clamp); CR18; CR16 (the lead's ruling: widen the split-text cap
  to Tantivy's and Xapian's 2^32 - 1 words, and list it); CR15; CR17; CR20 (main's
  `inverter.md` to one current state); CR19 is R26 (the bisect gap above).
- For the architect (the review's design question, D10 and D17): a document whose buffer
  exceeds what the host can allocate fences the writer, and fences again on every replay
  from `applied_position()`, since on one host the condition never clears. Options the
  review names: a stage-1 refusal by `part_bytes` against a ceiling in replicated state, or
  a row stating that memory bounds a standalone document. Not decided here. The same class
  holds one more edge: a lone document whose side entries (terms, plus words past position
  2^24 or byte 2^32) pass 2^32 would wrap the buffer's 32-bit side index; it needs more than
  2^24 terms beside 2^32 - 1 words, hundreds of GB resident.
- Blocked on: nothing.
