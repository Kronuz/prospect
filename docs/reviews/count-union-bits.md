# Code review: perf/count-union-bits at 0c3bac1

Range `0c895cb..0c3bac1` (base is origin/main, 3 commits): `postings_iterator::fill` and its helpers in
`include/prospect/reader.hh`, `test/fill_test.cc`, `test/fill_mutants.py`, `CMakeLists.txt`, and
`docs/perf/count-union-bits.md`. There is no ledger entry with done criteria for this branch and
`docs/reviews/calibration.md` does not exist in this tree, so the review checks the four points in the
prompt plus the design note's claims (no format change, byte-identical results, asserts not clamps).
Host: macOS x86_64, Homebrew LLVM 23 (`/usr/local/opt/llvm`), `-O1 -g -DNDEBUG -fsanitize=address,undefined
-fno-sanitize-recover=undefined`, run at `nice -n 19` under a light `quiet` hold.

## Verdict: APPROVE WITH CONDITIONS

No release-build write outside the window's 64 words or outside `word_sum_` exists for any block shape I
could construct or reach, including damaged on-disk input. The differential against main is byte-identical on
good and on damaged segments. The two conditions are test and assert gaps on the very checks the bounds
argument leans on (CR1, CR2); neither is a bug in the code as it stands.

## Blocking items

None.

## Conditions (a rechecker confirms, no second review)

1. **CR1: add a test that fails when either wrap guard is removed.** See the finding.
2. **CR2: assert `r.last > r.base` in `fill_for`.** See the finding.

## Findings

### Bounds attack (the first ask): no out-of-window write found

I walked every write in `fill` against the checks that dominate it, then attacked each check by deleting it
(mutants, on a copy of the header) and running the release damage probe.

Every write and what bounds it:

- First (partly consumed) block, `reader.hh:858-863`: `o[k] = buf_ords_[k] - base` with `buf_ords_[k]` in
  `[doc_, end)`, rising (decode_gaps, `check_increasing`, bitset popcount). `scatter` then indexes
  `bits[o >> 6]` and `word_sum_[o >> 5]` with `o < 4096`, so `< 64` and `< 128`. Needs `base <= doc_`, the
  documented caller contract (`search.hh:151-153`), which every caller establishes by `advance(base)` first
  (`search.hh:529-560`, `862-864`, `1031-1033`, `count()` at `search.hh:1411`). The old loop wrapped the same way
  on a violation, so this is not new exposure.
