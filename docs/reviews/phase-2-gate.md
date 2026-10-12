# strata review: Phase 2 gate

- Date: 2026-10-12.
- Reviewed:
  - `docs/gates/phase-2.md` at strata `9204851`.
  - `include/strata/{format,writer,reader,search,scorer}.hh`, `tools/strata_engine.cc`,
    `tools/parity.cc`, `test/*.cc`, and `fuzz/fuzz_segment.cc` at `9204851`.
  - The pinned libraries the reader trusts: `termdict` `51278ef`, `residency` `adc23e8`,
    `fst` `a8d8478`, and `bitpack` `0ce2691`.
  - The runs behind the gate tables (`.scratch/phase2`: `results*.json`, `gate.py`,
    `index_1m.sh`) and the segment fuzz logs.
- Checked against:
  - PLAN's Phase 2 gate and the rules every gate carries.
  - BENCHMARKS.
  - ARCHITECTURE.
  - `docs/design/phase-2.md` at `2e9cb57`.
  - The design review (D1–D18, R1) and the Phase 1 review.
- Checked by:
  - Reading every file listed above.
  - Building at `9204851` with the pinned libraries and running the three tests: all
    pass, in the default build and under ASan and UBSan (Homebrew LLVM).
  - Running `parity` against `strata-oracle`: 300 of 300 queries equal, worst relative
    error 3.26e-16.
  - Running a scratch copy of `parity` that takes counts from `count()` and the top 10
    from `top_k` without a count, so every pruning path runs under Xapian BM25: 300 of
    300 equal, worst relative error 3.23e-16, under ASan and UBSan.
  - Running the `analysis` tests: they pass, on 966 term-generation vectors
    byte-identical to the oracle's.
  - Recomputing the query tables and the counts from the recorded JSON.

## Verdict

**REJECT: not approved yet.** Five items, G1–G5, block the gate.

The engine is close. Every settled row passes: relevance parity, with pruning off and
on; the analyzer vectors; the postings size; `termdict` lookup; the Xapian floor; and
counts equal to both baselines on all 1,162 counting queries. The blocking items are:

