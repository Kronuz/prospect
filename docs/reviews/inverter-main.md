# strata code review: the inverter's last fixes on main (CR11 to CR13)

- Date: 2026-10-18. Tier: fix (the run encoding is in memory only; nothing on disk changes).
- Reviewed: `main` at `734949e`, in a detached worktree (`strata-wt/review-main-inverter`).
  Its code equals `5b7787e`'s, since `734949e` changes only `.github/workflows/ci.yml`.
  - The fixes, on `perf/inverter` (`35498e2`, equal to origin): CR11 `5cde290`, CR12 `9aed71d`,
    CR13 `09aa9b2`.
  - The rebase onto layers, `ac2c3e2..5b7787e`: `2ec8c36` (the squash), `1e2d89b`, `888d03c`,
    and `5b7787e` (the ledger).
  - `origin/main` has since moved to `2b598dd`, which adds only to `docs/reviews/layers-design.md`.
    I did not read it, to keep this review independent.
- Checked against: `docs/reviews/inverter-code.md` (CR9 to CR13), the lead's rulings in
  `docs/status/inverter.md`, `docs/design/limits.md`, `phase-3.md` D10, and `calibration.md`.
- Checked by:
  - Release builds of main and of the base `ac2c3e2` (Apple clang 17, `-march=native`, macOS
    x86_64). ctest passed 35 of 35 in 118 s.
  - ASan+UBSan as CI runs them: Homebrew clang 23, Debug, `-O1`, asserts on.
    `tuple_buffer_test` (49 s), `segment_test` (496 s), and `index_test` (162 s) all pass with
    no reports.
  - CI: run 39192342645 (`5b7787e`) and run 39194785687 (`734949e`) are green on all eight
    jobs, the sanitizers and FreeBSD included.
  - Probes, in `.scratch/probes/`, with their outputs in `.scratch/out/`:
    - `ident.sh`: byte identity of `strata_engine` against the base.
    - `probe_boundary.cc`, built against `.scratch/open`, a copy of the header with its
      private sections made public.
    - `probe_index_main.cc`: index workloads and crash replays.
    - `probe_scan.cc`, `probe_shift.cc`, `probe_cr12_block.cc`, and `probe_bigdoc.cc`.
    - `mutate.py`: the mutants.
    - `strata_engine_cap.cc`, which reads `STRATA_RUN_CAP`, built against the counting header
      in `.scratch/instr`.
  - Not run:
    - Benchmarks (excluded).
    - A probe past 4 GiB, since one entry that large needs about 16 GB resident on this shared
      32 GiB host.
    - Local TSan. CI's linux-tsan is green at both commits.
    - Fuzzing and gate-VM checks.
    - The rebase's other items: R24.4, R24.5's two word hashes, the earlier CR3 to CR8, and
      Lucene's code past the files cited.

## Verdict

**APPROVE: CR11 to CR13 hold on main, and nothing blocks. Fixes are required on main as
conditions: CR14 and CR18 (code), and CR15 and CR17 (text and tests). CR16 goes to the
architect and the owner.**

What holds, and reproduces:

- No size refusal is left between the log and the buffer. The buffer no longer throws for an
  entry's size: `reserve_entry` only drains (`tuple_buffer.hh:274-280`), and `drain` gives an
  entry past the cap a run of its own (`:718`).
- Every other throw in stage 2 has a stage-1 check in front of it:
  - the stored record (`reader.hh:668`);
  - split text over 2^32 − 1 bytes (`reader.hh:353`, ahead of `tuple_buffer.hh:193`);
  - the segment document limit, which `fits` flushes ahead of (`index.hh:1909-1914`).
- The index-level probes ran 204 workloads, and none fenced. They cover lone runs at every cap,
  and crash replays at caps different from the first run's.
- Segments are byte-identical to the base's at 20,000 and 100,000 documents, including when
  every entry is a run of its own.

## Criteria (the four checks asked for)

