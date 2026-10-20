# Format 13's remaining query losses, attributed

At 5M on the gate VM, strata (main, reordered) loses rows to every baseline
(`format13-gate-vm.md`, row 4b). This file attributes each loss class with counters, profiles,
and prototypes. The owner's decision of 2026-10-20 (PLAN): no format freeze until the first
release; format 13 stays a rolling pre-release format, and a format change lands as any change
does, on proof. So each cause is classified as a format change now (when it wins), a format change
later, or format-free, and the verdict ranks them by expected gain.

## Plan (written before the runs)

- **Classes** (5M, harness, five trials on S′; strata time over the engine's):
  1. Counted unions of more than three terms (`COUNT`): 2.30 times slower than Lucene, 2.77
     than Lucene `-bp`; `TOP_100_COUNT` 1.09 and 1.16.
  2. `TOP_100_COUNT` unions of three or more terms: 1.71 and 1.97 times slower than Tantivy.
  3. intersection_union (one required term, one optional): 1.11 to 1.41 times slower than
     Lucene `-bp` at every command but `COUNT`; 1.08 to 1.24 than Tantivy at `TOP_100`,
     `TOP_1000`, `TOP_100_COUNT`.
  4. Phrases: 1.10 to 1.21 times slower than Tantivy at every command, with FQ3's positional walk
     (the positional deltas are found by walking each group's impact lists).
  5. Against Tantivy also: intersections at `TOP_1000` (1.15) and `TOP_100_COUNT` (1.18),
     negations at `TOP_1000` and two-term negations at `TOP_100_COUNT`, the two-phase-critic.
  6. PISA's 27 comparable rows: unions of two and three terms and intersections at the top-k
     commands, counted intersections of three or more.
- **Hypothesis per class:** none fixed in advance; each class gets its counters and profile first.
- **Procedure.**
  1. A counting build: main with perf/query-counters merged (its hooks ported to main's reader),
     plus counters for the impact-list walks and position blocks (`.scratch/wt-cnt`; not for
     landing). `strata_engine bench` on the 1M reordered index (identical bytes to main's), each
     class's gate queries, both presets where they differ.
  2. A profile per class: a Release build with symbols, `sample` over a looping bench.
  3. Where a mechanism is suspected, a scratch A/B of the format-free change on the 1M index under
     a light hold, one thread, A/A first, so the expected gain is measured, not guessed.
  4. The baselines' code read for how they do the same work (Tantivy 0.26, Lucene 10.4, PISA
     0.8.2; Apache 2.0 and MIT, so nothing ported here needs more than a notice).
  5. FQ3: the positional walk's share of phrase time from the counters and the profile, and a
     scratch build that stores a level-1 positional offset, timed against main on phrases.
- **Classification.** Format-dependent when closing the loss needs bytes, a layout, or a reserved
  place; format-free when an algorithm change over format 13's bytes closes it. Each row names the
  place (or "none") and the expected gain with its source. (Revised after the owner's decision of
  2026-10-20: format-dependent causes are a format change now, if it wins, or later.)

## Setup

Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, Release, one thread under light holds.
Index: the 1M reordered index from the seeds item's identity run (`.scratch/idx/id-1000000-c-reordered`,
reordering merge at two threads), byte-identical to main's. Queries: the 962 gate queries split by
class (`.scratch/q/`). Builds:
- **Counting** (`.scratch/wt-cnt`, `-DSTRATA_COUNTERS=ON`): main `06a730b` with perf/query-counters
  `750ce06` merged, its reader hooks ported to format 13's reader (group loads, FOR and bitset
  decodes, bitset ORs), plus three counters: `pair_walks` (a group's impact lists walked),
  `positional_groups` (a group's positional deltas read), `position_blocks` (position blocks
  decoded). Never timed.
- **Profile** (`.scratch/wt-b/build-prof`): main with `-g -fno-omit-frame-pointer`; `sample` over
  `strata_engine bench` looping on one class for 8 s (about 6,000 samples), self time.
- **Prototypes**, each checked before timing: `.scratch/wt-ff` by `verify` (pruned top-k against an
  exhaustive scan, both presets, k 10, 100, 1,000; 0 mismatches over the 962 queries) and `dump`
  (top 10, 100, 1,000 with counts, the counted union path included) byte-identical to main's over
  the 962; `.scratch/wt-pos` by `dump` of the 300 phrases, byte-identical.
- A/Bs: `bench/quick/ab.sh`, A/A then A/B, scored at the run level by perf/query-exp's
  `bench/quick/ab.py --aa` (`ab-run-level.md`): a win needs its upper bound under 1.0 and the
  A/A band, at 0.97 or less.

Per-class counters at 1M (per query; every class and command in `.scratch/cnt1/`):

| Class, command | µs (mean) | Counters per query |
|---|---:|---|
| union > 3, `COUNT` | 1,280 | 3,584 FOR blocks decoded, 7,904 bitset blocks OR'd, 220 windows |
| union > 3, `TOP_100_COUNT` | 15,181 | 991,994 postings scored, 504,604 candidates, 1,294 kept |
| union > 3, `TOP_100` | 1,905 | 63,670 scored, 3,495 candidates |
| union 3, `TOP_100_COUNT` | 4,359 | 324,767 scored, 295,765 candidates |
| intersection_union, `TOP_10` | 794 | 22,207 scored, 14,689 candidates, 0 rejected by a bound, 2 blocks skipped, 112 kept |
| phrase 2, `COUNT` | 320 | 3,945 advances, 516 position blocks, 10 pair walks = 10 positional groups |
| phrase 3, `TOP_10` | 400 | 5,720 advances, 382 position blocks, 30 pair walks for 30 positional groups |
| intersection 2, `TOP_1000` | 157 | 3,663 advances (3,462 in block), 771 candidates |
| intersection > 3, `COUNT` | 319 | 648 FOR blocks, 877 bitset blocks OR'd |
| negated, `TOP_1000` | 211 | 1,545 candidates, 1,878 advances of the excluded side |
| two-phase-critic, `COUNT` | 9,129 | 47,085 advances, 22,004 position blocks |

Profiles (share of self samples; `.scratch/prof/`):

| Class, command | Where the time goes |
|---|---|
| union > 3, `COUNT` | `fill`'s FOR path (decode gap - 1 blocks to ordinals, then one bit each) 54%; `block_at` (each block's entry checks) 13%; `load_next_block` 9%; `or_bitset` 9%; copies 6% |
| union > 3, `TOP_100_COUNT` | the window loop (sum over clauses, collect) 49%; `score_window` (decode and score every posting) 41% |
| intersection_union, `TOP_10` | `score_of` 19%; `required_optional_scorer::next` 11%; `advance` 11%; `skip_blocks` 7%; the required side's every document is scored |
| phrase 2, `COUNT` | postings `advance` 21%; `positions_reader::read` 21%; `phrase_freq` 16%; `find` 9%; PFOR decode 4%; residency verification 3.7%; `load_positional` with its impact-list walk 2.1% |
| intersection 2, `TOP_1000` | the conjunction's loop 34%; `advance` 25%; the collector's truncation (partition, sort, `nth_element`, docid reads for ties) 14% |
| intersection > 3, `COUNT` | `fill` 37%; `block_at` 11%; `load_next_block` 9%; `advance` 9%; `or_bitset` 8% |
| union 2, `TOP_10` | the window loop 42%; `score_window` 18%; `advance` 8% |

## Measured format-free changes

`.scratch/wt-ff` (not for landing; 1M, six rounds, A/A first):
1. **Counted union top-k prunes like top-k.** Counting kept every clause essential and scored every
   match. The prototype counts from window bits filled by fresh copies of the clauses and lets the
   essential split use the collector's threshold, as top-k does.
2. **Required plus optional skips blocks** (Lucene's `ReqOptSumScorer`): a required block whose
   bound plus the optional side's over it cannot compete is skipped, and one whose required bound
   alone cannot makes the optional side required, leapfrogging the two.

| Row (B/A [95% over runs]; A/A) | Result |
|---|---|
| `TOP_100_COUNT` union:num_tokens_3 | **0.478** [0.462–0.493] win (A/A 0.998) |
| `TOP_100_COUNT` union:num_tokens_>3 | **0.364** [0.354–0.374] win (A/A 1.005) |
| `TOP_100_COUNT` union:num_tokens_2 | 1.004 (two clauses take the exact count, untouched) |
| intersection_union `TOP_10` | **0.820** [0.758–0.886] win |
| intersection_union `TOP_100` | **0.879** [0.847–0.914] win |
| intersection_union `TOP_100_COUNT` | **0.876** [0.846–0.908] win |
| intersection_union `TOP_1000` | 1.001 [0.922–1.087] neither |
| Every other union row, every `COUNT` row | neither (0.98 to 1.03; the `TOP_100` union rows' 1.02 is on code the prototype leaves unchanged, against an A/A of 0.98) |

FQ3's layout, measured (`.scratch/wt-pos`): the writer stores each group's offset of its
positional deltas in level 1 (one more offset per group, u32 or u64 as the others), and
`load_positional` jumps there without walking the impact lists. Phrases, 1M, ten rounds:

| Row | B/A [95%] | A/A [95%] |
|---|---|---|
| phrase `COUNT` | 0.988 [0.982–0.994] | 1.005 [0.996–1.016] |
| phrase `TOP_1000` | 0.983 [0.960–1.007] | 1.005 [0.986–1.024] |
| phrase `TOP_10`, `TOP_100`, `TOP_100_COUNT` | 1.006, 0.993, 1.016 | 0.999, 0.995, 1.016 |

The offset saves 1% to 2% of phrase time (the profile's 2.1%: one walk per positional group,
and a walk is 32 sized lists), for 253,760 bytes at 1M (0.044% of the segment; about 1.1 MB of
postings at 5M by the postings ratio, against D15's 9.96 MB miss). Results identical (dump of the
300 phrases).

## The classes

Ratios are the 5M gate's (strata over the engine, row 4b of `format13-gate-vm.md`); the 1M runs
here attribute them. How each baseline does the work is from its source: Lucene 10.4.0
(`Lucene104PostingsWriter`, `Lucene104PostingsReader`, `BooleanScorer`, `BooleanWeight.count`,
`BooleanScorerSupplier`, `ReqOptSumScorer`), Tantivy 0.26 (`positions/reader.rs`,
`postings/segment_postings.rs`, `postings/skip.rs`, `query/phrase_query/phrase_scorer.rs`,
`query/union/buffered_union.rs`), and PISA's harness as `phase-3-ratchets.md` records it. Nothing
was ported.

1. **Counted unions of more than three terms, `COUNT`** (2.30 times slower than Lucene, 2.77 than
   `-bp`; 0.57 of Tantivy's time, so Lucene alone leads). Both count the same way: 4,096-document
   windows, each clause OR'd in (Lucene's `BooleanScorer` with `intoBitSet`), then a popcount. The
   difference is per block. Strata decodes a FOR block to ordinals (gap - 1, prefix sum, the 64-bit
   sum check) and sets one bit per posting, and checks every block's entry (`block_at`) and loads
   it: 54% plus 22% of the time, with 3,584 FOR blocks per query against 7,904 bitset blocks OR'd
   (9%). Lucene 10.4's blocks are 256 documents; a block is a bitset whenever that is smaller than
   FOR at one more bit per value (biased toward bitsets), and a run of 256 consecutive documents
   takes no payload; frequencies are skipped unread; its FOR decode and set loop vectorize.
   **Format-free**: decode a FOR block straight into the window's bits, check entries per window
   rather than per block, and (a writer rule over the codes format 13 has) bias the bitset choice
   as Lucene does. The 256-document block is a format choice that the profile does not ask for:
   the per-block share (22%) falls as well by checking per window. Expected: removing the per-block share
   (22%, plus 6% of copies) alone is at most about 1.4 times; the rest depends on how much of the
   FOR path's 54% a fused decode removes. **Not measured** here.
2. **`TOP_100_COUNT` unions of three or more terms** (Tantivy 1.71 and 1.97; Lucene 1.09 and
   `-bp` 1.16 for more than three). Counting turned pruning off: every match was scored and offered
   (992k postings scored per four-term query at 1M, 12 times the time of `COUNT`). Tantivy scores
   every match too (`BufferedUnionScorer`), just cheaper per match. **Format-free, measured**: count
   from window bits and prune scoring as top-k does, 0.478 (three terms) and 0.364 (more than
   three) of main's time; by the 5M ratios, about 0.82 and 0.72 of Tantivy's and 0.40 to 0.42 of
   Lucene's and `-bp`'s.
3. **intersection_union** (`-bp` 1.11 to 1.41 at every top-k command, Lucene about 1.0, Tantivy 1.08
   to 1.24 at `TOP_100`, `TOP_1000`, `TOP_100_COUNT`). `required_optional_scorer` scores every
   required document (0 rejected by a bound at `TOP_10`, 14,689 candidates for 112 kept). Lucene's
   `ReqOptSumScorer` skips required blocks whose two bounds cannot compete and turns the optional
   side required where the required bound alone cannot; `-bp`'s reordering makes those skips pay,
   which is why only `-bp` leads. **Format-free, measured**: the same two rules give 0.820, 0.879,
   and 0.876 at `TOP_10`, `TOP_100`, `TOP_100_COUNT` (projected 1.19, 1.21, 0.97 of `-bp` from the
   5M query-level ratios 1.45, 1.38, 1.11);
   `TOP_1000` does not move (1.001), where the threshold is low. What remains is the windowed
   MaxScore over the two sides that the union path already has (format-free, not measured).
4. **Phrases** (Tantivy 1.10 to 1.21 at every command). The time is the postings intersection
   (21%), reading positions (21%, with PFOR decode 4% and stepping over block headers), the
   matching (16%), residency verification of each read (3.7%), and FQ3's walk (2.1%). Tantivy
   locates a document's positions by a running position offset from its skip entries and the
   term frequencies before it in the block, and skips position blocks by a per-block bit-width
   array (plain bit packing, no exceptions). Strata has the same offsets, but a group's positional
   deltas follow its impact lists in the record, so reading them walks the lists. **FQ3's part is
   format-dependent and measured at 1% to 2%** (the level-1 offset above, 0.988 at `COUNT`, 0.983 at
   `TOP_1000`), for 0.044% of the segment. The rest is format-free: verify a position block once per
   group rather than per read, the matching loop, and the intersection (class 5); its gain is not
   measured.
5. **Intersections against Tantivy** (`TOP_1000` 1.15, `TOP_100_COUNT` 1.18). Tantivy scores every
   match with no block-max for intersections; strata's block-max does not help at `TOP_1000`, so the
   per-match cost shows: the conjunction loop (34%), `advance` (25%), and the collector's truncation
   (14%: `nth_element` over 2k entries, reading docids for ties in a reordered segment).
   `TOP_100_COUNT` scores every match (counting). **Format-free**: a collector that keeps a
   threshold-filtered buffer and resolves docid ties only for the final k (about 14% at
   `TOP_1000`, from the profile), and class 2's counting rule for conjunctions (count with `fill`'s
   AND, score only competitive documents). Not measured.
6. **Negations** (Tantivy: `TOP_1000` 1.10; two-term `TOP_100_COUNT` 1.27). Every candidate advances
   the excluded side once (1,878 advances for 1,545 candidates at `TOP_1000`). **Format-free**:
   exclude by window bits, as `count()` already does (`exclusion_scorer::fill`). Not measured.
7. **two-phase-critic** (one query, `+"the who" +uk`, Tantivy 1.21 to 1.30): a phrase of two
   stopwords, 22,004 position blocks decoded per query at 1M. Class 4's costs; FQ3's offset is
   part of it. Format-free apart from that.
8. **PISA's comparable rows.**
   - Unions of two and three terms at `TOP_10` (1.39): exact on both sides (PISA's seed is safe at
     k = 10). PISA's lead is block-max data over 40-posting blocks, quantized integer scores, and
     block-max WAND (`phase-3-ratchets.md`). **Format-dependent, with places**: F12's variable
     block-max partitions (skip flags bit 3, reserved and refused today) and P20's quantized impacts
     (structure bit 11, kind 21). The window loop's own cost (42% of union `TOP_10`) is format-free.
   - Unions at `TOP_100` and `TOP_1000` (up to 2.40): **not comparable**. PISA seeds every k with
     its 10th-best score, so at k = 100 and 1,000 it may drop true hits.
   - Intersections at `TOP_100` and `TOP_1000` (1.25 to 1.44): PISA's ranked AND adds quantized
     integer scores over SIMD-BP128 blocks. **Format-dependent part: P20's quantized scores (bit
     11, reserved)**; H22's codec verdict was no format change. The rest is class 5's collector and
     advance.
   - `TOP_100_COUNT` (1.43 to 1.80): **not comparable**: PISA's `TOP_100_COUNT` counts and ranks
     nothing.
   - `COUNT` intersections of three or more (1.09, 1.25): class 1's per-block costs (`fill` 37%,
     `block_at` 11%, `load_next_block` 9%); format-free.

