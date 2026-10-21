# Counted unions of more than three terms: FOR blocks into the window's bits

Row 1 of `format13-loss-attribution.md`, `COUNT` on unions of more than three terms: 2.30 times
slower than Lucene and 2.77 than Lucene `-bp` at 5M, the largest search gap left. This file is
the plan (written before the runs), the prototype, and its numbers. Branch `perf/count-union-bits`;
a prototype to decide whether to build it, not for landing.

## Plan

- **Class.** `COUNT` through `count()`'s 4,096-ordinal windows: `postings_iterator::fill` for each
  clause (`union:num_tokens_>3`; 20 gate queries, none with a required term). It extends
  `query-wins.md` row 1's count-from-window-bits path, which made `TOP_100_COUNT` count this way.
- **Hypothesis.** The attribution put 54% of the time in decoding FOR blocks to ordinals and
  setting one bit each, and 22% in per-block entry checks and loads. Decoding straight into the
  window's bits, checking entries per window rather than per block, and spending no per-posting
  work on dense blocks removes most of it. Lucene's lead is its bitset blocks for dense terms.
- **Procedure.**
  1. A counter build (block codecs, FOR sizes, the bitset size each FOR block would take) over
     the class, to see what is being decoded.
  2. The prototype in `postings_iterator::fill`, checked by `dump` (counted and uncounted, both
     presets, k 10, 100, 1,000) and `verify` over the 962 gate queries at 100k and 1M, docid-merged
     and reordered, byte for byte against main.
  3. Interleaved best-of-N A/Bs on the class (`qab`), then run-level sessions at 1M, A/A first,
     both presets, every row, under a light hold, scored by `ab.py --aa`.
  4. The writer's bitset rule, as a proposal: a bias toward bitset blocks measured on the same
     reader (index size and time); no format bytes land from this branch.
- **Keep if.** The class wins at the run level (upper bound under 1.0 and the A/A band, ratio at
  most 0.97) and no row loses (lower bound above 1.03 and the band) under either preset; results
  byte-identical.
- **Remove if.** Any row loses by that rule, or the identity runs differ.

## Result

**Worth productionizing.** On the target class (`COUNT`, unions of more than three terms) the
prototype takes 0.528 [0.497–0.561] of main's time at 1M reordered under bm25 (1.89× faster) and
0.564 [0.538–0.590] under bm25_xapian (1.77× faster), with no row lost under either preset and
results byte-identical. It changes no format bytes. Projected onto the 5M ratios (not measured at
5M): 2.30 × 0.528 = 1.21× Lucene's time and 2.77 × 0.528 = 1.46× Lucene `-bp`'s, so the gap
narrows from the largest to a smaller one but does not close. The writer bias toward bitset
blocks that the attribution proposed measured as a loser (below) and is not proposed.

Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, Release, one thread, light holds
(`quiet run`); A is main `0c895cb`, B is `e144857` on `perf/count-union-bits`. Linux, FreeBSD, and
arm64 are not measured here: the code is portable C++ with no intrinsics beyond bitpack's, but a
win stays unclaimed until the gate VM's rows are run.

## What it changes (`postings_iterator::fill`, reader.hh)

`fill` sent every FOR block through `load_next_block` (entry located and checked, bytes verified,
`decode_gaps` to an ordinal array, a sum check) and then set one bit per posting with a
read-modify-write on the window's word. Now the blocks wholly inside the window go straight into
its bits:

1. **Entries per window.** `fill_blocks` walks a group's advance entries from the first block
   inside the window, with the F10 checks (last above the previous and within the group, bytes
   inside the group's), where `block_at` located each block anew. The block holding the window's
   end is left to `load_next_block` with its entry already checked.
2. **Constant blocks of step 1 are a bit range.** A constant block whose gaps are all 1 is 128
   consecutive ordinals (5 bytes); it sets bits `[base+1, last]` in at most three word ops.
3. **Other FOR blocks unpack and scatter.** `bitpack::unpack_delta` writes the prefix sums, a ramp
   adds `i + 1`, and the check is the old one (the last equals the entry's last; widths up to 25
   bits cannot wrap). The scatter sums a 32-bit mask per posting into its word's slot, a word's
   total being the sum at its last posting less the sum before its first, so postings in one
   word do not wait on each other's store (the baseline's chain). Sparse blocks (span of 768 or
   more, or under 16 postings) set bits directly. Constant blocks of other steps, blocks with
   exceptions, and widths over 25 bits decode to ordinals first and scatter the same way.
4. **Bitset blocks** OR in a word at a time with the carry kept in a register, with no
   variable-length `memcpy` (it was a libc call per word, 7% of the time in the profile).
5. **The crossing block** (the one holding the window's end, loaded by `load_next_block`)
   scatters its part inside the window the same way, with a consecutive run taking the range path.

Correctness stays where it was: every check `block_at` and `decode_gaps` made is made (the wide
and exceptional paths call `decode_gaps` itself), `need()` verifies each block's bytes before they
are read. Memory: `word_sum_` adds 1,024 bytes to each `postings_iterator` (a union of four terms
holds four); nothing is allocated. Disk and indexing: the writer is untouched; an index rebuilt
with B is byte-identical to A's (100k, 4 flushed segments, `cmp`), and `fixtures_test` passes.

## What the class reads

Counter build over the 20 gate queries (`union:num_tokens_>3` with no required term), one pass,
1M, B's reader with counters; per query:

| Index | FOR blocks | of them constant, step 1 | other FOR | bitset blocks |
|---|---:|---:|---:|---:|
| Reordered | 3,584 | 3,043 (84.9%) | 541 | 4,164 |
| Docid-merged | 1,078 | 2 | 1,076 | 6,669 |

So the attribution's "3,584 FOR blocks decoded" in a reordered index is, for 85% of them, a run
of 128 consecutive documents that the old path decoded and set bit by bit: reordering gathers a
stopword's documents into long runs. That is where most of the gain comes from.

Profile (`sample`, self time, looping `COUNT` over the class, reordered 1M):

| A (main) | B (prototype) |
|---|---|
| `fill` 49.8% (FOR decode and per-bit set) | `or_bitset` 20.1% |
| `block_at` 13.2% | `fill_blocks` 17.9% |
| `load_next_block` 10.4% | `scatter` 14.5% |
| `or_bitset` 8.4% | `fill` 13.6% (the crossing block) |
| `memmove` 7.4% | `block_at` 6.0%, `fill_for` 4.9%, `memmove` 4.3% |

## Identity

Indexes built once with A's binary (identity seed 1) from the first N corpus documents: 100k (4
flushed segments) and 1M (32), each docid-merged and reordered (4 threads). `dump` is every gate
query (962) at k 10, 100, 1,000 under both presets, with the count (5,772 lines each);
`PROSPECT_DUMP_COUNT=0` dumps the uncounted path.