- G1–G3, the robustness rules every gate carries: decoders memory-safe on corrupt input
  (ARCHITECTURE, and the Phase 1 review's B1), with an hour of fuzzing per decoder at
  the shipped format (PLAN).
- G4, a phrase bound that understates.
- G5, the quiet-host interleaved re-run, which the Phase 1 review made this gate's
  evidence.

None of them changes the format. The re-check covers G1–G5 only. Phase 3 design may
proceed; Phase 3 implementation starts at approval, and conditions C1–C11 carry into it.

## Gate rows

| Row | Evidence | Ruling |
|---|---|---|
| Tests under ASan and UBSan | Not in the report; all three pass in my sanitizer build | Pass |
| One CPU-hour of fuzzing per decoder | The segment target's logs: the completed hour ran at format 1 (`bd15441`); formats 3 and 4 had ten minutes each, formats 5–7 none. The format-8 hour is running: at 31 minutes, 226,000 inputs at 123 per second, 2,699 PCs, no findings. The library targets report runs, not CPU time | Not met (G3) |
| Unknown versions rejected; fixtures | The reader accepts version 8 only; `small.sx` pins format 8 | Pass |
| Query classes | One run per engine; the baselines ran 5.6 hours before strata (see "Benchmark evidence") | Not shown (G5) |
| Term class | 200 queries stratified by document frequency, seed 2026 | Pass |
| Xapian floor | Phase 0: 3–28× slower than the faster baseline in `TOP_10` and 4–194× in `COUNT` (Phase 1 review); 881 docs/s | Pass |
| Single-thread indexing | One trial per engine: 2.05× Lucene, 0.87× Tantivy | Not shown (G5) |
| Postings size | 1,008.0 MB against 990.6 MB: 1.018× | Pass |
| `termdict` lookup | Medians at 0.54× `seekExact`, present and absent keys | Pass; p99 and the hash-map ratio owed (G5) |
| Relevance parity | 300 of 300; reproduced, and reproduced with pruning on | Pass |
| Analyzer | 966 vectors and the per-code-point sweeps | Pass |

## Rulings on the changes for the architect

1. **Scorer contract: `max_score(from, upto)`. Accepted.**
   - A bound over `[from, upto]` from skip data alone lets a conjunction bound children
     it has not moved. The `[doc(), upto]` form made it rescan blocks a dense child had
     not passed.
   - Every scorer meets it:
     - The term scorer takes the smallest skip entry covering the range
       (`search.hh:235-242`).
     - Conjunctions, disjunctions, and required-plus-optional sum their children
       (`400-404`, `543-547`, `628`); exclusion takes its include side (`676`).
     - Phrases meet it in Xapian mode, not in Lucene mode (G4).
   - `scorer.hh` states two more things: `max_score` decodes skip groups into the
     iterator's cache, so it is not `const`; and a disjunction's `block_end` is a
     window, the nearest essential child's block end (`548-552`), not a block.
   - PLAN's minimum competitive score in nested scorers is met. Required-plus-optional
     passes `s` minus the optional bound to its required side (`631-634`), and exclusion
     passes `s` to its include side (`679`). Conjunctions and disjunctions consume it
     without passing it down.
2. **Whole Pareto frontiers. Accepted.**
   - The measurement decides it: with merged pairs, 30,315 of `the`'s 32,563 blocks stay
     above the top-10 threshold, against 21 with whole frontiers. The postings row
     passes with whole frontiers at 1.018×.
   - A frontier holds at most one pair per length code, 244 codes in use. Readers decode
     it only for a bound and stop at the first competitive pair (`reader.hh:353-369`).
   - The re-run reports the frontier length distribution per level (mean, p99, maximum).
     Evaluating long group frontiers is a candidate cause of the frequent-term gap.
   - D3's rule stands for any future cap: stored pairs dominate the dropped ones.
3. **Lucene BM25 by one-byte norms. Accepted.**
   - Lucene scores quantized norms too. The same code in `norms` and in skip data puts
     Lucene-mode bounds and scores through one table (`search.hh:85-87, 257-259`), so a
     bound equals the score bit for bit when the pair is the document's own. Xapian BM25
     keeps exact lengths, which parity needs.
   - The code is not Lucene's. Both are exact below 24, but Lucene's `SmallFloat`
     encodes n − 24 with four significant bits, and `encode_length` encodes n
     (`format.hh:221-231`). The gate rows state this.
   - Adopting Lucene's mapping would make Lucene-mode scores match Lucene's up to float
     rounding, so `TOP_K` hits could be compared with Lucene's. If adopted, it is a
     format change and lands before G5's re-run.
4. **Formats 2–8. Accepted as development versions.**
   - None persisted outside the tree, the reader accepts version 8 only
     (`reader.hh:65-67`), and `small.sx` pins it. From here, every version bump adds a
     fixture and a test that the previous version is rejected or read.
   - On substance:
     - Position and count blocks located from skip data are D1 and R1 as specified.
     - Sized pairs let readers step over frontiers.
     - Term byte lengths make range verification possible, and G1 bounds reads by them.
     - Bitset blocks for dense postings are sound once G1's ordinal check lands.
     - The stored section verifies each frame before a bounded decompression.
     - Norms follow ruling 3.
   - Two parts of the format differ from the design without being listed: raw wdf and
     the docids encoding (C1).
5. **Evaluation changes. Accepted.**
   - **Two-phase phrases** are the design's model: conjunctions leapfrog on
     approximations and confirm only documents every child agrees on.
   - **Windowed MaxScore** is sound (`search.hh:986-1061`):
     - Globally non-essential clauses never open a window.
     - Each window splits its clauses by their bounds over the window.
     - Non-essential clauses are consulted highest bound first while the score can
       still compete.
     - A candidate cut off early carries a partial score, which `collect` rejects
       because it is at most the threshold (`165-169`).
   - **Lead-block windows** are sound (`426-467`). A window is skipped when its
     children's summed bounds over the lead's block cannot compete. A document is
     skipped when its lead score plus the other children's window bounds cannot.
   - **Counting from statistics** is exact without deletes (C3).
   - **The 2k-buffer collector** is sound (`159-190`):
     - The threshold is the k-th best score at the last truncation, so it only lags.
     - Rejecting `score <= threshold` is right because every path collects in ascending
       ordinal order: the term bulk path by block, the disjunction bulk path by window
       and bit, and the generic loop by `next()`. A later equal score loses its tie.
     - That ordering is load-bearing (C2).

## Deviations the report does not list

- **Raw wdf** (`writer.hh:504, 519`), where the design stores wdf − 1
  (`phase-2.md:107-109`). The reason holds: a Xapiand term mixes wdf 0 and wdf ≥ 1
  postings. C1 settles the design text and whether a per-term flag keeps wdf − 1.
- **docids** (`writer.hh:656-671`): a kind byte, then a base or one `u64` per document.
  The design specifies bit-packing and a docid → ordinal array (`phase-2.md:75`). The
  reader neither parses nor validates the section, and consumers assume
  docid = ordinal + 1 (`parity.cc:228`). C1.
- **NEAR** has no scorer, and `parity` rejects it (`parity.cc:144`). PLAN lists near
  queries in Phase 2. C6.
- **WAND.** Nested scoring disjunctions run the heap MaxScore (`search.hh:479-607`),
  which skips nothing unless a parent passes a minimum. PLAN and ARCHITECTURE call for
  block-max WAND there, chosen by a measured cost model. C7.
- **Region checksums** reach `residency` as `verified_regions`, which the segment checks
  on first read, not as a backend check on prefetch (D10's wording). Accepted for the
  synchronous backend, which reads nothing ahead. The asynchronous backends verify as
  they load, through the same per-region state (Phase 6).
- **The term index is not pinned.** ARCHITECTURE and the design pin it; the reader keeps
  a view of the mapping. G2.

## Correctness and safety

### Integrity model

- **Verified at open, whole:** the footer, the TOC, the header, `term_index`, `lengths`,
  `norms`, `docids`, and `stats` (`format.hh:171-215`, `reader.hh:58-105`).
- **Verified per 64 KiB region on first read:**
  - Term blocks: `termdict` verifies a block's length prefix, then the whole block,
    before decoding it (`termdict.hh:289-300`).
  - A term's postings and positions ranges, when an iterator or a positions reader opens
    (`reader.hh:249-250, 732-733`).
  - The stored index, then each frame (`reader.hh:147-163`).
- The tests cover detection and lazy verification (`segment_test.cc:133-173`).

Two reads escape verification:

1. **Decoders are bounded by the section, not by the verified range.** `end_` is the
   section's end in both readers (`reader.hh:252, 735`). With self-consistent verified
   bytes, every read stays in the term's range. Bytes can change after verification:
   ARCHITECTURE leaves mapped pages re-read after eviction unverified. A corrupt length
   or count then walks into neighboring terms' bytes, which nothing has checked. G1.
2. **The term index is checked once.** `term_index`, `lengths`, and `norms` are views of
   the mapping, verified at open. Changed values in `lengths` and `norms` cannot break
   bounds. `fst::map` lookups are unchecked after their construction-time validation
   (`fst.hh:147-152, 260`), so a changed term index page can send a lookup out of
   bounds. This is the exposure the Phase 1 review ruled out for `fst::trusted` over
   mapped bytes. G2.

The TOC also accepts duplicate kinds and overlapping sections (`format.hh:198-212`); a
duplicate silently replaces the earlier entry (`reader.hh:82-93`). No byte escapes a
checksum through it (C9).

### Memory safety on corrupt input

1. **Skip-data offsets.**
   - Level-1 sums are checked only in total (`reader.hh:280-287`).
   - Within a group, level-0 byte lengths accumulate into entry offsets with no check
     against the group's level-1 byte count (`563-584`).
   - The block check forms `p` from that offset and tests
     `cur.bytes > uint64(tail_ - p)` (`611-612`; `or_bitset` at `497-498`). When `p`
     lies past `tail_`, the difference is negative, the cast makes it huge, and the
     check passes, so the decoder reads outside the section. G1.
2. **Bitset ordinals.**
   - `decode_group` checks only a group's last entry against the document count, and
     never checks that entries increase (`569, 587`).
   - FOR blocks pass through `check_increasing`, which bounds them by the document count
     (`688-695`). Bitset blocks skip it (`629-630`), and their span is bounded by their
     byte length, not by the document count (`659-674`).
   - An interior bitset block can therefore emit ordinals at or past the document count,
     and `segment::length()` and `segment::norm()` index by them without checks
     (`130, 193-202`). G1.
3. **Allocations sized by file values.** `positions_reader::read` resizes its output to
   the document's count before reading any position (`764-765`). The bound it checks
   allows up to 2^32 positions, a 16 GiB vector. G1.
4. **Lazy wdf decoding is bounded.** The pending pointer and size come from the block's
   checked byte length, `bitpack::decode` rejects malformed blocks, and every block load
   resets the pointer before use (`438-444, 621-628`). Item 1's fix covers its offset.
5. **Stored frames** are verified before a bounded zstd decompression into exactly the
   recorded size (`144-182`). The record walk sums lengths before checking them
   (`174-180`); reads stay in bounds (C9).

Neither the fuzz target nor the tests can find items 1–3:

- `fuzz_segment` corrupts bytes without resealing checksums (`fuzz_segment.cc:52-53`),
  so every mutation fails verification before a decoder runs.
- Its inputs stay under 4 KiB, and a second skip group needs 4,224 postings.
- Only term scorers run on the corrupted segment (`54-66`).
- `segment_test`'s corruption test checks detection, not decoding.

G3.

### Pruning

- **Frontier bounds never understate.**
  - `decode_length(encode_length(n)) <= n`, with monotone codes (`format.hh:219-231`,
    tested at `segment_test.cc:224-231`).
  - The frontier keeps the largest wdf per length code (`format.hh:355-363`), so a
    stored pair dominates every posting.
  - Both BM25 forms rise with wdf and fall with length.
- **Phrase bounds.**
  - Xapian mode sums each slot's wdf-based term bound and scores each slot's wdf-based
    weight. It is consistent.
  - Lucene mode bounds a phrase by the smallest of its terms' wdf bounds, on the premise
    that phrase frequency is at most each term's wdf (`search.hh:787-798, 851-854`;
    `phase-2.md:233`).
  - Phrase frequency is bounded by position counts (`868-892`), which exceed wdf for
    terms without `counts_are_wdf`. A boolean term carries positions with wdf 0, its
    pairs are (0, length), and the weight at tf 0 is 0 (`90`).
  - Every phrase containing such a term is bounded at 0. With a positive threshold,
    `find()` skips its blocks unverified (`837-840`), and conjunction windows skip past
    it (`434-439`). G4.
  - The benchmark's whitespace terms always have counts equal to wdf, so the gate rows
    are unaffected.
- **Required plus optional is sound.** A document is skipped when its required score
  plus the optional bound over the required block cannot compete (`646-659`). The
  required side receives `s` minus the optional side's total (`631-634`).
- **Conjunction windows and the collector are sound** (ruling 5).
- **Rounding.**
  - Bounds are summed, and partial bounds derived by subtraction, in orders that differ
    from the scores' (`441, 496, 633, 1002, 1021, 1036-1051`).
  - A computed bound can land an ulp below a score that beats the threshold by an ulp,
    so pruned and exhaustive top-k can differ on exact ties.
  - The effect stays inside D16's tolerance (C4).
- **Evidence.**
  - `search_test` checks pruned top-k against brute force at k = 1, 3, and 10, under
    Lucene BM25 (`search_test.cc:152-165`).
  - The pruned `parity` copy matches the oracle on all 300 queries under Xapian BM25.
  - Neither covers counts that differ from wdf, or the composite shapes `strata_engine`
    builds (C5).

### Counting shortcuts

- **Statistic counts are exact without deletes:**
  - Document frequency and `matches_all` (`search.hh:214-215`).
  - |A| + |B| − |A ∩ B| on fresh copies (`1078-1095`).
  - The required side's count (`635`).
  - |A| − |A ∩ B| for a term minus a term (`1063-1076`).
  - The engine's single-term `COUNT` (`strata_engine.cc:181-185`).

  C3.
- **Exclusion windows are sound** (`search.hh:686-701`). Include's window bits AND NOT
  exclude's, each filled from its current position in the same window, leave include
  on its next non-excluded match.
- **Windowed counting is sound** (`968-978`). Every `fill` leaves its scorer on the
  first match at or after the window's end, as `count()` requires. The conjunction and
  exclusion overrides hold only for windows of 4,096 ordinals (C8).
- Counts equal Lucene's and Tantivy's on all 1,162 `COUNT` and `TOP_100_COUNT` queries.

## Benchmark evidence

The recorded runs reproduce the report's table exactly. They do not meet BENCHMARKS'
rules for a gate:

- **Not interleaved, not one session.** Lucene and Tantivy ran at 23:22 on 10-11
  (`results.json`) and strata at 04:59 on 10-12 (`results-strata-v8.json`), 5.6 hours
  apart; the term class ran 45 minutes apart. Each engine ran once, with no confidence
  interval (rules 3 and 4), on a host that was not idle.
- **Arithmetic means.** `gate.py` compares arithmetic class means. The Phase 1 review's
  ruling 6.2 set the per-class geometric mean, which heavy queries do not dominate.
- **The floor constant.** PLAN's ~155 µs is Phase 0's figure. In this session the term
  `COUNT` class, which all three engines answer from document frequency, has medians of
  200 µs (strata), 202 µs (Tantivy), and 208 µs (Lucene). The floor is about 200 µs.
- **The 1.5× line.** On the report's own statistic, `TOP_100_COUNT` intersection is
  1.503× floor-subtracted: over the line, not within it.

Classes near the line, under each statistic:

| Command | Class | Arithmetic, 155 µs floor | Arithmetic, 200 µs floor | Geometric, raw | Geometric, 200 µs floor |
|---|---|---:|---:|---:|---:|
| `TOP_10` | term | 1.40× | 1.71× | 1.16× | 1.88× |
| `TOP_100_COUNT` | intersection | 1.50× | 1.53× | 1.19× | 1.26× |
| `COUNT` | phrase | 1.48× | 1.50× | 1.35× | 1.45× |
| `COUNT` | intersection | 1.41× | 1.44× | 1.29× | 1.44× |
| `COUNT` | negated | 1.30× | 1.35× | 1.25× | 1.42× |

- Under the ruling's statistic, every class whose floor is under half its time stays
  within 1.45×, and no class reaches 2× on any statistic. The margins on `COUNT` phrase,
  intersection, and negated are within shared-host noise.
- `TOP_10` term is floor-dominated (ruling 6.7) and 1.16× raw. Its gap sits in the
  frequent stratum (df ≥ 100,000): 400 µs against Tantivy's 280 µs, about 2.5× over a
  200 µs floor. Rare and medium terms are within 1.1× raw.
- **Indexing** ran one trial per engine, sequentially, from a dropped page cache.
  - `index_1m.sh` counts segments in the final index. For Lucene, a flush followed by a
    merge also ends at one `.si`. For Tantivy, it counts `meta.json` itself, one file
    whatever the segment count. Neither shows that only one segment was flushed.
  - Lucene ran with a 1 GB RAM buffer, against the design's 2 GiB per engine.
  - Tantivy's margin is 9% over the 0.8× target.
- **The harness checks counts, not hits.** `strata_engine` answers every `TOP_K` command
  with 1 (`strata_engine.cc:189-193`). Nothing at Wikipedia scale shows that pruned
  top-k equals exhaustive top-k.

G5 is what must be re-run before Phase 3 work depends on it:

- Phase 3's indexing ratchet (≥ 1.0× Tantivy) and its multi-thread rows start from the
  indexing row.
- Merges re-encode postings in the format C1 settles.
- Later phases' query targets compare against the re-run's per-class baselines.

The postings size is deterministic. It needs no re-run unless C1 changes the format.

## Blocking items

1. **G1. Decoders memory-safe on corrupt input, bounded by the verified range.**
   - Skip-data offsets are checked against their areas before pointers are formed
     (`reader.hh:280-287, 563-584, 611-612, 497-498`). A group's level-0 bytes are
     consumed exactly, and its entries' byte lengths sum to its level-1 count.
   - `decode_group` requires each entry's last ordinal to exceed the previous one and to
     stay below the document count (`569, 587`): one compare per block, for both block
     kinds.
   - Each reader's end is its term's range end: `info.postings + info.postings_length`
     and `info.positions + info.positions_length` (`252, 735`).
   - A document's position count is bounded by the term's own stream before the output
     is resized (`764-765`).
   - A unit test per check, on segments corrupted and then resealed.
2. **G2. Pin the term index.** Copy `term_index` into owned memory at open
   (`residency::pinned`, `residency.hh:132-152`) and build the FST over the copy
   (`reader.hh:84, 126`), as ARCHITECTURE's pinning list states. `lengths` and `norms`
   may stay mapped: once G1 bounds ordinals, their reads stay in bounds whatever the
   bytes.
3. **G3. Fuzzing that reaches the decoders.**
   - `fuzz_segment` reseals checksums (regions, sections, TOC, footer) after corrupting,
     so mutations reach the decoders. The unsealed mode stays, for detection.
   - Inputs reach multi-group skip data: `-max_len` raised, or each input byte expanded
     into several documents.
   - The corrupted segment runs conjunctions, the disjunction bulk path, phrases,
     exclusions, required-plus-optional, and `count()`, not only term scorers.
   - One CPU-hour at the final format with zero findings, recording executions per
     second, coverage, and corpus size. The library targets record CPU time, not only
     runs.
4. **G4. Phrase bounds when counts differ from wdf.** In Lucene mode, a term without
   `counts_are_wdf` contributes the phrase's unconditional bound, not its wdf pairs
   (`search.hh:787-798, 851-854`). The design's scorer table drops the premise
   (`phase-2.md:233`). A test runs a phrase with a boolean term that carries positions,
   under a positive threshold.
5. **G5. The quiet-host re-run** (Phase 1 review: ruling 6, and Phase 0 open item 3).
   - All five commands and every class, for strata, Lucene, Tantivy, and Xapian, in one
     session on the gate VM with the host idle. Interleaved trials, at least five after
     warm-up, with the median and the 95% bootstrap interval per row (BENCHMARKS rules 3
     and 4).
   - Per class, the geometric mean (ruling 6.2), raw and floor-subtracted with the floor
     measured in that session. Classes where the floor exceeds half the time are flagged
     (ruling 6.7).
   - `COUNT` and `TOP_100_COUNT` classes within 1.5×, as PLAN states. A `TOP_K` class
     above 1.5× carries a cause and a fix plan; the frequent-term `TOP_10` gap carries
     one whatever its class ratio.
   - Pruned against exhaustive top-k on the Wikipedia segment, for every gate query,
     both schemes, at k = 10, 100, and 1,000: equal up to ties within 1e-9 ordered by
     ordinal.
   - Single-thread indexing on 1M documents: at least three interleaved trials per
     engine; flush counts from each engine's own log, and Tantivy's segments from the
     list in `meta.json`; Lucene at the 2 GiB budget, or at its per-thread limit, rather
     than 1 GB.
   - `termdict`: p50 and p99 for present and absent keys, and the hash-map ratio
     (Phase 1 review, ruling 2).
   - Every row records the commit, format version, compiler and flags, and trial count
     (BENCHMARKS, "Reporting").

## Conditions

1. **C1. Format decisions Phase 3 builds on.** Before Phase 3 writes merge code.
   - wdf: the design states raw wdf and its reason, or a per-term flag stores wdf − 1 in
     blocks when every wdf is at least 1. Blocks of ones and twos then save a bit per
     value, and the postings row has 0.2% headroom.
   - docids: the design specifies the section as written; the reader parses and
     validates it at open; it gains the docid → ordinal lookup the design lists; and a
     test covers the non-contiguous kind.
   - A format change re-measures the postings row.
2. **C2. Docid tie-breaks.** Collectors break ties by ordinal and rely on ascending
   arrival to reject an equal score (`search.hh:162, 166, 934`). Once replaced documents
   sit at late ordinals, or hits merge across segments, ties compare docids, as
   ARCHITECTURE states, and so does the equal-score rejection. Before Phase 3 merges
   hits across segments.
3. **C3. Count shortcuts under deletes.** Each statistic count under "Counting
   shortcuts" applies only to a segment without deletions, or subtracts the deleted
   matches, with a test per shortcut on a segment with deletes. With Phase 3's deletes.
4. **C4. One-sided bound arithmetic.**
   - Remaining bounds are summed directly instead of by subtraction
     (`search.hh:441, 1051`).
   - Summed bounds are inflated by a relative error bound, as Lucene's
     `MathUtil.sumUpperBound` does, and `s` minus the optional bound rounds down
     (`633`).
   - A test compares pruned and exhaustive top-k, ordinal for ordinal, on a corpus with
     duplicate documents.
5. **C5. Tests that match what ships.**
   - `parity` keeps the pruned mode this review ran: counts from `count()`, and the top
     10 from `top_k` without a count. Today's mode requests counts, which turns pruning
     off for conjunctions, phrases, and unions of three or more clauses.
   - The oracle's rank vectors gain `AND_MAYBE`, `AND_NOT`, and `NEAR`; today they hold
     `AND`, `OR`, and `PHRASE` only.
   - `search_test` gains Xapian BM25, terms whose counts differ from wdf, and the shapes
     `strata_engine` builds: required-plus-optional over a conjunction, exclusion
     chains, and conjunctions of disjunctions.
6. **C6. NEAR.** A two-phase NEAR scorer with oracle vectors lands before the Phase 3
   gate, or PLAN moves it with a reason. The gate report lists it either way.
7. **C7. WAND.** Nested scoring disjunctions are measured: conjunctions of disjunctions,
   and the intersection_union class at 1.15–1.23× raw. Block-max WAND lands there, or
   ARCHITECTURE's cost-model choice is amended with the measurement. Before the Phase 3
   gate.
8. **C8. The window contract.** `scorer::fill` allows any window the caller's array
   covers (`search.hh:127-131`), while the conjunction and exclusion overrides use
   64-word stack arrays (`349, 691`). The contract states 4,096 ordinals, as one
   constant shared with `count()`, or the arrays take their size from the window.
9. **C9. Hardening and text.**
   - `top_k` with k = 0 and a count reads `heap.front()` of an empty heap
     (`search.hh:949`).
   - `postings_iterator::fill` on an unstarted iterator reads `buf_ords_[buf_len_ - 1]`
     with `buf_len_` at 0 (`reader.hh:469`). It starts the iterator first, or asserts.
   - The writer accepts any document count (`writer.hh:225`). Ordinals must stay below
     `no_more_docs`, and `count()`'s window end wraps past 2^32 − 4,096
     (`search.hh:972`). The writer caps the count, and the reader checks it at open.
   - The reader rejects duplicate section kinds and overlapping sections at open
     (`format.hh:198-212`).
   - Each stored record length is checked as it is read (`reader.hh:174-180`). The
     stored section records its largest frame size, which bounds a frame's allocation
     (`159, 166`).
   - The design's collectors section still describes a heap and a count triple
     (`phase-2.md:258-265`). `writer.hh`'s layout comment lists per-block length tables
     that are single totals, and omits the position and count offsets
     (`writer.hh:26-37`).
10. **C10. Concurrent readers under TSan.** `verified_regions` lets queries on several
    threads verify one segment's regions (`residency.hh:154-192`). A TSan test runs
    several threads querying and reading stored records from one segment. Before Phase 3
    shares segments across threads.
11. **C11. Term dictionary size.** The `terms` section is 2.5× Lucene's `.tim` (100.6 MB
    against 39.5 MB). It is broken down (keys, term info, frontiers, offsets) and given
    a target before Phase 3's primary-key lookups depend on it.

Carried to Phase 6: the asynchronous `residency` backends verify regions as they load
them, through the segment's `verified_regions` state, and the reader prefetches the
ranges it decodes.