- `fill_blocks` (`:1166-1190`): a block is taken only if `last < end` (first block `:1168`, later ones
  `:1185`), and the entry chain is checked to rise (`:1182`, `last <= r.last`; the first block by
  `block_at`'s `last <= base` check, `:981`). So `r.last > r.base >= doc_ >= base - 1`.
- `or_bitset` (`:1127-1153`): writes `out[0 .. words-1]` and `out[words]` only on a nonzero carry. Bits are
  masked to `span`, so the highest bit written is `r.last - base < 4096`. I checked
  `floor(at/64) + ceil(span/64) - 1 <= floor((at+span-1)/64)`, so the unconditional `out[w] |= ...` for
  `w < words` stays in the window. The 8-byte read past `bytes` for the last word is inside `r.bytes`
  (`full = min(words, r.bytes / 8)`) and masked off.
- `fill_for` constant step-1 path (`:1199-1203`): `head == constant_flag` and a zero step payload, so
  `r.last - r.base == 128` is the real distance (`r.last > r.base`), `set_range(r.base + 1 - base, r.last - base)`
  with `r.base + 1 >= base` and `r.last < end`.
- `fill_for` fused path (`:1206-1217`), the only place arithmetic wraps. `unpack_delta` prefix-sums mod 2^32.
  With width <= 25 the 128 gaps plus 128 total at most 2^32 exactly. `o[127] == r.last - base` (mod 2^32) with
  `0 < r.last - r.base < 2^32` then forces the true total to equal `r.last - r.base`: the alternatives are
  `+2^32` (above the maximum) or `-2^32` (negative). So there is no wrap, the ordinals rise strictly from
  `r.base - base + 1 >= 0` to `r.last - base < 4096`. This is the argument the doc states as "cannot wrap", and
  it holds, but it depends on `r.last > r.base` being established elsewhere (CR2).
- `scatter` (`:1235-1262`): the dense branch needs `o[n-1] - o[0] < 768`, so slots `o[0]>>5 .. o[n-1]>>5`, at
  most 25, all `< 128`; every written slot is inside the clearing loop's range, so `word_sum_` is all zero
  between calls.

Probes:

- `.scratch/review/damage_probe.cc` (`damage_main`, `damage_branch`, `run_damage.sh`, `dmg_*.txt`): 12 seeds
  (1 to 12) of 150 to 300 trials per term over 7 terms: header bytes set to chosen widths (0, 1, 12, 24 to 26, 31,
  63), flag bits, width 22 to 25 blocks filled with 0xff, constant blocks with random payloads, random bytes and
  bits in blocks, random advance-entry fields, all resealed. Per trial three window schemes (4096-aligned, 700,
  random start and length with pre-set bits), canary words on both sides of the 64-word window, and with
  `-DPEEK` (`#define private public`) a check that `word_sum_` is all zero after every fill. ASan+UBSan,
  `NDEBUG` on, asserts off. Result: no ASan or UBSan report, no canary or `word_sum_` hit, and the
  transcripts (fence message at the same trial, bits, `doc()` after each window) are identical to main's for
  all 12 seeds (about 22,000 damaged files, about 80 percent fenced).
- `.scratch/review/run_mutants2.py` / `run_mutants2.log`: deletes each runtime check the argument leans on.
  Results are the basis of CR1 below.

### CR1 (condition): nothing in the test suite fails if a wrap guard is deleted

Probe: `.scratch/review/run_mutants2.py` (output in `run_mutants2.log`). Two checks keep the fused unpack
from wrapping, and `fill_test` (asserts on) and the damage probe both stay green without either:

| Mutant (copy of reader.hh) | `fill_test` | release damage probe |
|---|---|---|
| `fill_blocks`: drop `last <= r.last` from the entry check, `reader.hh:1182` | SURVIVED | runs, answers differ from main, no crash |
| fused path: width limit 25 raised to 32, `reader.hh:1206` | SURVIVED | identical to main |

Both checks are load-bearing: with either removed a crafted block (width 26 to 32, or an entry with
`last <= previous last` and gaps summing to `2^32 - (r.base - r.last)`) passes `o[127] == r.last - base` while
its leading ordinals wrap, which lands `bits[o >> 6]` or `word_sum_[o >> 5]` out of bounds. Random damage
does not reach those shapes (it needs `o[127]` to match by chance), and valid files cannot (a gap inside a
window is at most 4,095, so width at most 12). The fused-path argument in the doc therefore has no executable
witness.

Condition: add to `fill_test.cc` one crafted case per guard, each of which fences (`corrupt_segment`) with the
guard and is caught by the canary or an assert without it: (a) a FOR block whose header width is 26 to 32 with
gaps chosen so `o[127]` matches mod 2^32; (b) a block pair whose second advance entry has `last <= first.last`
with 25-bit gaps summing to `2^32 - d`. Add both to `fill_mutants.py` so they stay checked. (`skip_layout.hh`
has `put` for entries; the block bytes are edited in place and resealed like the existing damaged cases.)

### CR2 (condition): state the dominating check as an assert in `fill_for`

`reader.hh:1194-1217`. The fused path is correct only when `r.last > r.base`, which `block_at` (`:981`) and
`fill_blocks` (`:1182`) establish, not `fill_for` and not the fused branch (`decode_gaps` re-checks it at
`format.hh:131`, the fused branch does not). The branch already asserts the other bounds as free-in-release
invariants; add `assert(r.last > r.base)` at the top of `fill_for` and the same in `or_bitset` (where
`span = last - first + 1` wraps to a huge `bytes` and is fenced today, so only `fill_for` is exposed). A future
caller of `fill_for` that skips the entry walk would otherwise silently lose the wrap argument.

### Point 1: the 32-bit sum cannot wrap

`reader.hh:1252-1260`. The accumulator `c` is `uint64_t` and each term is a mask `< 2^32` (`bit_masks[d & 31]`),
at most 128 terms, so `c < 2^39` and never wraps for any posting count or width. A word's total is
`x - prev` where the slot values are cumulative, and because the ordinals are strictly rising the masks within
one 32-bit word are distinct bits, so each delta is `< 2^32` and exact; `& -(x != 0)` is right because a written
slot is at least 1. Probe: `.scratch/review/scatter_probe.cc` (`scatter_probe`): 2,999,981 random strictly
rising sequences (1 to 128 postings, six shapes including full words followed by a lone bit and `d & 31`
boundaries, 2,378,212 on the summed path) against a model with random pre-set bits, calling the private
`scatter` directly and checking `word_sum_` is zero after each: 0 mismatches, 0 dirty slots.

### Point 2: fill ORs and never clobbers

Every store into `bits` is `|=` except `set_range`'s middle words (`reader.hh:1224`, `= ~0`), which is the same
as OR-ing all ones. Edge masks `ml`/`mh` (`:1220`) are correct at `lo&63 == 0`, `hi&63 == 63`, one word, and two
words. Bits at or above `end - base` are never set (all taken blocks have `last < end`; the first segment stops
at `lower_bound(end)`). Evidence: `fill_test` pre-fills half the windows with random bits and canaries and compares
to a model for windows of 4096, 4000, 1024, 300, 64, 1 from three starts plus the end sweep; and
`diff_probe.cc` pre-sets random words in a quarter of its fills. Both agree with the model and with main.

### Point 3: results identical to main

Probe: `.scratch/review/diff_probe.cc`, built twice from the same source (`diff_main` against
`git archive origin/main include` in `.scratch/review/main/include`; `diff_branch_rel` NDEBUG, asserts off;
`diff_branch_assert` asserts on). Per seed: a 70,000-document segment with 16 terms (random density 0.02 to 0.9,
stepped runs of step 1/2/3 so constant blocks of step 1 and not, bitset-dense, mixed stretches, df of 1, 5, 128,
129, 640, varied wdf, a positional term); per term 12 runs of 500 random operations mixing `next`, `advance`
(+500, +9000), fills with base up to 70 before `doc()` and random ends, and aligned windows, with pre-set bits;
hash of every window's 64 words, `doc()`, and `wdf()` after each step; then 60 `count()` runs over
disjunctions, 2 to 3 term conjunctions of dense terms, and `(a or b) minus c`, on the scorer and on its `fresh()`
copy. Seeds 1 to 30 (first build of the probe) and 1 to 12 (final probe): output byte-identical to main in every
case; the assert-on branch build matches too on seeds 1 to 8. Plus the 12 damaged-segment seeds of CR-bounds above.

### Point 4: the tests exercise the fast paths

Probe: `.scratch/review/cov/include` is a copy of the header with counters (`COV(n)`), `fill_test_cov`, run on
`test/fill_test.cc` unchanged. Hits: step-1 constant range 36,956; fused unpack 5,777; fallback decode 143;
`scatter` consecutive 423,058, sparse-direct 10,945, summed 41,914; bitset shifted 57,316, shift-0 996, carry
write 29,225; entry walk continuing within a group 97,909, crossing a group 2,695, stopping on the entry at
`end` 19,036; first-segment scatter 470,273. Every new path is hit by the valid-file cases, not only by the
fallback. `fill_slow_test` (PROSPECT_FILL_SLOW) turns `fill_fast` off at compile time and shares the model, so
it covers the old per-bit route. I re-ran the author's 13 mutants on a copy of the header
(`run_mutants.py`, `run_mutants.log`, same table as `test/fill_mutants.py`): 13 of 13 caught, 0 survivors.
Note: two of them (crossing block, fallback relative-ordinal) are caught only by the asserts in `scatter`
(`fill_test` has `#undef NDEBUG`), which is intended; CR1 shows the guard checks are the part the mutants miss.
Two other check-deletions survive, both equivalent to the current code given the caller contract
(`base <= doc_`): dropping `r.base + 1 < base` in `fill_for` (`:1195`) and `first < base` in `or_bitset` (`:1129`),
since `r.base >= doc_ >= base`. They are defense in depth, not load-bearing; keeping them is fine.

### Suite and fuzz replays

ASan+UBSan, `-O1`, `-DNDEBUG` (asserts off), `.scratch/review/build-asan`: `fill`, `fill_slow`, `postings`,
`fuzz_postings_replay` (779 inputs), `fuzz_segment_replay` (1,186 inputs), and `concepts`, `segment`,
`tuple_buffer`, `docids`, `bisection`, `reorder`, `concurrency`, `format13`, `fixtures`, `merge`, `multisegment`,
`media`, `exact`, `aggregate`, `docvalues`, `ordered`, `sortkey`, `collapse`, `search`, `cost`, `engine_query`,
`churn`, `directory`: all pass, no sanitizer report. The release-mode copy of the test
(`fill_test_rel.cc`, `NDEBUG` kept) was built as `fill_test_rel` and `fill_slow_test_rel`; only the build was
checked (the suite above and the mutant runs used the unmodified file).

### Non-blocking notes (follow-ups, no re-review)

- **CR3: `word_sum_` adds 1 KB, zero-initialized, to every `postings_iterator`** (`reader.hh:1364`). Counting
  unions build one iterator per clause per segment; for the many-indexes workload the construction memset and
  the larger object are per-open-index cost. The design note rejects `thread_local` for hot-path TLS cost, which
  is fair, but neither alternative (a caller-provided scratch passed to `fill`, or a window-sized stack array in
  `fill`) was measured. Not measured here (no benchmarks).
- **CR4: `fill_test`'s damaged-copy section checks canaries but not `word_sum_`** (`fill_test.cc:339-360`). The
  scratch state is private; a one-line friend or test hook would let the test check it stays zero. `damage_probe.cc`
  does this with `-DPEEK` and found nothing.
- `docs/reviews/calibration.md` is absent from this tree, so no earlier-miss list was applied.

## Not run

Crash-point suites (`index`, `reclaim`, `lifecycle`, `background_merge`, the `_mapped` variants, as told).
Benchmarks and any timing claim in `docs/perf/count-union-bits.md` (100k/1M `dump`/`verify` identity, the 1.21x
and 1.46x projections): not re-measured; the identity claim was re-derived on synthetic segments only, not on
the 962-query gate corpus. The Debug-build suites with asserts on beyond `fill`/`fill_slow` and the assert-on
differential. libFuzzer itself (only the checked-in corpus replays). TSan. Linux, FreeBSD, and arm64 (the NEON
`unpack_delta` is a different code path from the SSE one tested here). A crafted wrap block that actually
writes out of bounds with a guard removed (CR1 reasons it from the arithmetic; the survivors show the tests
do not reach it).

## Probes kept (`.scratch/review/`)

`diff_probe.cc` (+ `diff_main`, `diff_branch_rel`, `diff_branch_assert`, `out_*.txt`), `damage_probe.cc`
(+ `damage_main`, `damage_branch`, `run_damage.sh`, `run_damage.log`, `dmg_*.txt`), `scatter_probe.cc`,
`run_mutants.py` / `run_mutants2.py` (+ logs, `mut/`, `mut2/`), `cov/include` + `fill_test_cov`,
`fill_test_rel.cc`, `cc.sh` (compile one TU against any include dir with the build's flags),
`run_suite.sh` / `run_suite.log`, `main/include` (origin/main headers), `build-asan/`.