| Index | dump, counted | dump, uncounted | verify (B) |
|---|---|---|---|
| 100k merged | identical | identical | 962 queries, 0 mismatches |
| 100k reordered | identical | identical | 0 mismatches |
| 100k flushed, 4 segments | identical | identical | 0 mismatches |
| 1M merged | identical | identical | 0 mismatches |
| 1M reordered | identical | identical | 0 mismatches |

Also: the full ctest suite minus the crash-point runs (`lifecycle`, `reclaim`, `background_merge`,
and the `*_mapped` variants) passes, 26 of 26 plus `concurrency` and `churn`, including both
fuzz replays; and a build with the fast paths off (every FOR block through `decode_gaps` and the
scatter, no step-1 range) dumps identically at 100k, so the fallback is exercised. A first
version of the scatter used a zero in a 64-bit sum to mean "word untouched"; a sum of full words
wraps to zero, and `dump` caught one count off by one in the 100k reordered index (73,750
against 73,751). The 32-bit sums cannot wrap and are what is in the branch.

## Time

### Run-level, 1M reordered, every row

`bench/quick/ab.sh` at one thread, best of 3 per query and round, 12 rounds for the A/A (A against
itself) and 12 for the A/B per preset, scored at the run level (`ab.py --aa`, perf/query-exp): a
loss needs its lower bound above 1.03 and the A/A band, a win its upper bound under 1.0 and the
band with the ratio at most 0.97. Under one light hold (`quiet run`, 25 minutes): A/A and A/B took
163 and 165 s (bm25), 181 and 182 s (bm25_xapian).

**No row is lost under either preset.** The target and the rows it moves:

| Row | bm25 | bm25_xapian |
|---|---|---|
| `COUNT` union:num_tokens_>3 | **0.528** [0.497–0.561], 1.89× faster (A/A 1.010) | **0.564** [0.538–0.590], 1.77× faster (A/A 0.989) |
| `COUNT` union:num_tokens_3 | **0.884** [0.859–0.910], 1.13× faster | **0.889** [0.873–0.905], 1.12× faster |
| `COUNT` intersection:num_tokens_3 | 0.955 [0.925–0.987], 4.5% faster | 0.980 [0.963–0.997], 2.0% faster, neither |
| `COUNT` intersection:num_tokens_>3 | 0.964 [0.933–0.996], 3.6% faster | 0.978 [0.961–0.996], 2.2% faster, neither |
| `TOP_100_COUNT` union:num_tokens_>3 | **0.892** [0.863–0.921], 1.12× faster | **0.887** [0.872–0.902], 1.13× faster |
| `COUNT` global (all 657 queries) | 0.981 [0.950–1.012], 1.9% faster, neither | 1.003 [0.987–1.019], 0.3% slower, neither |

The `TOP_100_COUNT` union rows count through the same windows (`query-wins.md` row 1). Three
`TOP_10` intersection_union rows read as wins under bm25_xapian only (0.943–0.953); that session's
A/A for them sits at 1.034–1.053 (the band rule passes them), the code is not the prototype's, and
bm25 shows 1.005, 0.996, and 1.016 for the same rows, so they are noise and not claimed. The
highest point estimates elsewhere are `TOP_10` negated 1.023 [0.959–1.092] (A/A 1.018) and
`TOP_100` negated 1.027 [0.966–1.091] (A/A 0.972) under bm25_xapian, four to nineteen queries
each, intervals across 1.0; neither is a loss by the rule.

Every row, B/A [95% over runs], its direction, the A/A band, and the verdict:

| Command | Row | n | bm25 | bm25_xapian |
|---|---|---:|---|---|
| `TOP_10` | global | 657 | 0.997 [0.986–1.008], 0.3% faster; A/A 1.002 [0.989–1.016]; neither | 0.981 [0.960–1.002], 1.9% faster; A/A 1.039 [0.994–1.085]; neither |
| `TOP_10` | intersection | 300 | 0.997 [0.985–1.009], 0.3% faster; A/A 1.000 [0.986–1.015]; neither | 0.984 [0.964–1.004], 1.6% faster; A/A 1.032 [0.991–1.076]; neither |
| `TOP_10` | intersection:num_tokens_2 | 198 | 0.993 [0.979–1.008], 0.7% faster; A/A 1.000 [0.983–1.017]; neither | 0.983 [0.962–1.004], 1.7% faster; A/A 1.033 [0.990–1.078]; neither |
| `TOP_10` | intersection:num_tokens_3 | 83 | 1.002 [0.991–1.013], 0.2% slower; A/A 1.001 [0.986–1.016]; neither | 0.985 [0.965–1.005], 1.5% faster; A/A 1.035 [0.991–1.081]; neither |
| `TOP_10` | intersection:num_tokens_>3 | 19 | 0.998 [0.982–1.014], 0.2% faster; A/A 1.007 [0.990–1.024]; neither | 0.985 [0.962–1.008], 1.5% faster; A/A 1.028 [0.978–1.081]; neither |
| `TOP_10` | intersection_union | 40 | 1.005 [0.951–1.062], 0.5% slower; A/A 0.996 [0.948–1.047]; neither | 0.946 [0.902–0.992], 5.4% faster; A/A 1.053 [1.003–1.106]; win |
| `TOP_10` | intersection_union:num_tokens_2 | 10 | 1.015 [0.954–1.080], 1.5% slower; A/A 1.028 [0.956–1.104]; neither | 0.900 [0.790–1.026], 10.0% faster; A/A 1.072 [1.003–1.145]; neither |
| `TOP_10` | intersection_union:num_tokens_3 | 20 | 0.996 [0.928–1.068], 0.4% faster; A/A 0.990 [0.934–1.050]; neither | 0.953 [0.913–0.995], 4.7% faster; A/A 1.048 [0.995–1.104]; win |
| `TOP_10` | intersection_union:num_tokens_>3 | 10 | 1.016 [0.958–1.077], 1.6% slower; A/A 1.006 [0.976–1.038]; neither | 0.943 [0.900–0.988], 5.7% faster; A/A 1.034 [0.991–1.079]; win |
| `TOP_10` | negated | 19 | 1.023 [0.959–1.092], 2.3% slower; A/A 1.018 [0.964–1.076]; neither | 0.967 [0.930–1.007], 3.3% faster; A/A 1.038 [0.990–1.089]; neither |
| `TOP_10` | negated:num_tokens_2 | 4 | 1.015 [0.952–1.082], 1.5% slower; A/A 1.022 [0.969–1.079]; neither | 0.968 [0.936–1.002], 3.2% faster; A/A 1.043 [0.993–1.095]; neither |
| `TOP_10` | negated:num_tokens_3 | 15 | 1.024 [0.959–1.093], 2.4% slower; A/A 1.018 [0.964–1.075]; neither | 0.967 [0.928–1.007], 3.3% faster; A/A 1.036 [0.988–1.086]; neither |
| `TOP_10` | phrase | 300 | 0.996 [0.986–1.006], 0.4% faster; A/A 1.001 [0.986–1.016]; neither | 0.984 [0.967–1.001], 1.6% faster; A/A 1.032 [0.991–1.075]; neither |
| `TOP_10` | phrase:num_tokens_2 | 198 | 0.991 [0.977–1.005], 0.9% faster; A/A 1.001 [0.986–1.016]; neither | 0.982 [0.963–1.000], 1.8% faster; A/A 1.034 [0.992–1.077]; neither |
| `TOP_10` | phrase:num_tokens_3 | 83 | 0.998 [0.987–1.010], 0.2% faster; A/A 1.000 [0.982–1.018]; neither | 0.990 [0.979–1.002], 1.0% faster; A/A 1.035 [0.991–1.082]; neither |
| `TOP_10` | phrase:num_tokens_>3 | 19 | 1.001 [0.988–1.014], 0.1% slower; A/A 1.004 [0.986–1.022]; neither | 0.985 [0.958–1.012], 1.5% faster; A/A 1.015 [0.974–1.058]; neither |
| `TOP_10` | term | 201 | 0.973 [0.911–1.039], 2.7% faster; A/A 1.023 [0.979–1.070]; neither | 0.978 [0.946–1.011], 2.2% faster; A/A 1.014 [0.959–1.072]; neither |
| `TOP_10` | term:frequent | 66 | 0.977 [0.905–1.054], 2.3% faster; A/A 1.015 [0.988–1.043]; neither | 0.978 [0.945–1.012], 2.2% faster; A/A 1.034 [0.958–1.117]; neither |
| `TOP_10` | term:medium | 67 | 0.968 [0.906–1.035], 3.2% faster; A/A 1.024 [0.977–1.073]; neither | 0.980 [0.948–1.013], 2.0% faster; A/A 1.007 [0.948–1.070]; neither |
| `TOP_10` | term:rare | 67 | 0.979 [0.921–1.040], 2.1% faster; A/A 1.028 [0.968–1.093]; neither | 0.976 [0.942–1.012], 2.4% faster; A/A 1.016 [0.962–1.074]; neither |
| `TOP_10` | two-phase-critic | 1 | 1.014 [0.968–1.062], 1.4% slower; A/A 0.997 [0.934–1.064]; neither | 0.989 [0.933–1.050], 1.1% faster; A/A 1.020 [0.963–1.079]; neither |
| `TOP_10` | union | 301 | 0.994 [0.980–1.007], 0.6% faster; A/A 1.003 [0.990–1.017]; neither | 0.983 [0.963–1.003], 1.7% faster; A/A 1.031 [0.990–1.074]; neither |
| `TOP_10` | union:num_tokens_2 | 198 | 0.992 [0.977–1.007], 0.8% faster; A/A 1.003 [0.989–1.016]; neither | 0.983 [0.963–1.004], 1.7% faster; A/A 1.032 [0.991–1.076]; neither |
| `TOP_10` | union:num_tokens_3 | 83 | 0.998 [0.986–1.010], 0.2% faster; A/A 1.005 [0.993–1.018]; neither | 0.983 [0.965–1.002], 1.7% faster; A/A 1.033 [0.989–1.079]; neither |
| `TOP_10` | union:num_tokens_>3 | 20 | 0.996 [0.977–1.016], 0.4% faster; A/A 1.010 [0.987–1.034]; neither | 0.979 [0.956–1.002], 2.1% faster; A/A 1.021 [0.979–1.065]; neither |
| `TOP_100` | global | 657 | 0.981 [0.958–1.004], 1.9% faster; A/A 0.992 [0.978–1.006]; neither | 1.002 [0.998–1.005], 0.2% slower; A/A 1.004 [0.989–1.020]; neither |
| `TOP_100` | intersection | 300 | 0.979 [0.955–1.004], 2.1% faster; A/A 0.989 [0.970–1.009]; neither | 1.004 [0.999–1.008], 0.4% slower; A/A 1.005 [0.992–1.018]; neither |
| `TOP_100` | intersection:num_tokens_2 | 198 | 0.980 [0.955–1.005], 2.0% faster; A/A 0.989 [0.971–1.008]; neither | 1.004 [1.000–1.008], 0.4% slower; A/A 1.003 [0.989–1.017]; neither |
| `TOP_100` | intersection:num_tokens_3 | 83 | 0.977 [0.953–1.002], 2.3% faster; A/A 0.986 [0.963–1.009]; neither | 1.002 [0.996–1.007], 0.2% slower; A/A 1.007 [0.993–1.020]; neither |
| `TOP_100` | intersection:num_tokens_>3 | 19 | 0.968 [0.937–1.000], 3.2% faster; A/A 0.998 [0.978–1.019]; neither | 1.005 [1.002–1.008], 0.5% slower; A/A 1.027 [1.001–1.055]; neither |
| `TOP_100` | intersection_union | 40 | 0.998 [0.936–1.066], 0.2% faster; A/A 0.993 [0.974–1.012]; neither | 0.994 [0.949–1.041], 0.6% faster; A/A 0.995 [0.937–1.058]; neither |
| `TOP_100` | intersection_union:num_tokens_2 | 10 | 1.006 [0.946–1.070], 0.6% slower; A/A 1.010 [0.975–1.045]; neither | 0.980 [0.948–1.014], 2.0% faster; A/A 0.983 [0.926–1.044]; neither |
| `TOP_100` | intersection_union:num_tokens_3 | 20 | 0.998 [0.933–1.066], 0.2% faster; A/A 0.984 [0.939–1.030]; neither | 0.990 [0.938–1.045], 1.0% faster; A/A 1.006 [0.941–1.074]; neither |
| `TOP_100` | intersection_union:num_tokens_>3 | 10 | 1.012 [0.952–1.075], 1.2% slower; A/A 0.986 [0.944–1.030]; neither | 0.999 [0.949–1.052], 0.1% faster; A/A 0.995 [0.937–1.057]; neither |
| `TOP_100` | negated | 19 | 1.017 [0.955–1.084], 1.7% slower; A/A 0.970 [0.912–1.033]; neither | 1.027 [0.966–1.091], 2.7% slower; A/A 0.972 [0.914–1.034]; neither |
| `TOP_100` | negated:num_tokens_2 | 4 | 1.020 [0.954–1.090], 2.0% slower; A/A 0.973 [0.915–1.034]; neither | 1.020 [0.960–1.083], 2.0% slower; A/A 0.962 [0.891–1.039]; neither |
| `TOP_100` | negated:num_tokens_3 | 15 | 1.014 [0.954–1.079], 1.4% slower; A/A 0.970 [0.911–1.032]; neither | 1.028 [0.968–1.092], 2.8% slower; A/A 0.973 [0.915–1.035]; neither |
| `TOP_100` | phrase | 300 | 0.980 [0.958–1.004], 2.0% faster; A/A 0.989 [0.971–1.007]; neither | 1.001 [0.996–1.006], 0.1% slower; A/A 1.005 [0.992–1.018]; neither |
| `TOP_100` | phrase:num_tokens_2 | 198 | 0.981 [0.957–1.004], 1.9% faster; A/A 0.990 [0.972–1.007]; neither | 1.000 [0.996–1.004], same; A/A 1.005 [0.992–1.018]; neither |
| `TOP_100` | phrase:num_tokens_3 | 83 | 0.980 [0.957–1.003], 2.0% faster; A/A 0.985 [0.960–1.010]; neither | 1.002 [0.995–1.010], 0.2% slower; A/A 1.006 [0.991–1.021]; neither |
| `TOP_100` | phrase:num_tokens_>3 | 19 | 0.984 [0.968–1.001], 1.6% faster; A/A 0.993 [0.976–1.011]; neither | 1.005 [0.991–1.019], 0.5% slower; A/A 1.010 [0.993–1.028]; neither |
| `TOP_100` | term | 201 | 1.019 [0.953–1.090], 1.9% slower; A/A 0.976 [0.924–1.031]; neither | 1.001 [0.976–1.027], 0.1% slower; A/A 0.981 [0.924–1.043]; neither |
| `TOP_100` | term:frequent | 66 | 1.014 [0.965–1.065], 1.4% slower; A/A 0.945 [0.864–1.034]; neither | 1.017 [0.973–1.063], 1.7% slower; A/A 0.969 [0.906–1.036]; neither |
| `TOP_100` | term:medium | 67 | 1.013 [0.941–1.091], 1.3% slower; A/A 0.977 [0.924–1.034]; neither | 1.002 [0.978–1.026], 0.2% slower; A/A 0.978 [0.927–1.032]; neither |
| `TOP_100` | term:rare | 67 | 1.044 [0.968–1.125], 4.4% slower; A/A 0.976 [0.922–1.033]; neither | 0.999 [0.971–1.028], 0.1% faster; A/A 0.981 [0.923–1.042]; neither |
| `TOP_100` | two-phase-critic | 1 | 1.008 [0.960–1.059], 0.8% slower; A/A 0.984 [0.921–1.052]; neither | 0.982 [0.951–1.013], 1.8% faster; A/A 1.044 [0.988–1.104]; neither |
| `TOP_100` | union | 301 | 0.977 [0.952–1.002], 2.3% faster; A/A 0.988 [0.972–1.006]; neither | 1.002 [0.997–1.006], 0.2% slower; A/A 1.005 [0.991–1.019]; neither |
| `TOP_100` | union:num_tokens_2 | 198 | 0.977 [0.952–1.003], 2.3% faster; A/A 0.989 [0.972–1.006]; neither | 1.001 [0.996–1.007], 0.1% slower; A/A 1.004 [0.988–1.021]; neither |
| `TOP_100` | union:num_tokens_3 | 83 | 0.977 [0.954–1.001], 2.3% faster; A/A 0.985 [0.966–1.005]; neither | 1.003 [0.995–1.011], 0.3% slower; A/A 1.009 [0.996–1.022]; neither |
| `TOP_100` | union:num_tokens_>3 | 20 | 0.977 [0.951–1.004], 2.3% faster; A/A 0.989 [0.975–1.003]; neither | 1.000 [0.994–1.007], same; A/A 1.004 [0.983–1.026]; neither |
| `TOP_1000` | global | 657 | 0.993 [0.962–1.025], 0.7% faster; A/A 1.005 [0.989–1.021]; neither | 1.006 [0.982–1.031], 0.6% slower; A/A 0.985 [0.963–1.008]; neither |
| `TOP_1000` | intersection | 300 | 0.994 [0.960–1.028], 0.6% faster; A/A 1.004 [0.988–1.019]; neither | 1.008 [0.985–1.032], 0.8% slower; A/A 0.984 [0.959–1.009]; neither |
| `TOP_1000` | intersection:num_tokens_2 | 198 | 0.994 [0.960–1.029], 0.6% faster; A/A 1.005 [0.988–1.021]; neither | 1.008 [0.986–1.031], 0.8% slower; A/A 0.982 [0.956–1.008]; neither |
| `TOP_1000` | intersection:num_tokens_3 | 83 | 0.991 [0.959–1.023], 0.9% faster; A/A 1.004 [0.991–1.018]; neither | 1.006 [0.981–1.032], 0.6% slower; A/A 0.985 [0.960–1.009]; neither |
| `TOP_1000` | intersection:num_tokens_>3 | 19 | 1.000 [0.962–1.040], same; A/A 0.996 [0.982–1.009]; neither | 1.010 [0.986–1.035], 1.0% slower; A/A 0.990 [0.974–1.006]; neither |
| `TOP_1000` | intersection_union | 40 | 0.991 [0.927–1.059], 0.9% faster; A/A 1.024 [0.974–1.077]; neither | 1.005 [0.965–1.046], 0.5% slower; A/A 1.050 [1.005–1.097]; neither |
| `TOP_1000` | intersection_union:num_tokens_2 | 10 | 1.005 [0.954–1.058], 0.5% slower; A/A 1.013 [0.938–1.095]; neither | 1.003 [0.952–1.057], 0.3% slower; A/A 1.052 [0.993–1.115]; neither |
| `TOP_1000` | intersection_union:num_tokens_3 | 20 | 0.980 [0.906–1.059], 2.0% faster; A/A 1.026 [0.962–1.094]; neither | 0.998 [0.947–1.053], 0.2% faster; A/A 1.057 [1.012–1.104]; neither |
| `TOP_1000` | intersection_union:num_tokens_>3 | 10 | 1.005 [0.949–1.064], 0.5% slower; A/A 1.009 [0.975–1.045]; neither | 1.011 [0.968–1.057], 1.1% slower; A/A 1.004 [0.961–1.049]; neither |
| `TOP_1000` | negated | 19 | 1.011 [0.972–1.051], 1.1% slower; A/A 1.029 [0.976–1.084]; neither | 1.092 [0.944–1.264], 9.2% slower; A/A 0.993 [0.961–1.026]; neither |
| `TOP_1000` | negated:num_tokens_2 | 4 | 1.008 [0.968–1.050], 0.8% slower; A/A 1.026 [0.971–1.085]; neither | 1.095 [0.952–1.259], 9.5% slower; A/A 1.005 [0.972–1.039]; neither |
| `TOP_1000` | negated:num_tokens_3 | 15 | 1.013 [0.975–1.053], 1.3% slower; A/A 1.034 [0.987–1.084]; neither | 1.092 [0.943–1.265], 9.2% slower; A/A 0.990 [0.954–1.027]; neither |
| `TOP_1000` | phrase | 300 | 0.994 [0.961–1.027], 0.6% faster; A/A 1.002 [0.987–1.016]; neither | 1.004 [0.981–1.027], 0.4% slower; A/A 0.981 [0.956–1.007]; neither |
| `TOP_1000` | phrase:num_tokens_2 | 198 | 0.993 [0.960–1.028], 0.7% faster; A/A 1.002 [0.989–1.016]; neither | 1.004 [0.981–1.026], 0.4% slower; A/A 0.981 [0.956–1.008]; neither |
| `TOP_1000` | phrase:num_tokens_3 | 83 | 0.994 [0.968–1.021], 0.6% faster; A/A 1.003 [0.986–1.021]; neither | 1.006 [0.982–1.030], 0.6% slower; A/A 0.978 [0.951–1.004]; neither |
| `TOP_1000` | phrase:num_tokens_>3 | 19 | 0.998 [0.963–1.034], 0.2% faster; A/A 0.995 [0.972–1.018]; neither | 1.008 [0.984–1.033], 0.8% slower; A/A 0.981 [0.953–1.011]; neither |
| `TOP_1000` | term | 201 | 0.981 [0.894–1.076], 1.9% faster; A/A 1.053 [1.003–1.105]; neither | 1.031 [0.987–1.078], 3.1% slower; A/A 0.964 [0.927–1.003]; neither |
| `TOP_1000` | term:frequent | 66 | 1.007 [0.977–1.037], 0.7% slower; A/A 1.007 [0.952–1.065]; neither | 1.037 [0.990–1.087], 3.7% slower; A/A 0.981 [0.933–1.031]; neither |
| `TOP_1000` | term:medium | 67 | 0.987 [0.898–1.085], 1.3% faster; A/A 1.056 [1.003–1.111]; neither | 1.025 [0.979–1.072], 2.5% slower; A/A 0.958 [0.916–1.002]; neither |
| `TOP_1000` | term:rare | 67 | 0.979 [0.872–1.099], 2.1% faster; A/A 1.071 [1.010–1.136]; neither | 1.033 [0.984–1.085], 3.3% slower; A/A 0.967 [0.930–1.007]; neither |
| `TOP_1000` | two-phase-critic | 1 | 0.995 [0.934–1.060], 0.5% faster; A/A 1.011 [0.974–1.050]; neither | 0.992 [0.966–1.018], 0.8% faster; A/A 0.984 [0.939–1.032]; neither |
| `TOP_1000` | union | 301 | 0.993 [0.959–1.027], 0.7% faster; A/A 1.003 [0.987–1.020]; neither | 1.003 [0.979–1.029], 0.3% slower; A/A 0.980 [0.955–1.005]; neither |
| `TOP_1000` | union:num_tokens_2 | 198 | 0.993 [0.959–1.029], 0.7% faster; A/A 1.004 [0.988–1.021]; neither | 1.003 [0.979–1.029], 0.3% slower; A/A 0.978 [0.952–1.006]; neither |
| `TOP_1000` | union:num_tokens_3 | 83 | 0.990 [0.959–1.023], 1.0% faster; A/A 1.006 [0.986–1.026]; neither | 1.002 [0.976–1.029], 0.2% slower; A/A 0.978 [0.953–1.002]; neither |
| `TOP_1000` | union:num_tokens_>3 | 20 | 0.999 [0.970–1.029], 0.1% faster; A/A 1.003 [0.991–1.016]; neither | 1.007 [0.983–1.031], 0.7% slower; A/A 0.989 [0.968–1.010]; neither |
| `TOP_100_COUNT` | global | 657 | 1.010 [0.984–1.036], 1.0% slower; A/A 0.997 [0.986–1.008]; neither | 1.001 [0.987–1.016], 0.1% slower; A/A 0.983 [0.955–1.011]; neither |
| `TOP_100_COUNT` | intersection | 300 | 1.019 [0.987–1.052], 1.9% slower; A/A 0.997 [0.985–1.009]; neither | 1.006 [0.986–1.026], 0.6% slower; A/A 0.982 [0.949–1.017]; neither |
| `TOP_100_COUNT` | intersection:num_tokens_2 | 198 | 1.021 [0.987–1.055], 2.1% slower; A/A 0.997 [0.984–1.009]; neither | 1.005 [0.986–1.025], 0.5% slower; A/A 0.983 [0.950–1.016]; neither |
| `TOP_100_COUNT` | intersection:num_tokens_3 | 83 | 1.019 [0.988–1.052], 1.9% slower; A/A 0.996 [0.983–1.008]; neither | 1.008 [0.986–1.031], 0.8% slower; A/A 0.980 [0.944–1.017]; neither |
| `TOP_100_COUNT` | intersection:num_tokens_>3 | 19 | 1.010 [0.979–1.042], 1.0% slower; A/A 0.998 [0.990–1.006]; neither | 1.011 [0.993–1.029], 1.1% slower; A/A 0.981 [0.937–1.026]; neither |
| `TOP_100_COUNT` | intersection_union | 40 | 0.995 [0.954–1.037], 0.5% faster; A/A 1.019 [0.982–1.058]; neither | 1.011 [0.962–1.063], 1.1% slower; A/A 0.967 [0.925–1.012]; neither |
| `TOP_100_COUNT` | intersection_union:num_tokens_2 | 10 | 1.014 [0.961–1.070], 1.4% slower; A/A 1.012 [0.948–1.081]; neither | 1.013 [0.961–1.069], 1.3% slower; A/A 0.992 [0.948–1.038]; neither |
| `TOP_100_COUNT` | intersection_union:num_tokens_3 | 20 | 0.990 [0.941–1.041], 1.0% faster; A/A 1.025 [0.982–1.070]; neither | 1.017 [0.964–1.073], 1.7% slower; A/A 0.967 [0.921–1.015]; neither |
| `TOP_100_COUNT` | intersection_union:num_tokens_>3 | 10 | 0.985 [0.952–1.019], 1.5% faster; A/A 1.047 [0.999–1.098]; neither | 1.011 [0.967–1.056], 1.1% slower; A/A 0.955 [0.904–1.009]; neither |
| `TOP_100_COUNT` | negated | 19 | 0.968 [0.917–1.021], 3.2% faster; A/A 1.034 [0.988–1.083]; neither | 1.024 [0.968–1.084], 2.4% slower; A/A 0.983 [0.918–1.053]; neither |
| `TOP_100_COUNT` | negated:num_tokens_2 | 4 | 0.981 [0.927–1.039], 1.9% faster; A/A 1.044 [0.983–1.107]; neither | 1.026 [0.975–1.080], 2.6% slower; A/A 0.984 [0.920–1.053]; neither |
| `TOP_100_COUNT` | negated:num_tokens_3 | 15 | 0.959 [0.903–1.018], 4.1% faster; A/A 1.035 [0.992–1.080]; neither | 1.023 [0.967–1.083], 2.3% slower; A/A 0.984 [0.918–1.055]; neither |
| `TOP_100_COUNT` | phrase | 300 | 1.019 [0.987–1.051], 1.9% slower; A/A 0.996 [0.984–1.009]; neither | 1.004 [0.982–1.026], 0.4% slower; A/A 0.982 [0.948–1.018]; neither |
| `TOP_100_COUNT` | phrase:num_tokens_2 | 198 | 1.018 [0.986–1.052], 1.8% slower; A/A 0.996 [0.984–1.008]; neither | 1.003 [0.981–1.026], 0.3% slower; A/A 0.986 [0.954–1.020]; neither |
| `TOP_100_COUNT` | phrase:num_tokens_3 | 83 | 1.019 [0.990–1.048], 1.9% slower; A/A 0.996 [0.982–1.009]; neither | 1.004 [0.983–1.027], 0.4% slower; A/A 0.975 [0.938–1.013]; neither |
| `TOP_100_COUNT` | phrase:num_tokens_>3 | 19 | 1.016 [0.988–1.044], 1.6% slower; A/A 0.997 [0.983–1.011]; neither | 1.006 [0.995–1.016], 0.6% slower; A/A 0.974 [0.936–1.013]; neither |
| `TOP_100_COUNT` | term | 201 | 1.011 [0.949–1.077], 1.1% slower; A/A 0.972 [0.938–1.007]; neither | 1.002 [0.955–1.051], 0.2% slower; A/A 0.952 [0.896–1.012]; neither |
| `TOP_100_COUNT` | term:frequent | 66 | 1.002 [0.932–1.076], 0.2% slower; A/A 0.970 [0.929–1.013]; neither | 0.996 [0.952–1.042], 0.4% faster; A/A 0.971 [0.897–1.051]; neither |
| `TOP_100_COUNT` | term:medium | 67 | 1.008 [0.945–1.074], 0.8% slower; A/A 0.970 [0.935–1.007]; neither | 1.006 [0.955–1.060], 0.6% slower; A/A 0.952 [0.901–1.006]; neither |
| `TOP_100_COUNT` | term:rare | 67 | 1.011 [0.947–1.081], 1.1% slower; A/A 0.970 [0.934–1.007]; neither | 1.006 [0.959–1.056], 0.6% slower; A/A 0.951 [0.898–1.007]; neither |
| `TOP_100_COUNT` | two-phase-critic | 1 | 1.021 [0.980–1.064], 2.1% slower; A/A 0.999 [0.970–1.028]; neither | 1.026 [0.996–1.057], 2.6% slower; A/A 1.007 [0.973–1.042]; neither |
| `TOP_100_COUNT` | union | 301 | 1.006 [0.977–1.036], 0.6% slower; A/A 0.995 [0.981–1.009]; neither | 0.995 [0.979–1.012], 0.5% faster; A/A 0.985 [0.956–1.016]; neither |
| `TOP_100_COUNT` | union:num_tokens_2 | 198 | 1.019 [0.985–1.053], 1.9% slower; A/A 0.995 [0.981–1.010]; neither | 1.007 [0.986–1.029], 0.7% slower; A/A 0.984 [0.954–1.015]; neither |
| `TOP_100_COUNT` | union:num_tokens_3 | 83 | 0.962 [0.938–0.986], 3.8% faster; A/A 0.997 [0.984–1.010]; neither | 0.963 [0.945–0.981], 3.7% faster; A/A 0.984 [0.954–1.015]; neither |
| `TOP_100_COUNT` | union:num_tokens_>3 | 20 | 0.892 [0.863–0.921], 1.12× faster; A/A 0.995 [0.980–1.011]; win | 0.887 [0.872–0.902], 1.13× faster; A/A 0.984 [0.951–1.019]; win |
| `COUNT` | global | 657 | 0.981 [0.950–1.012], 1.9% faster; A/A 1.006 [0.999–1.013]; neither | 1.003 [0.987–1.019], 0.3% slower; A/A 1.001 [0.958–1.046]; neither |
| `COUNT` | intersection | 300 | 0.985 [0.956–1.014], 1.5% faster; A/A 1.006 [0.999–1.013]; neither | 1.000 [0.983–1.018], same; A/A 1.008 [0.954–1.065]; neither |
| `COUNT` | intersection:num_tokens_2 | 198 | 0.995 [0.966–1.024], 0.5% faster; A/A 1.007 [1.000–1.014]; neither | 1.008 [0.990–1.026], 0.8% slower; A/A 1.011 [0.955–1.070]; neither |
| `COUNT` | intersection:num_tokens_3 | 83 | 0.955 [0.925–0.987], 4.5% faster; A/A 1.005 [0.998–1.013]; win | 0.980 [0.963–0.997], 2.0% faster; A/A 0.996 [0.950–1.045]; neither |
| `COUNT` | intersection:num_tokens_>3 | 19 | 0.964 [0.933–0.996], 3.6% faster; A/A 1.010 [0.997–1.023]; win | 0.978 [0.961–0.996], 2.2% faster; A/A 0.985 [0.936–1.037]; neither |
| `COUNT` | intersection_union | 40 | 0.978 [0.906–1.057], 2.2% faster; A/A 0.956 [0.865–1.056]; neither | 1.053 [1.004–1.105], 5.3% slower; A/A 1.014 [0.967–1.064]; neither |
| `COUNT` | intersection_union:num_tokens_2 | 10 | 0.971 [0.901–1.046], 2.9% faster; A/A 0.956 [0.861–1.062]; neither | 1.057 [1.007–1.108], 5.7% slower; A/A 1.015 [0.969–1.064]; neither |
| `COUNT` | intersection_union:num_tokens_3 | 20 | 0.980 [0.908–1.058], 2.0% faster; A/A 0.952 [0.852–1.063]; neither | 1.054 [1.006–1.106], 5.4% slower; A/A 1.015 [0.967–1.065]; neither |
| `COUNT` | intersection_union:num_tokens_>3 | 10 | 0.982 [0.906–1.066], 1.8% faster; A/A 0.992 [0.941–1.044]; neither | 1.000 [0.891–1.122], same; A/A 1.013 [0.965–1.064]; neither |
| `COUNT` | negated | 19 | 1.023 [0.947–1.106], 2.3% slower; A/A 0.995 [0.944–1.049]; neither | 1.089 [1.038–1.142], 8.9% slower; A/A 1.016 [0.968–1.066]; neither |
| `COUNT` | negated:num_tokens_2 | 4 | 1.016 [0.938–1.100], 1.6% slower; A/A 0.995 [0.943–1.050]; neither | 1.079 [1.025–1.136], 7.9% slower; A/A 1.024 [0.970–1.081]; neither |
| `COUNT` | negated:num_tokens_3 | 15 | 1.025 [0.950–1.106], 2.5% slower; A/A 0.995 [0.944–1.050]; neither | 1.091 [1.041–1.144], 9.1% slower; A/A 1.013 [0.965–1.064]; neither |
| `COUNT` | phrase | 300 | 0.986 [0.960–1.014], 1.4% faster; A/A 1.005 [0.999–1.012]; neither | 1.001 [0.989–1.014], 0.1% slower; A/A 1.013 [0.962–1.067]; neither |
| `COUNT` | phrase:num_tokens_2 | 198 | 0.985 [0.958–1.013], 1.5% faster; A/A 1.006 [1.000–1.012]; neither | 1.003 [0.992–1.015], 0.3% slower; A/A 1.019 [0.964–1.078]; neither |
| `COUNT` | phrase:num_tokens_3 | 83 | 0.990 [0.964–1.016], 1.0% faster; A/A 1.005 [0.996–1.015]; neither | 0.998 [0.983–1.014], 0.2% faster; A/A 0.996 [0.955–1.040]; neither |
| `COUNT` | phrase:num_tokens_>3 | 19 | 0.991 [0.963–1.019], 0.9% faster; A/A 1.004 [0.991–1.018]; neither | 1.000 [0.987–1.013], same; A/A 0.990 [0.948–1.034]; neither |
| `COUNT` | term | 201 | 0.990 [0.975–1.005], 1.0% faster; A/A 1.003 [0.959–1.048]; neither | 1.020 [0.990–1.051], 2.0% slower; A/A 1.021 [0.977–1.066]; neither |
| `COUNT` | term:frequent | 66 | 0.992 [0.961–1.023], 0.8% faster; A/A 1.010 [0.966–1.057]; neither | 1.023 [0.989–1.057], 2.3% slower; A/A 1.028 [0.981–1.077]; neither |
| `COUNT` | term:medium | 67 | 0.994 [0.974–1.015], 0.6% faster; A/A 1.003 [0.959–1.048]; neither | 1.021 [0.990–1.053], 2.1% slower; A/A 1.026 [0.982–1.072]; neither |
| `COUNT` | term:rare | 67 | 1.000 [1.000–1.000], same; A/A 1.002 [0.958–1.049]; neither | 1.020 [0.990–1.051], 2.0% slower; A/A 1.026 [0.988–1.066]; neither |
| `COUNT` | two-phase-critic | 1 | 1.003 [0.960–1.047], 0.3% slower; A/A 0.995 [0.945–1.047]; neither | 0.995 [0.976–1.014], 0.5% faster; A/A 0.983 [0.953–1.013]; neither |
| `COUNT` | union | 301 | 0.976 [0.946–1.007], 2.4% faster; A/A 1.007 [0.999–1.014]; neither | 0.995 [0.978–1.012], 0.5% faster; A/A 1.008 [0.956–1.062]; neither |
| `COUNT` | union:num_tokens_2 | 198 | 0.996 [0.968–1.024], 0.4% faster; A/A 1.006 [0.998–1.013]; neither | 1.013 [0.998–1.029], 1.3% slower; A/A 1.017 [0.958–1.080]; neither |
| `COUNT` | union:num_tokens_3 | 83 | 0.884 [0.859–0.910], 1.13× faster; A/A 1.011 [0.999–1.022]; win | 0.889 [0.873–0.905], 1.12× faster; A/A 0.998 [0.954–1.045]; win |
| `COUNT` | union:num_tokens_>3 | 20 | 0.528 [0.497–0.561], 1.89× faster; A/A 1.010 [1.002–1.019]; win | 0.564 [0.538–0.590], 1.77× faster; A/A 0.989 [0.947–1.034]; win |