## Verdict, ranked by expected gain

Measured gains first (1M prototypes, run level, A/A first), then estimates from profile shares.
"Format change now" would need the change to win its rows (D14's 0.97 with no row lost); none
here does, so none is proposed now.

| Rank | Loss class (5M, strata over the engine) | Cause (1M counters and profile) | Kind | Place or mechanism | Expected gain (source) |
|---:|---|---|---|---|---|
| 1 | `TOP_100_COUNT` unions ≥ 3 terms (Tantivy 1.71, 1.97; Lucene 1.09, `-bp` 1.16) | Counting turned pruning off: every match scored (992k per four-term query) | Format-free | Count from window bits filled by separate iterators; prune scoring as top-k does | **2.09 and 2.75 times faster** (0.478, 0.364; measured) → about 0.82 and 0.72 of Tantivy's time |
| 2 | Counted unions > 3 terms, `COUNT` (Lucene 2.30, `-bp` 2.77) | FOR blocks decoded to ordinals then set bit by bit (54%); per-block entry checks and loads (22%) | Format-free | Decode FOR straight into window bits; check entries per window; a writer rule biased toward bitsets (Lucene's: smaller than FOR at one more bit), over format 13's existing codes | Up to about 1.4 times from the per-block share alone; more from the fused decode (profile; **not measured**). The largest remaining gap |
| 3 | intersection_union (`-bp` 1.11 to 1.41; Tantivy 1.08 to 1.24) | Every required document scored: no block skip, no switch to a conjunction | Format-free | Lucene's `ReqOptSumScorer` rules (measured); then windowed MaxScore over both sides for `TOP_1000` | **1.14 to 1.22 times faster** at `TOP_10`, `TOP_100`, `TOP_100_COUNT` (measured); `TOP_1000` unmoved |
| 4 | PISA: unions of 2 and 3 terms at `TOP_10` (1.39); intersections at `TOP_100`, `TOP_1000` (1.25 to 1.44) | Block-max over 40-posting blocks, quantized integer scores, block-max WAND; for intersections also row 5's per-match costs | Format change later | F12's variable block-max partitions (skip flags bit 3) and P20's quantized impacts (bit 11, kind 21), each its own design and proof | Not measurable until built; `phase-3-ratchets.md` attributes PISA's lead to them |
| 5 | Intersections `TOP_1000`, `TOP_100_COUNT` (Tantivy 1.15, 1.18) | Per-match loop and `advance`; the collector's truncation 14% (docid reads for ties) | Format-free | A threshold-filtered collector resolving ties only for the final k; row 1's counting rule for conjunctions | About 14% at `TOP_1000` (profile; not measured) |
| 6 | Phrases (Tantivy 1.10 to 1.21); the two-phase-critic (1.21 to 1.30) | Intersection 21%, position reads 21%, matching 16%, verification 3.7%, FQ3's impact-list walk 2.1% | Format change later (FQ3's part); the rest format-free | A per-group offset of the positional deltas in level 1 (FQ3, measured), best bundled with the next postings layout change (F12's partitions change that walk anyway); verify position blocks per group, the matching loop | FQ3: **1% to 2%** (0.988 `COUNT`, 0.983 `TOP_1000`; below the 0.97 win line) for +0.044% of the segment; the rest not measured |
| 7 | Negations (Tantivy 1.10, two-term `TOP_100_COUNT` 1.27) | One excluded-side advance per candidate | Format-free | Exclude by window bits, as `count()` does | Not measured |
| — | PISA unions at `TOP_100`, `TOP_1000` (to 2.40); PISA `TOP_100_COUNT` (1.43 to 1.80) | PISA's k = 10 seed is unsafe at k = 100 and 1,000; PISA's `TOP_100_COUNT` ranks nothing | Not comparable | — | — |
| — | PISA `COUNT` intersections ≥ 3 (1.09, 1.25) | Row 2's per-block costs | Format-free | As row 2 | As row 2 |

**Where the next work goes.** Rows 1 and 3 are measured and format-free: their prototypes
(`format13-loss-attribution-ff.patch`) are ready to become a workstream, with the proof every change
needs (verify, parity, a revert check, a run-level benchmark with no row lost). Row 2 is the largest
remaining gap and needs a prototype first. No format change is proposed now: the only measured one,
FQ3's level-1 offset, saves 1% to 2% of phrase time, below the win line, and is best folded into
the next postings layout change. PISA's lead (row 4) is the format change later that matters most,
through F12's partitions and P20's quantized impacts.

## Not run

- The 5M gate VM for any of the attribution: all of it is 1M on the Mac (macOS x86_64); the
  classes were reproduced only by strata's own counters and profiles, not against the baselines
  at 1M (no Tantivy, Lucene, or PISA engine runs here).
- `bm25_xapian`: every run above is `bm25`.
- Linux, FreeBSD, arm64.
- Prototypes of rows 1, 5, 6, and the phrase reads beyond FQ3's offset.

The prototypes and the counters' port, as patches against main `06a730b` (scratch code, not for
landing): `format13-loss-attribution-ff.patch` (rows 2 and 3), `format13-loss-attribution-pos.patch`
(FQ3's offset), `format13-loss-attribution-counters.patch` (the counting build's reader hooks over
the merge of perf/query-counters).
