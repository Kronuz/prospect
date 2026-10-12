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

## Re-check

- Date: 2026-10-12.
- Reviewed: G1–G4 at strata `4edad0a` (`771634f`, `e059d64`, `2731bb2`, `4e756bc`,
  `4edad0a`), from the code and the diffs against `9204851`. Nothing was built or run
  for this check; the fuzz figures come from `.scratch/fuzz-segment-v9.log`.

### Verdict

**Not approved yet.** G2 and G4 close. G3 closes in substance, but its record is
incomplete. G1 stays open on one residual, and the phrase path has a new blocking
defect, G6. G5 waits for its trials.

### Status

1. **G1. Open on one residual.**
   - Closed:
     - Both readers stop at their term's verified range (`reader.hh:260, 753`).
     - Group lasts increase and stay below the document count, in 64-bit arithmetic
       (`277`).
     - Block lasts increase within their group and end at its last (`585`).
     - Block byte lengths are nonzero and fill their group exactly
       (`576-577, 601, 605`), so bitset blocks cannot emit ordinals past the document
       count.
     - A positions read rejects a count above the segment's maximum before it allocates
       (`784`).
   - Open: the level-1 sums are checked only in total (`288-291` against `295, 297`). A
     sum that wraps past 2^64 leaves a later group's level-0 or block offset outside the
     term's range. `decode_group` trusts it (`571-577`), and so does `load_next_block`,
     which relies on `decode_group` for a block's bounds (`630`). Rejecting each addend
     larger than the room left in the term's range closes it.
   - No unit test covers these checks; the resealing fuzzer covers them. A resealed test
     comes with the wraparound fix.
2. **G2. Closed.** The term index is copied out of the mapping, and `termdict` validates
   and reads the copy (`reader.hh:129-130`). The checksum runs on the mapped bytes
   before the copy (`77`). Validating the copy keeps lookups in bounds; checking the
   checksum on the copy, as `residency::pinned::copy` does, would also close that
   window for content.
3. **G3. Closed in substance; the record is incomplete.**
   - `fuzz_segment` recomputes every checksum for seven inputs in eight
     (`fuzz_segment.cc:49-59, 121`). Its bases reach several skip groups (`43`), and it
     runs every scorer, `count()`, and stored reads on the corrupted segment (`61-106`).
   - The format-9 hour is clean: 107,932 runs in 3,601 seconds, 29 per second, 3,941
     PCs, a 933-input corpus.
   - The gate report owes those figures, and the library targets' CPU time: it lists the
     library targets by runs (`docs/gates/phase-2.md:116-118`).
   - Follow-up: mutations stay inside sections (`fuzz_segment.cc:115-120`), so the TOC
     parser's new duplicate and overlap checks (`format.hh:218, 224`) only see
     well-formed tables.
4. **G4. Closed, except one line of design text.** Lucene-mode bounds use only terms
   whose counts are their wdf (`search.hh:805, 862`). A test leads a phrase with a
   positioned wdf-0 term and compares pruned with exhaustive top-k
   (`search_test.cc:176-196`). The design's phrase row states the wdf premise
   (`phase-2.md:233`).

### New blocking item

**G6. Phrases over a term without positions.**

- A term indexed without positions, with any wdf of at least 1, is flagged
  `no_positions` without `counts_are_wdf` (`writer.hh:246, 314`).
- Its `positions_reader` returns before reading anything (`reader.hh:748`), yet a phrase
  asks it for counts (`search.hh:879`).
- For a document in the term's tail, which holds every document of a term with df under
  128, `count()` indexes `count_tail_.data()` of an empty vector (`reader.hh:773, 801`),
  so a valid segment crashes the process. For a document in a full block, it throws
  `corrupt_segment` (`803`) and fences a valid segment.
- Neither the fuzz vocabulary (`fuzz_segment.cc:17-33`) nor the tests hold a
  positionless term.
- The fix belongs in `positions_reader`, so NEAR (C6) inherits it: a term without
  positions has a count of 0 and no positions, and a phrase containing it matches
  nothing. A test runs such a phrase over tail and block documents.

### Conditions

- C4 stays open in one place: `rest_bound_` is `sum` minus the lead's bound
  (`search.hh:447`). When the lead's window bound dwarfs the rest, the rounding of `sum`
  exceeds a `sum_upper` margin taken on the lead's score plus the rest. Summing the
  other children's bounds directly closes it. The duplicate-document tie test is owed.

## Second re-check