### Interleaved A/B on the class (informational)

`qab.sh`: best of 3 per query over 10 interleaved rounds, the 20 queries, total time of each
side's per-query best.

| Index | A | B | B over A |
|---|---:|---:|---:|
| 1M reordered | 28.17 ms | 12.60 ms | 0.447 (2.24× faster) |
| 1M docid-merged | 19.65 ms | 15.86 ms | 0.807 (1.24× faster) |

The merged index gains less because its blocks are mostly bitsets already (above) and the
remainder are medium-density FOR blocks, where the scatter's floor is about 0.7 ns per posting.

### Build-up (separate sessions on a loaded host; indicative, total-time ratio to A in that session)

| Step | B over A |
|---|---:|
| Fused FOR decode and scatter, entries per run (a plan array) | 0.86 |
| Scatter sums masks into 32-bit word slots | 0.74 |
| Constant step-1 blocks as bit ranges | 0.50 |
| Bitset OR with a carry and no libc copy, entry walk without the plan array, consecutive crossing run | 0.44 |

The scatter alone (microbenchmark, 2,000 blocks per density, noisy host, ns per 128-posting
block): the baseline's per-bit RMW 110–175 ns at mean gaps 1 to 32, summed masks into word slots
82–105 ns at spans up to 640 bits, equal beyond; hence the sparse fallback. About 0.65 to 0.9 ns per
posting is the floor without a different mechanism.