| Check | Met | Evidence |
|---|---|---|
| 1. No oversized document can fence the writer or its log replay | yes, for every size limit; memory remains (design question) | `probe_index_main`: 10 seeds × G/T/S × caps 2^32 − 1, 64 KiB, 4 KiB, 0, 600 transactions each, with 70,000-token documents, 70 KB words, terms, and keys, and terms with 40,000 given positions. No FENCE in 204 runs. There were 295,416 lone runs; the largest was 80,021 bytes. Capped output equals uncapped output in 90 of 90 runs: every file after every commit, every digest, the verdicts, the contents, and the merge. Two threads equal one in 30 of 30. Crash and replay: 54 of 54 runs equal the straight run (verdicts, contents, digest, merge). They reopened at applied 0, 62, 161, 187, 252, and 322 and replayed with caps of 64 KiB, 2^32 − 1, and 0. Five runs under ASan+UBSan are clean |
| 2a. Nothing on disk changes | yes | `ident.sh`, 20,000 documents, `STRATA_BUDGET_MB=2056`: main matches `ac2c3e2` in 5 of 5 files. They also match `ref-base-20k`, the earlier reviews' base. With 2,858 deletes, they still match. Capped runs match the base at every cap. Lone runs per cap: 0 gives 1,678,522; 64 gives 2,370; 256 gives 52; 1 KiB and above give none. At 4 KiB there are 5,140 cut runs. At 100,000 documents, main and cap 0 (8,900,623 lone runs) both match the base |
| 2b. In-memory encoding at the boundaries | yes | `probe_boundary` passes all 137,764 checks, in Release and under ASan+UBSan. LEB128 round-trips 2^32 − 1, 2^32, 2^32 + 1, 2^35, 2^63, and 2^64 − 1 at their byte counts. `view()` decodes section lengths of 2^32 − 1 through 2^34, each length and `end − data` exact (in a 64 GiB reserved range). The cut test covers 270 cases: entries of exactly cap − 1, cap, and cap + 1 bytes for caps 200, 1,000, and 4,096, with 0 to 130 entries around them. In every case each run is within the cap or holds one entry, marks fall on every 64th entry's offset, keys in lone runs resolve, and the flush equals both an uncapped buffer's and `segment_writer`'s. `splitters()` and ranged `each_term()` over lone runs equal the whole merge in 64 of 64 splits. `probe_scan`: `scan()` equals the uncapped buffer's in 40 of 40 |
| 3. The limits row against Lucene's, Tantivy's, and Xapian's effective per-document limits | partly | The run row's Status holds for the run cap and for tokenized documents, but three things fall short. Split text keeps an unlisted cap below the largest (CR16). The row's Lucene cell names the wrong mechanism (CR15). The Term bytes row is stale (CR15) |
| 4. The tests fail when each fix is reverted | partly | MU1, restoring CR9's stage-2 refusal (the state CR11 fixed), fails both tests. MU2, reverting the lone run to the old `logic_error`, fails `tuple_buffer_test` only. MU3 (32-bit lengths) passes both (CR17). MU4, removing CR13's cut, fails `tuple_buffer_test.cc:341` |

## Status of CR11 to CR13

- **CR11: closed, superseded by CR12.**
  - CR11 added a stage-1 refusal (`fits_run`), and CR12 took it out with the stage-2 refusal it
    guarded. Neither stage refuses by size now.
  - This also removes the verdict's dependence on `max_run_bytes`, an option rather than
    replicated state. So replicas whose options differ still agree. The replay probe with
    different caps shows it.
  - What survives of CR11: `index_options::max_run_bytes` (see CR14), and `reserve_entry`'s
    unconditional drain, which clears a stale longest-term bound.
- **CR12: closed.**
  - `entry_ref::size`, `entry_view::lengths`, and the encoder's lengths are 64 bits
    (`tuple_buffer.hh:517`, `:770`, `:823`). The room reserve is 66 bytes plus the term
    (`:825`), which is exact: term size 5, postings 5, positions 5, cf 10, flags 1, and four
    lengths of 10.
  - The drain's lone run is at `:718`.
  - The tests cover the refusal's removal but not the widths (CR17). The row text and the
    rationale need corrections (CR15).
- **CR13: closed.** `run_count()` is at `:284`. The test at `tuple_buffer_test.cc:324-343` fails
  without the cut (MU4).

## Findings