- Date: 2026-10-12.
- Reviewed: G1–G6 and C4, C7, C9–C11 at strata `cb01c65`, from the code and the diffs
  against `4edad0a`: `2542867`, `5fa66a4`, `e073258`, `523438b`, `e5fafec`, `662558f`,
  `7f48e42`, `03e2544`, `24429ec`, `69813ba` (format 10), `64623d0` (format 11),
  `5ec6c0d`, and `cb01c65`. The Phase 3 design commits (`474b29c`, `b2ffe24`, `d885f7a`)
  are outside this gate. Nothing was built or run; fuzz figures come from
  `.scratch/fuzz-segment-v9b.log` and `.scratch/fuzz-segment-v11a.log`.

### Verdict

**Not approved yet.** G2, G4, and G6 close. Format 10 removes G1's level-1 sums, but G1
stays open on a position index that wraps past its check and on its resealed tests. Two
new defects block: G7, a pruned term query that loops forever on a skipped group's
corrupt last ordinal; and G8, a disjunction of up to four clauses that drops its
remaining matches once a parent's minimum splits it, on valid segments. G3 waits for the
format-11 hours, G5 for its trials, and the postings row for a format-11 measurement.
Phase 3 implementation starts at approval.

### Status

1. **G1. Open.**
   - Closed:
     - Level 1 is fixed-width, so no offset is a sum. The header checks the level-1 array
       and the pairs, level-0, and block areas against the term's range before forming a
       pointer (`reader.hh:287-303`). `read_group` takes a group's level-0 and block
       ranges from its own entry and the next one, requires them ordered and inside their
       areas, and requires its last above its predecessor's and below the document count
       (`583-601`). `group_impacts` checks its pair range (`403-408`).
     - `decode_group` keeps block lasts increasing up to the group's last, and block bytes
       nonzero and filling the group (`620, 636, 640-641`).
     - The lazy bitset path is bounded. `load_bitset` caps a block at its entry's bytes
       and at `bits_`'s 512, masks the last word, and requires 128 bits with the block's
       last ordinal among them (`715-737`), so `ords()` and `advance_bits` stay in the
       loaded words and stop at that bit (`740-764`). The cap rejects no valid block: the
       writer picks a bitset only when it is smaller than the FOR encoding
       (`writer.hh:500`).
   - Open:
     1. **The position index wraps past its check.** `read` compares `first + count` with
        the term's stream (`reader.hh:880`), and `first` sums unchecked file values: the
        tail's first index (`283`), a group's (`593`, 64-bit when wide), level-0 deltas
        (`627`), and counts (`451`). With `first` within `count` of 2^64 the sum wraps,
        the check passes, neither loop runs (`889, 895`), and `read` returns `count`
        values that `make_unique_for_overwrite` left uninitialized, or a previous
        document's (`881-884, 896`). Fix: reject `first > limit || count > limit - first`.
     2. **No resealed unit test.** G1 asks for one per check; `segment_test` still
        corrupts without resealing (`segment_test.cc:131-166`). `refresh_checksums`
        (`fuzz_segment.cc:50-60`) becomes a test helper, and tests cover at least a
        level-1 offset past its area, a bitset block without its last bit, a FOR block
        reaching the document count, the wrapped position index, and G7's lowered last.
     3. `decode_group` does not require a group's level-0 bytes consumed exactly, as G1
        asks (`reader.hh:640`): one compare.
2. **G2. Closed** (`reader.hh:132-135`, unchanged).
3. **G3. Open until the format-11 hours; the format-9 record is complete.**
   - The table-of-contents mode the first re-check asked for is in: one input in eight
     changes the table and recomputes only its checksum (`fuzz_segment.cc:131-135`), so
     the duplicate and overlap checks see malformed tables (`format.hh:214-225`).
   - The target reaches the new code: dense terms produce bitset blocks; the sequential
     walk expands them (`ords`); conjunctions, unions, phrases, and required-plus-optional
     advance inside pending blocks (`advance_bits`); counted conjunctions OR whole bitset
     blocks (`or_bitset`); Lucene-mode phrases run the existence check and the frequency
     path.
   - It does not reach NEAR or Xapian-weighted phrases (`fuzz_segment.cc:111-115` passes
     neither weights nor a window), valid wide level-1 entries (every base is narrow,
     `44`; a flipped width byte reaches the wide reads only with garbage entries), or
     G8's shape.
   - The format-9 "re-run after the positionless-term and table-of-contents additions"
     (`gates/phase-2.md:118-119`) stopped at 36,489 runs (`fuzz-segment-v9b.log`), and
     the format-11 run is 10 minutes, 45,298 runs (`fuzz-segment-v11a.log`). Neither is
     the hour; what it must show is under "What approval needs".
4. **G4. Closed.** The design's phrase row bounds by position counts and states that a
   positionless term matches no phrase (`phase-2.md:237`).
