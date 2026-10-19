# inverter

- Design: none (a Track P item on T4's one-thread row); experiments in
  `docs/perf/inverter.md`, each written before it runs. Owner direction (2026-10-17, through
  the lead): for every cost, try at least one mechanism neither Lucene nor Tantivy uses
  (sort-based inversion, batch SIMD analysis, arena layouts that make flush a copy), and
  build the winners of the research note `docs/research/indexing.md` (branch
  `research/indexing`) as it lands.
- Branch / worktree: `perf/inverter-on-layers` at
  `/Users/kronuz/code/Kronuz/strata-wt/inverter-on-layers`, from `origin/phase4/layers-refactor`
  at `ac2c3e2`, carrying `perf/inverter` at `35498e2` (kept untouched at
  `/Users/kronuz/code/Kronuz/strata-wt/inverter`) as a squash, redone on layers' plugins.
- State: landed on main (`2ec8c36` to `5b7787e`). The follow-ups from the architect's re-check
  on main (R26 to R33, C11) and the code review of main (`docs/reviews/inverter-main.md`, CR14
  to CR20) are on `phase4/inverter-followups`, ledger `docs/status/inverter-followups.md`.
  Main's `2ec8c36` and `1e2d89b` do not build: a bisect over main skips them (R26, CR19).
- Rebase onto layers (R23, R24):
  - Public additions, the whole list: `document::text`, `document::tokens`,
    `document::split_text` (`inverted_plugin::part`'s, beside `terms` and `length`), and
    `text_token` (beside `term_entry` in `reader.hh`). Two public types changed behavior:
    `flush_charge` counts what it is told without a budget (`held()`), and `section_stream`
    gathers 64 KiB before writing and cannot be copied. The run cap is not an option
    (`index_options::max_run_bytes` left on `phase4/inverter-followups`, R28.1, CR14).
  - Internal: `detail::tuple_buffer` (its six preconditions stated at the type and, on
    `phase4/inverter-followups`, all asserted in debug builds: non-empty unique terms,
    non-empty in-bounds tokens, increasing positions in tokens and in each terms entry, no
    token equal to a terms entry), `detail::chunked_spill` (`section_io.hh`),
    `text_detail::{split_words, count_words, word_counts, spaces}` (`reader.hh`),
    `inverted_plugin::valid_words` and `inverted_builder::add_text` (private on the
    follow-up branch, R28.2-3), and `detail::writer_access`, the only way to
    `segment_writer`'s text add, per-document add, and `flush_from` (private members).
  - `inverted_plugin::validate` takes the token and word checks (key prefix from
    `vc.key_prefix`), `bytes` the text and tokens, `disk_estimate` the tokens and words. R23's
    recommendation taken: with `split_text`, a nonzero `length` is refused (test added).
  - R24.3, as R30 corrected it: budget-triggered flush points moved for three reasons. At
    one flush thread the charge is the tuple buffer's allocations, by chunk; in both
    configurations the version map charges its two tables' capacity; and at more than one
    flush thread the text inputs grow `sizeof(document)` (by 56 bytes on libc++, 64 on
    libstdc++), which `doc_bytes` reads. Only the third is the pin's. The byte-identical
    claim holds at equal flush points only.
  - R24.4 deviations, for the L5 amendment before 7c: the tuple buffer is fed in arrival
    order at buffer time and gets the output order and live set only at flush; it holds
    each document's stored record, key, and version (`stored`'s, `keys`', and the core's).
    It writes through `segment_writer` via `writer_access` (per-document add, then
    `flush_from` into `inverted_builder::finish`). Closes at 7c's builder commits.
  - R24.5: the multi-thread text path (`writer_access::add_text`) stays for the lead's
    later change; the two word hashes are left for the rebase's code review.
  - Verified at `888d03c`: ctest 35 of 35 (macOS, Apple clang, Release); segments,
    manifests and meta byte-identical to `ref-base-20k` and `ref-base-100k` (one segment
    each, so equal flush points); CI run 39189683111 green on linux-x86_64-clang,
    linux-x86_64-gcc, linux-arm64-gcc, macos-arm64, freebsd-x86_64 when recorded.
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - CR1 (`0da5a05`): `sort_run` keeps tied runs on an explicit stack instead of recursing
    per 8 shared bytes; `tuple_buffer_test` checks two 1 MiB terms on the main thread and
    two 64 KiB terms on a 512 KiB-stack pthread, alone and behind the radix path. One
    100,000-document run each against `r26`: user 1.76 s vs 1.73 s (single runs, not a
    measurement).
  - CR10: `tuple_buffer_test` sorts 3 seeds of 3 documents, each 380-odd shuffled terms of
    7 to 10 bytes from {a, b} sharing 8-byte prefixes, by radix, against `segment_writer`.
    With the other new tests off, it kills MR1 (no length-class pass) and MR3 (ties not
    re-sorted): both abort with "terms must increase".
  - CR9 (`c8b65c6`): `tuple_buffer::max_run_bytes` (2^32 - 1) caps a run; `drain` cuts a batch's run
    between terms at it, and `add` drains early when an entry's bound (its term, 46 bytes,
    20 a tuple, 5 a given position) could pass it, refusing (`length_error`) a document
    that could alone, before touching the buffer. `add_split`'s quick bound counts a token
    a byte; only over the cap does it count words exactly. `limits.md` row added.
    `tuple_buffer_test`: caps of 16 and 64 KiB give the uncapped segment (instrumented once:
    71 early drains, 170 cuts, 4 exact counts); a 5,000-byte term, 1,000 given positions,
    and a split 5,000-byte word are refused under 4 KiB, and 150 one-byte words are kept.
  - CR2 (`adc82ba`): a run entry's cf is 64 bits in `encode_entry` and `entry_view`;
    `tuple_buffer_test` flushes two wdfs of 0x90000000 and matches `segment_writer`'s bytes.
  - Worktree, Release build with frame pointers (`.scratch/build-rel`), the first 100,000
    documents of the gate corpus copied from the gate VM (`.scratch/corpus`, sha256
    `0a1d5344…acebd`, equal to the VM's `head -100000 corpus.json`).
  - E0 on macOS (`0d8a98f`): per-document allocation, copying, and grouping hold 80% of the
    one-thread samples at 100,000 documents; calibration Tantivy 12.10 s, strata 21.88 s
    (0.544x).
  - E1, documents as token streams (`d5468e4`, kept): byte-identical segments; 2.12x faster
    than `4c9ebfd`; 1.20x Tantivy (interval 1.15-1.26) at 100,000 documents on macOS; 4.5x
    is 3.74x away. CI run 39093822529 (dispatched).
  - E2, inversion on arrival (kept as E3's scaffold): byte-identical segments; 1.077x E1
    (interval 1.034-1.131) at 45% less peak RSS; 1.26x Tantivy; 4.5x is 3.57x away.
  - E3, the research note's inverter C as the active buffer (`tuple_buffer.hh`):
    byte-identical; 1.214x E2 (interval 1.156-1.459, short of its 1.3x rule, kept on the
    lead's direction), 1.544x Tantivy at 100,000 documents; 4.5x is 2.91x away. Peak RSS
    1,229 MB against E2's 447.
  - E4, the encoder's per-term waste (kept by BENCHMARKS rule 3): byte-identical; 1.095x E3
    (interval 1.036-1.202), 1.710x Tantivy at 100,000 documents; 4.5x is 2.63x away.
  - E5 (`bb7c9b0`), unresolved: 17 pooled rounds 1.066x E4 (interval 0.985-1.130); the
    profile shows the grouping and the term sort about 180 ms smaller. Kept provisionally;
    a gate-VM rerun is owed.
  - E7 (`f6b06a1`), the engine's front end: 1.043x E5 (interval 1.024-1.075), 1.875x
    Tantivy.
  - E9, C in batches into a dense term store: byte-identical; 1.143x E7 (interval
    1.002-1.586) at 57% less peak RSS (588 MB); 1.975x Tantivy; 4.5x is 2.28x away.
  - E10 (small terms as bytes) and E15 (decode loops, chains fetched ahead) removed, no win.
  - Linux (gate VM, 100,000 documents): E9 is 2.272x Tantivy (interval 2.187-2.331), 4.27x
    the base; 4.5x is 1.98x away.
  - E18 (term-table probes fetched ahead, the version map by view, gathered section
    writes): 1.055x E9 (interval 1.028-1.066), 1.943x Tantivy on macOS.
  - E17 (PFOR blocks in the store) removed: no change on either platform.
  - E19, sorted runs instead of a term table: byte-identical; 1.075x E18 on the gate VM
    (interval 1.030-1.100), 1.044x on macOS; 2.487x Tantivy on Linux, 2.029x on macOS;
    4.5x is 1.81x away on Linux.
  - 1M on the gate VM: E19 2.672x Tantivy (41.8 against 111.8 s), one segment.
  - E11, the flush writes its segment to the file as it goes: peak RSS -21% at 1M
    (3,832 to 3,030 MB) and -20% at 100,000, time unchanged.
  - The key-range interface for perf/indexing-gap (`339acf7`): drain_for_flush, plan,
    add_documents, alphabet, splitters, each_term.
  - E20, compact run entries: peak RSS -15% at 1M (3,031 to 2,580 MB), time unchanged.
  - The digest's scan without a map (`lifecycle_test` 65 to 59 s in Release; the sanitizer
    job's lifecycle runs had doubled since E1).
  - E21, the front end without zeroing or a branch per check: 1.052x on the gate VM
    (interval 1.012-1.144), 2.613x Tantivy there; 4.5x is 1.72x away on Linux.
  - 5M confirmation on the gate VM (one round each, host loaded): strata E21 279.0 s,
    Tantivy 707.2 s: 2.535x; peak RSS 3.70 GB against 11.64 GB; 2.80 GB in 3 segments
    against 3.07 GB; 4.5x is 1.78x away.
  - From E23 on, gate-VM A/Bs read a warm corpus and write indexes to tmpfs (the Mac's disk
    writes at 12 to 22 MB/s; `docs/perf/inverter.md`, "Measurement conditions").
  - E23 (`097d33f`), entries encoded where their tuples are: 1.035x and 1.056x E21
    (1.011-1.076) on the gate VM, 1.023x on macOS; kept.
  - E24 (`a4f1b40`), the positions spill in chunks: peak RSS -8% on macOS at 100,000, -2.3%
    at 1M on the VM, time unchanged; kept (first removed on the VM's numbers alone).
  - E25 (`147be0e`), the merge keyed by 8-byte prefixes: 1.038x on macOS (1.022-1.064),
    1.02x and 1.03x on the VM; kept.
  - E26 (`297464f`), run entries as one stream: removed (`96e1f92`). Its VM wins came
    while a hung process held a VM core; on the rebooted VM under a strict hold, 1.012x
    (0.996-1.039).
  - E27, E28, E29, E30 removed (`7bd8b67`, `deb2ed2`, `9a384c0`, `b024a86`): no win by rule 3
    on either platform.
  - UBSan's null `memcpy` in E23's scratch fixed (`ff36290`, CI run 39118457889).
  - E31 (`b7dd0f4`), the text split inside the buffer (`document::split_text`): 1.079x on
    macOS (1.069-1.091), peak RSS -7%.
  - E32 (`c62c902`), the version map as one table of buffer indexes: 1.040x on macOS
    (1.016-1.064), peak RSS -9%.
  - E33 (`0a5cabe`), each batch's terms sorted by LSD radix: 1.051x on macOS (1.009-1.129),
    1.011x on the noisy gate VM; kept.
  - E34 (`c6bf4d9`), grouping into scratch with no copy back: removed (`2db1c54`); on the
    rebooted VM under a strict hold, 1.008x (1.000-1.026). CI run 39130900973 had been green
    on all 8 jobs at `87b64dd` (with E34).
  - Deciding rerun (`.scratch/runs/vm-tsc/`, tsc clocksource): Tantivy 3.696x E34's build
    and 2.930x E25's at 100,000.
  - 1M confirmation at `c6bf4d9` (gate VM, three rounds): 3.494x Tantivy (3.207-3.498) at
    1,886 MB against 3,247; 1.334x E21.
  - 5M round at `c6bf4d9` (gate VM, one round each, cold cache): 192.2 s against Tantivy's
    883.7 s and E21's 321.2 s. Not counted (the lead): Tantivy at 884 s against n1017's 512 s
    marks the round as disturbed, likely by the hung process; the 5M row is the lead's, after
    the cut lands.
  - E36 (the harness's texts read in place, transactions of 100), E37 (long groups' run
    entries in PFOR blocks), E38 (tuples built in their bucket): 1.000x and 1.016x, 1.016x
    and 1.018x, 1.011x and 1.008x; removed. With E37 the remaining "double encoding" at the
    flush is about 3%, so the frequent-term carry (Next, item 5) is not worth building.
  - The branch carries main's ASan cap of 120 minutes (`b5f375c`, from `584cab1`): CI run
    39121406949's ASan job hit the 60-minute cap. CI run 39125555615 on `b5f375c` (E32):
    green on all 8 jobs. ctest 34 of 34 at E34 locally.
  - At E32 (`c62c902`), on the gate VM: 3.339x Tantivy at 100,000 (3.257-3.603; E31 1.083x,
    E32 1.084x there, both wins) at 209 MB peak RSS against Tantivy's 291, and 3.470x at 1M
    (3.228-4.418) at 1,888 MB against 3,247; on macOS 2.707x at 100,000 (2.660-2.851). 4.5x
    is 1.35x away at 100,000 and 1.30x at 1M on the gate VM. THP through glibc's tunable: 1.064x (0.859-1.126) with
    compaction stalls and +22% RSS, not pursued.
- Doing: nothing; the branch landed on main.
- Open levers after the landing (from the kept series, for the lead):
  1. Format 13's copy-merge items 1 to 3 (a stored list base, tail-coded blocks anywhere, the
     same for positions; `docs/design/format-13.md` on `phase4/format13`, "Proposed: F13
     candidates from research/indexing") are this item's largest remaining lever. Phase
     timers at `c6bf4d9` (`docs/perf/inverter.md`, "What bounds the kept series"): the two
     encodings format 12 forces (LEB128 runs at the drain 0.32 s, the flush's merge and decode
     0.30 s, and most of `flush_from`'s 0.56 s) are about half the CPU at 100,000 documents on
     macOS; runs holding final blocks would save about a quarter, which on the gate VM is
     about 4.5x at 100,000. A design input for format 13's owner, through the lead.
  2. Stage 1's remaining per-document costs and the harness's copies are 1% to 3% items
     (E36's numbers); the drain's grouping and encoding and `push` are what is left of the
     add and drain phases, near the research note's stage costs.
  3. `termdict::builder::add` is about 1.5% of the gate VM's samples (161 ns a term in the
     research note); a change there is in the termdict library, not here.
  4. The buffer's memory across flushes (below): a few percent at 5M.
- Open with the lead and the architect (D17): reusing a flushed buffer's pages. Measured:
  keeping freed memory cuts minor faults 27% at a 600 MB budget (several flushes), nothing at
  one flush; a fault costs about 5 µs on the quiet gate VM, so at 5M (1.39M faults, 50 s of
  system time) it is worth a few percent. Keeping capacity breaks D17's accounting as built
  (`buffered_bytes()` is the buffer's charge, so kept capacity reads as a full buffer); a pool
  owned by the budget, charged as idle and handed to whichever buffer grows next, keeps the
  bound for many indexes. Not built: it changes what D17 counts. (`split_text`'s review is
  settled: R23 put it in `inverted_plugin::part`.)
- Coordination: perf/indexing-gap merges this branch after each kept item (lead,
  2026-10-18): this branch owns `tuple_buffer.hh` and the one-thread flush, perf/indexing-gap
  the parallel flush and the assembly; boundary changes go under "Interface with
  perf/indexing-gap" in the perf file and in the commit message. `perf/indexing-gap`'s E1 (`934811b`) pipelines the engine's parse at more
  than one thread and calls `tokens_document`, whose body E1 here changed; the two compose.
  E2 moves inversion to stage 2 at one thread; K inversion buffers on threads (D18) are the
  indexing gap's. `phase4/layers-refactor` rewrites `writer.hh` and `index.hh`; these
  changes rebase onto it by the lead.
- Process: the Mac's data volume was 99% full (16 GiB free) at 03:30 on 2026-10-18; writes
  recovered after the small-indexes fixtures finished. Small-indexes' strict holds came every
  10 to 20 minutes through that night, and an unheld `research_workloads tiny` (pid 268527)
  ran at 100% of a gate-VM core from about 01:28 until the lead killed it. E4's build and tests (21:35-21:47) overlapped `perf/indexing-gap`'s strict hold
  `ig-e2`; its eight-thread A/B from that window may need a rerun (the lead decides).
  Every step now runs behind `.scratch/gate.sh`. A second slip on 2026-10-18 at about 03:12:
  a command ran `tuple_buffer_test` and `index_test` (old builds, about 15 s on one core at
  nice 19) after `gate.sh` had refused its build, because the tests followed a `;` instead of
  the gate's `&&`, during `perf/index-size`'s strict hold `index-size-e3c-freebsd` (the
  FreeBSD VM's 1M A/B from 03:10:47); its first rounds may need a rerun (the lead decides).
  Every compile, test, and measurement now runs inside one `gate.sh && { ...; }` block.
  An overlap the gate cannot catch: a build checked free at about 04:38:40 (E33 and E34's
  compile, tests, and byte checks at nice 19, four jobs) ran until about 04:42, past the
  start of `small-indexes`' strict hold `si-f5-open2` at 04:39:02, since a hold taken later
  cannot see work already running; its first minutes may need a rerun (the lead decides).
  Likewise my light-hold A/B on macOS (`inverter-mac-e34`, about 04:41:30 to 04:43:30, one
  thread) overlapped the start of `si-f5-mac` (strict, 04:41:48): a strict hold does not
  wait for light holds already running.
- Findings folded in: CR1 (`0da5a05`), CR2 (`adc82ba`), CR9 (`c8b65c6`), CR10, CR11 (`5cde290`, a stage-1
  refusal), CR12 (64-bit entry and section lengths; an entry past the cap takes a run alone, so
  no size refusal is left in stage 2 or `valid`; `options::max_run_bytes` stays for tests).
  CR13 (`tuple_buffer::run_count`; a test that needs the cut).
  At `09aa9b2`: ctest 34 of 34 (macOS, Release); segments, manifests and meta byte-identical
  to `ref-base-20k` and `ref-base-100k`; CI run 39181650955 green on the six non-sanitizer
  jobs, asan-ubsan and tsan still running when recorded.
  The code review of main (`inverter-main.md`) closed CR11 to CR13; its CR14 to CR20 are
  on `phase4/inverter-followups`.
- Next: none on this branch; the follow-ups' ledger carries the work. The L5 amendment
  (R24.4, widened by R31) is due before 7c's builder commits or D16's code. Blocked on:
  nothing.
- Last verified: 2026-10-18, `888d03c` (main's code at `5b7787e`): ctest 35 of 35 on macOS
  (Apple clang, Release); segments, manifests and meta byte-identical to `ref-base-20k` and
  `ref-base-100k`; CI runs 39192342645 (`5b7787e`) and 39194785687 (`734949e`) green on all
  eight jobs.
- Previously verified: 2026-10-18, `c6bf4d9` (the kept series): ctest 34 of 34 on macOS (Apple
  clang, Release, `-march=native -g -fno-omit-frame-pointer`), byte-identical segments at
  20,000 and 100,000 documents on macOS and the gate VM (clang 18); CI run 39125555615 green
  on all 8 jobs at `b5f375c` (E32).
- Lead's rulings on the second re-check (2026-10-18), for the next developer:
  - CR11: refuse an oversize document at admission, in `valid` (stage 1), with the same bound
    stage 2 uses, so it is rejected before the log and never fences the writer or its replay.
    Test: the 64 KiB-cap probe_index case is refused at add, and the writer stays usable.
  - CR12: meet the limits rule with 64-bit entry and section lengths, so an entry too big for
    a run takes a run by itself; no per-document token cap below Lucene's, Tantivy's and
    Xapian's. limits.md's row says so with the mechanism. Until it lands, the row's Status is
    "Below the largest: open".
  - CR13 (low): a test that fails when the between-terms cut is removed.