## Writer bias toward bitset blocks: a loser, not proposed

The attribution suggested leaning the writer toward bitsets as Lucene does. Measured with B's reader
on indexes rewritten with a rule `bitset if bitset bytes <= 2.0 x FOR bytes` (FOR blocks that are
not constant; a 128-document run stays a 5-byte constant), one-line writer change, 1M, same
identity seed:

| Index | Segment bytes | `COUNT` union>3, bitset-biased over unbiased (B's reader) | (A's reader) |
|---|---:|---:|---:|
| Reordered | 576,286,832 -> 583,525,244 (+1.26%) | 1.021 total, 1.033 geometric mean | 1.000, 1.031 |
| Docid-merged | 616,896,840 -> 623,424,812 (+1.06%) | 1.019, 1.037 | 1.051, 1.076 |

More bytes and no faster, even for A's reader: a bitset block's OR over a wide span costs about
what a FOR block's unpack and scatter does at those densities. Under the 2.0 rule 8.3% of the class's
FOR blocks change codec in the reordered index (85% of them being constant runs, which the rule
leaves alone) and 56% in the merged one. No format change is proposed; the experiment's writer code (an environment variable read in
`write_postings`) is not on this branch.

## Cost and magnitude

At 1M reordered the class takes 1.41 ms per query with main and 0.63 ms with B: 0.78 ms of CPU
saved per query. If the query were run a billion times at 1M-scale indexes that is about 217
core-hours, and at 5M (by the 5x index size, assuming linear) about 1,080: at an assumed $0.04 per
core-hour that is $9 and $43 per billion queries. That is negligible next to the cost of the rest of
a mix, and the class is 20 of the gate's 962 queries and one of the five commands: the whole
`COUNT` command's mean over all 657 queries moves 1.9% faster under bm25 and 0.3% slower under
bm25_xapian, neither outside its band. The value
is the benchmark row (the largest remaining search gap), not CPU spend.

## Next, if built

1. Measure on the gate VM (Linux, FreeBSD) and an arm64 host before landing; the 768-bit and
   16-posting thresholds and the 32-bit word sums were tuned on this x86_64 host.
2. A differential unit test of `fill` against `next()` over every block kind and window
   alignment (wide widths need a segment over 2^25 ordinals' gaps; the fast-paths-off build stands
   in for that here), and the ASan, UBSan, and TSan CI.
3. Move `word_sum_` to a `thread_local` to take the 1,024 bytes off each iterator.
4. What is left in B's profile (`or_bitset`, the entry walk in `fill_blocks`, the crossing
   block) is spread across four items of 14% to 20% each; the next step is per-clause window
   fusion (OR the clauses' windows with the bits in registers), not another per-block change.
5. A 5M run of the class on the gate VM to confirm the projected 1.21× Lucene and 1.46× `-bp`.