5. **G5. Pending** (under "What approval needs").
6. **G6. Closed.** A term flagged `no_positions` has count 0 and no positions
   (`reader.hh:833-836, 860, 873`). The phrase existence check leads with that slot and
   finds nothing (`search.hh:918-946`); NEAR returns 0 when a term has fewer positions
   than it repeats (`974`), inheriting the fix from the reader. The test runs a phrase and
   a counted NEAR over 300 documents, two full blocks and a 44-document tail
   (`search_test.cc:248-265`).

### New blocking items

**G7. A pruned term query loops forever on a skipped group's corrupt last ordinal.**

- Format 10 checks a group's last only when the group decodes, and only against its
  predecessor (`reader.hh:597`); format 9 checked every last at open.
- `skip_blocks` advances to `group_last(g − 1) + 1` (directly, or as `group_first(g)`),
  where group g − 1 is one it skipped without decoding (`search.hh:299-301`). When that
  last is below `d`, `advance` returns `d` unchanged (`reader.hh:321`), `group_of(d)`
  returns the same group, and the loop repeats (`search.hh:294-302`).
- It needs a resealed level-1 entry and a threshold under which the group holding `d` and
  the next one cannot compete while a later group or the tail can. `collect_top_k`
  reaches it (`226-236`), as do a required side and an exclusion's include side. At
  libFuzzer's default 1,200-second timeout, such an input surfaces after 20 minutes.
- Fix: `skip_blocks` throws `corrupt_segment` unless its target exceeds `d`
  (`search.hh:301`); on valid skip data, the group before the target ends at or past `d`.
  Checking every level-1 last at open, one strided 32-bit load per group (1,018 for
  `the`), also closes it. A resealed test lowers a skipped group's last below `d` and runs
  a pruned `top_k`: it throws.

**G8. A disjunction of up to four clauses drops its remaining matches when a minimum
splits it.**

- Linear mode (`523438b`) advances the children directly and never fills the heap
  (`search.hh:605-614`).
- When `set_min_competitive_score` then makes a clause non-essential, `essential_from_`
  rises, `linear()` stays false from then on, and the heap stays empty (`511-521`). The
  next `next()` or `advance()` pops nothing and returns `no_more_docs` (`538-541,
  548-551`), so pruned top-k keeps only the best hits before the split. Valid segments
  trigger it.
- Under `top_k`'s generic loop the minimum reaches a disjunction through an exclusion's
  include side (`711`) and a required side (`665`). `strata_engine` builds `a b -c` as an
  exclusion over a two-clause disjunction (`tools/strata_engine.cc:166-176`), and
  `parity` builds `AND_NOT` and `AND_MAYBE` over whatever left side the query has
  (`tools/parity.cc:125-143`).
- No gate run sees it: the 19 negated benchmark queries are `+a -b [-c]`, every oracle
  `AND_NOT` and `AND_MAYBE` has a single term on the left, and `search_test` builds
  disjunctions only as the optional or excluded side (`search_test.cc:131-136`). Parity's
  last recorded pass (419 of 419, `e073258`) predates `523438b`.