- **CR14 (low, code): `index_options::max_run_bytes` is public and unchecked.**
  - Where: `index.hh:322-323`. It is copied into the buffer at `index.hh:1887`.
  - Above 2^32 − 1: a batch's encoded size never passes the cap (`tuple_buffer.hh:715`), so a
    batch over 4 GiB becomes one run. Its marks are truncated to 32 bits (`:726`). Two
    readers use them:
    - `splitters()` (`:390`);
    - the ranged merger's lower bound (`:557`, `:565`).
    On main only `tuple_buffer_test` calls either. `perf/indexing-gap`'s parallel flush is
    built on both (the ledger's key-range interface, `339acf7`).
  - Near 0: every entry becomes a run, and so a merge cursor. At 20,000 documents, cap 0
    peaks at 568 MB RSS against 93 MB at 1 KiB. At 100,000 documents it makes 8,900,623 runs.
  - The ledger calls the option a test hook (`docs/status/inverter.md:18-19`), but nothing
    keeps it one.
  - Fix:
    - Clamp it to [1, 2^32 − 1] where it is copied, or reject other values at `create` and
      `open` with `invalid_argument`.
    - Or make it `std::uint32_t` and move it out of the public options.
    - Add a test that a cap of 2^32 is refused or clamped.
    - Land this before `perf/indexing-gap` does.
- **CR15 (low, text): the limits rows and the rationale for the early drain are off in three
  places.**
  - The Term bytes row (`limits.md:15`), strata column, still reads "2^32 − 67 in the buffer
    (its run row)".
    - CR12 removed that refusal: the buffer takes any term of up to `format::max_term_bytes`.
    - Fix: drop the clause.
  - The run row's Lucene cell (`limits.md:20`) reads "`RAMPerThreadHardLimitMB`, 1,945 MB a
    thread: flushes at it and fails a larger document's inversion". Lucene 10.4.0's source
    disagrees:
    - Lucene checks the hard limit only after a document, and only sets flush pending
      (`DocumentsWriterFlushControl.java:205`, `:237-241`).
    - Within a document, the per-thread pools are int-addressed: `ByteBlockPool.nextBuffer`
      (`ByteBlockPool.java:199`) and `IntBlockPool` (`IntBlockPool.java:157`) use
      `Math.addExact`.
    - The resulting `ArithmeticException` is an aborting exception (`IndexingChain.java:1319-1321`).
      It aborts the thread's buffer, and the writer closes on a tragic event
      (`DocumentsWriterPerThread.java:556-566`, `IndexWriter.java:918`).
    - So Lucene fails a document at about 2 GiB of pool, not 1,945 MB, and the failure closes
      the writer and loses the thread's buffered documents. It is Lucene's form of a fence.
    - Tantivy is similar: a document past the arena panics at `assert!(page_id < 4096)`
      (`tantivy-stacker` 0.7.0, `memory_arena.rs:204`), and the budget is checked only after
      each document (`index_writer.rs:192-195`).
    - Fix: correct the Lucene cell, and add to the Tantivy cell that a document past the
      arena panics. The Status stays right: strata now takes such documents.
  - The mechanism cell says that, through the early drain, "only one document's entry can"
    pass the cap. The commit message of `9aed71d` says "Run marks stay 32 bits: the early
    drain still keeps any multi-document entry within the cap." But nothing depends on the
    early drain now:
    - The cut alone keeps every run of more than one entry within the cap (`:715-718`), so
      marks fit 32 bits.
    - 64-bit section lengths make a large entry from several documents safe.
    - MU5 removes the early drain, and `probe_boundary` still passes all 22,747 of its
      checks, flush comparisons included. Only CR13's test fails, because it relies on the
      early drain to start its drain.
    - The comments at `tuple_buffer.hh:170-171` and `:272-273` repeat the claim.
    - Fix: give the early drain its remaining job, a memory bound that keeps a batch's
      encoded entries near 4 GiB, or remove it and change CR13's test to drain explicitly.
- **CR16 (low to medium, limit; for the architect and the owner): split text has a
  per-document cap that limits.md does not list, and it is below the largest.**
  - `valid_words` refuses a split text over 2^32 − 1 bytes (`reader.hh:353`). Since each word
    takes a byte and a space, a split document holds at most 2^31 words.
  - Against the Position row (`limits.md:17`):
    - Lucene's column, 2,147,483,519 (`IndexWriter.java:209`): strata is 129 above it.
    - Tantivy's 2^32 − 1 (u32 positions; its arena holds about 4 × 10^9 one-byte deltas) and
      Xapian's 2^32 − 1 (32-bit termpos, wdf, and length, as documented): strata is half.
  - The run row says "A document's tokens are bounded only by the position, wdf, and length
    rows". For split text they are also bounded by this cap.
  - Tokenized documents do reach 2^32 − 1 tokens, since tokens may share bytes.
  - The mechanism is the 32-bit token offsets in `tuple::off` and `text_token::offset`.
  - It bites only standalone documents: a replicated document stays under the 64 MiB command
    ceiling.
  - Fix, either:
    - add a "Split text bytes" row: 2^32 − 1, its mechanism, and "**Below the largest**: 2^31
      words a text against 2^32 − 1 positions";
    - or lift the cap, for example by giving far tokens 64-bit offsets in side entries, as
      positions past 2^24 already go there.
- **CR17 (low, tests): the tests cover the refusal's removal, not the lone run or the widths.**
  - `index_test`'s CR12 block (`index_test.cc:2148-2174`) says "each such entry takes a run
    alone". But none of its entries passes the 64 KiB cap:
    - its largest entry is about 4 KB (4,000 one-byte deltas);
    - only the bound passes the cap, at 20 bytes a token;
    - `probe_cr12_block`, the block as it is, shows 3 early drains and 0 lone runs;
    - so MU2 passes `index_test`.
  - No test can see MU3 (lengths back to 32 bits), because no test reaches 4 GiB.
  - `tuple_buffer_test`'s CR12 block (`:344-361`) compares only with an uncapped buffer, which
    shares the encoder.
  - Fix:
    - Give one `index_test` document an entry past the cap, such as 70,000 tokens of one word
      or a 70 KB term, so MU2 fails there too, and fix the comment.
    - Check `view()` and the LEB128 lengths at 2^32 on a synthetic entry, as
      `probe_boundary` part B does, through a test hook.
    - Compare the CR12 block with `reference()` as well.
- **CR18 (low, code, outside CR11 to CR13): the page and chunk doubling shifts by 64 or more,
  which is undefined behavior.**
  - Where: `std::size_t(4096) << pages_.size()` (`tuple_buffer.hh:97`) and
    `<< arena_.size()` (`:671`). The shift reaches 64 at the 65th log page or arena chunk.
  - UBSan reports "shift exponent 64 is too large for 64-bit type 'std::size_t'":
    - at `:97:73` from `probe_shift L`, 70 runs over 1 MiB, each a page;
    - at `:671:91` from `probe_shift A`, one batch of 70 texts of 4 MiB + 1.
  - In Release the segment still equals `segment_writer`'s, because x86 masks the shift count.
  - Reach: a run over 1 MiB takes a page. 100,000 corpus documents make 18 runs in one buffer,
    so about 350,000 documents in one flush pass 64 pages, and every 1M-document gate run
    executes the undefined shift. CI's sanitizer jobs never get that far.
  - It came from `4125b30` and `1dadaf6` on `perf/inverter`, before CR11.
  - Fix: cap the exponent (`std::min<std::size_t>(pages_.size(), 8)` and
    `std::min<std::size_t>(arena_.size(), 10)`). Add a sanitizer-run test that appends 65
    pieces of 1 MiB + 1 to a `byte_log`.
- **CR19 (low, process): two commits on main do not build.**
  - `2ec8c36`, at `tuple_buffer_test.cc:76`: it calls `segment_writer`'s now-private text add.
  - `2ec8c36` and `1e2d89b`, at `index.hh:1832`: `split_words` is unqualified after its move
    into `text_detail`.
  - `888d03c` fixed both.
  - CI never built them: runs 39189361575 and 39189519677 were cancelled by the next push.
  - `git bisect` across main has to skip them.
  - Fix: main's history stays as it is. Build each commit of a squash landing before pushing
    (layers' C6 rule), and record the miss in `calibration.md`.
- **CR20 (low, ledger): `docs/status/inverter.md` on main states current and stale positions
  side by side.**
  - Current: "Next: the code reviewer's review of the rebase" (`:43`) and "Next: the code
    reviewer's re-check of CR11 to CR13" (`:220`).
  - Stale: "Doing: nothing (paused)" and "Next: 0. The code reviewer re-checks CR1 and
    CR2…" (`:155-173`), and "Last verified: … CR10's commit" (`:221`), which predates the
    "Verified at `888d03c`" lines (`:39-42`).
  - Fix: one State, Doing, Next, and Last verified, all current.

## Blocking items

None.

## Conditions

- C1 (CR14): before `perf/indexing-gap` lands.
- C2 (CR18): before the next gate run that buffers more than about 350,000 documents in one
  flush.
- C3 (CR15, CR17, CR20): with C1 or C2.
- C4 (CR16): the architect and the owner choose the row or the lift. Until then the run row's
  "bounded only by" sentence names the split-text cap.
- C5 (CR19): a calibration row (the lead's).
- C6: CI green on all eight jobs at the fixing commit.

## Design questions (for the architect)

- **D10 and D17: memory is the one stage-2 failure left, and on a given host it is
  deterministic for a given document.**
  - A document whose buffer exceeds what the host can allocate fences the writer. It then
    fences again on every replay from `applied_position()`. D10 allows this: memory
    exhaustion "never reject[s] an accepted transaction: the core stops applying until the
    condition clears, or fences" (`phase-3.md:260-262`; `index.hh:1880-1881`). For one
    oversized document on one host, though, the condition never clears.
  - Measured with `probe_bigdoc`, one split-text document at one flush thread:
    - 64 MiB (33,554,432 words, the replicated command ceiling): 2.87 GB peak RSS, about 85
      bytes a word;
    - 8 MiB: 201 MB.
  - Single runs on a loaded host, so these are not measurements: words past position 2^24
    take the side path and cost several times more CPU. User time was 1.35 s at 15.7M words
    and 13.6 s at 33.5M.
  - Lucene and Tantivy fail such documents too, but at a fixed size (CR15).
  - Options:
    - a stage-1 refusal by `part_bytes` against a ceiling taken from replicated state, so
      verdicts never depend on the host or its options;
    - or a documented row that says memory bounds a standalone document.