- Fix: when `set_min_competitive_score` raises `essential_from_` on a started disjunction
  whose heap is empty, it pushes each essential child at its current `doc()`
  (`search.hh:515-520`). A test compares pruned with exhaustive top-k for an exclusion
  over a disjunction and for a disjunction as the required side, with one clause's bound
  under the k-th score (C5's `strata_engine` shapes); `parity` and `verify` re-run on the
  fix.

### What approval needs

- **Fuzzing**, on the commit that closes G1, G7, and G8:
  - The segment target: one CPU-hour under ASan and UBSan, RSS limit 2 GB, with a
    `-timeout` of seconds rather than the default so a loop is a finding; zero findings;
    executions per second, PCs, corpus size, and CPU time recorded. `exercise` first adds
    a NEAR scorer, a Xapian-weighted phrase, a pruned `top_k` over an exclusion of a
    two-clause disjunction, and a base written with `wide_skip_offsets`.
  - PLAN asks an hour per decoder and parser added in the phase. The gate report gives
    `unicode` 5 CPU-minutes, `snowball` 30, `analysis` 30, and `bitpack`'s
    `encoded_size` 5 (`gates/phase-2.md:119-121`): each reaches an hour, or the report
    says which predate Phase 2.
- **Tests**: all four pass under ASan and UBSan, and `concurrency_test` under TSan
  (PLAN: TSan where threads are involved), at that commit, recorded in the gate report.
- **G5.** The first review's list stands. In addition:
  - `gate.py` reports per row the median and 95% bootstrap interval across trials, the
    raw geometric mean beside the floor-subtracted one, and the flag for classes whose
    floor exceeds half their time, and it scores the Xapian rows. Today it prints the mean
    and range of floor-subtracted geometric means against Lucene and Tantivy only
    (`bench/phase2/gate.py:19, 44-64`). It takes the floor as the smallest time over every
    trial (`cb01c65`); the report states that floor and the trial it came from.
  - `strata_engine verify` re-runs on the commit that closes G7 and G8. Trials taken at
    `cb01c65` count if that commit changes no measured path beyond the added checks and
    states its in-process term and phrase timings against `cb01c65`.
- **The record at format 11:**
  - The postings row is re-measured: the gate report holds format 8's 1.018×
    (`gates/phase-2.md:16`), and `64623d0` gives 1.026× for format 10 and no figure for
    format 11. Format 11 holds ≤ 1.02×.
  - Ruling 4: a test that a version-10 file is rejected. `small.sx` was rewritten at
    formats 9, 10, and 11 (`771634f`, `69813ba`, `64623d0`), and no test rejects or
    reads an earlier version.
  - The gate report says format 9 for the fuzz hour and the fixture (`117, 122`), and its
    format list stops at 8 (`133`). Its "Also closed" line (`98-102`) overstates C4 and
    C9.

### Conditions

- **C4. Open in two places.** `rest_bound_` sums the other children's window bounds
  (`search.hh:457-459`), and the tie test returns ordinals 0–9 over 5,000 identical
  documents for a term, a union, and a conjunction, pruned and counted
  (`search_test.cc:218-247`). The original C4 also names:
  - The windowed MaxScore's `remaining -= bound[order[e]]` (`search.hh:1161`). Its
    rounding error is relative to `rest`, not to what remains, so it can exceed
    `sum_upper`'s margin when the consulted bound dominates. Prefix sums of the
    non-essential bounds, formed lowest first as `rest` is, close it.
  - `s - opt_->max_score_total()` rounds to nearest, not down (`665`); one `nextafter`
    toward −∞ closes it.
  - The tie test cannot show either: identical documents only tie the threshold, where
    pruning cannot change the result.
- **C7. Closed for `AND_MAYBE`; open for conjunctions of disjunctions.** ARCHITECTURE
  records the measurement and the choice (`ARCHITECTURE.md:173-178`). Conjunctions of
  disjunctions, where a disjunction can lead, are unmeasured, and the amended text defers
  WAND to exactly that shape; the measurement is owed before the Phase 3 gate.
- **C9.**
  - Closed: `top_k` with k = 0 (`search.hh:1040-1043`); `fill` and `for_each_until` on
    an unstarted iterator (`reader.hh:468, 487`); duplicate and overlapping sections
    (`format.hh:214-225`); stored record lengths checked as read (`reader.hh:192-197`).
  - Open: the writer caps a segment at 2^32 − 2 documents (`writer.hh:229`), and
    `count()`'s window end `base + 4096` wraps to 0 for ordinals at or past 2^32 − 4,096
    (`search.hh:1082`); `fill` then sets nothing, and the loop never ends. The cap
    becomes 2^32 − 4,096, checked at open, or `count()` clamps its end as `collect_top_k`
    does (`1121`).
  - Open: a frame's allocation is bounded only by its 32-bit `raw_size`
    (`reader.hh:175, 182`), up to 4 GiB; the stored section records its largest frame
    size.
  - Open, text: the design's collectors section still describes a heap and Xapian's
    count triple (`phase-2.md:262-269`); its skip-data text gives format 10's 64-bit
    offsets and length-prefixed pairs (`114-124`), not format 11's width flag, 32-bit
    offsets, and count-headed compact pairs; `writer.hh:26-37` describes format 9's
    level 1, and `writer.hh:536` runs to 131 columns.
- **C10. Closed in code.** Eight threads run term, counted-union, and phrase queries and
  stored reads on one segment while its regions verify lazily, and each digest equals a
  single thread's (`concurrency_test.cc:33-61`). The TSan run appears only in
  `7f48e42`'s message (see "Tests" above).
- **C11. Closed.** The gate report breaks the term dictionary down and sets ≤ 1.5× `.tim`
  for Phase 3 (`gates/phase-2.md:144-151`). Its parts sum to 27.7 bytes a term, which
  over 3,964,752 terms exceeds the 100.6 MB section; it states whether keys count raw or
  as stored.
- **C12. Exception-safe skip-group state** (new). `decode_group` overwrites `entries_`
  before validating and sets `decoded_group_` only at the end, so after a throw it still
  names the previous group over half-decoded entries (`reader.hh:605-609, 642`). It
  invalidates `decoded_group_` before decoding. Before Phase 3 fences one segment and
  keeps serving others.
- Not re-checked here: C1–C3, C5, C6, C8.
