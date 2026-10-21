# Code review: perf/pruning-floors (6f2cf15, base main cb36499)

Reviewer: code-reviewer, fresh context. Worktree `~/code/Kronuz/prospect-wt/review-pruning` (detached at
origin/perf/pruning-floors). Design/ledger read: `docs/perf/pruning-floors.md`, `docs/status/pruning-floors.md`.
`docs/reviews/calibration.md` does not exist in this repository, so no earlier-misses list was applied.

## Verdict: APPROVE WITH CONDITIONS

The margin is sound: I could not make `side_minimum` hand a side a minimum at or above the exact threshold, and it
carries about 2x headroom over the worst case I could construct. The counted seed floor is reachable only for a
top-level `disjunction_scorer` and never changed a hit or a count. One condition: the committed margin test pins
the margin at 3 leaves only, so a margin that is too small at 4 or more leaves still passes it (CR1).

Blocking items: none. Condition before landing: CR1 (a test, no code change to `search.hh`).

## What I ran

| Check | Result |
|---|---|
| ctest, Release (Apple clang -O3), 39 tests | 39/39 pass (`.scratch/review/ctest-rel.log`) |
| ctest, ASan+UBSan (Homebrew LLVM 23.1.2, `-O1`), 39 tests | 39/39 pass (`.scratch/review/ctest-asan.log`) |
| `margin_probe` (real scorers over fake leaves), release and ASan+UBSan | 387,200 checks, 246,552 with a minimum handed down, 0 violations; min slack 3.0 units of 2^-52 s |
| `margin_probe big` (n = 2^20 and 2^20+1) | 0 violations |
| `diff_probe 120` release (random scorer trees vs exhaustive, 28,800 queries, 102,007 hit checks) | 0 failures |
| `diff_probe 12` ASan+UBSan | 0 failures |
| `leaf_probe` (every posting of 10,980,948, both presets, boosts, reordered) | 0 scores above their term's bound or block bound |
| `ident.sh`: `prospect_engine dump` main vs branch, counted and `PROSPECT_DUMP_COUNT=0`, 962 gate queries, 100k merged / reordered / 4-segment flushed | 5,772 lines each, byte-identical; `verify` 0 mismatches of 962, both binaries, all three layouts |
| `edge_probe` (s = inf, NaN, DBL_MAX, subnormal, 0, negative; R >= s; n at 2^20 and 2^20+1) | all sane, see below |

Mutants (in `.scratch/review/mut-*`, each a one-line change to a copy of `search.hh`):

| Mutant | margin_probe | committed pruning_test |
|---|---|---|
| none (no margin) | 296,440 violations | fails (3 crafted + random checks) |
| explorer's `s * 2^-52` | 241,742 violations (from n = 3) | fails (1 random check only) |
| `(n+2) 2^-54 s` ("quarter") | 22,698 violations (from n = 4) | passes |
| `(n+2) 2^-53 s` ("half") | 0 violations, min slack 1.0 | passes |
| `n 2^-52 s` | 0 violations, min slack 1.0 | passes |
| minimum raised to `s - R + 0.3 s` | n/a | `diff_probe` 657 failures (the differential has teeth) |

## 1. The margin (`search.hh:976-981`, called at 954 and 957)

Claim attacked: `x <= side_minimum(s, R, n)` implies `x + y < s` exactly, where y is the other side's score, R its
bound, n its leaf count, for every n up to the cap.

What I did:
1. Re-derived it. With every leaf score at most its leaf bound (as doubles), any two summation trees of the same n
   leaves satisfy `r <= R (1 + gamma_{n-1}) / (1 - gamma_{n-1}) = R (1 + 2 (n-1) 2^-53 (1 + O(n 2^-53)))`. Add the half
   ulp of `fl(s - R)` and of the second subtraction (each at most 2^-53 s), `nextafter`: the margin `(n+2) 2^-52 s` leaves
   about `4 * 2^-53 s` of spare. The second-order term at n = 2^20 is about 2^-65 s. The derivation in the comment holds.
2. Checked the premise it rests on, leaf score <= leaf bound exactly, on real postings: `leaf_probe.cc`, 10.98M postings,
   both presets, random boosts and wqf, docid-ordered and reordered corpora: 0 exceptions. Phrases keep it by
   `std::min(sum, bound_)` or by summing in slot order (`search.hh:1162-1185`); `score_terms()` for a phrase is its slot
   count, conservative.
3. Hill-climbed leaf values, costs and tree shape to maximise `r - R` (`margin_probe.cc`): flat conjunction and disjunction
   at n = 1..4096, 16 half-ulp patterns, 400 random trees of up to 41 leaves mixing AND/OR/AND_MAYBE, and n = 2^20 and
   2^20+1. Worst `(r - R) / (n 2^-52 R)` found: 0.5000 (a big leaf plus n-1 leaves just over half its ulp). Then, for
   about 130 values of s around the point where the minimum first appears (and 4 ulps either side), asked the real
   `required_optional_scorer` (both roles, so the required side's minimum is covered too) for the minimum and tested
   `m + r < s` with an exact two-sum comparison. 0 violations, in release and under ASan+UBSan.
4. Probe power: the no-margin, explorer's and quarter-margin mutants are caught (rows above), so the probe would see a
   short margin. The half margin and `n 2^-52 s` also pass, so the real margin has 2x over the worst case I reached;
   my search did not reach the theoretical ratio of 1.0, so the true headroom is between 1x and 2x, still positive.
5. Presets: the margin depends only on summation order, which is identical (`sum_parts`, given order) in both; the
   differential probe and `leaf_probe` ran both presets.
6. Float: there is no float scoring path (grep: no `float` in `search.hh`/`searcher.hh`); `0x1p-52` is binary64 only.
   A float-valued scorer would need its own constant. Not applicable here.
7. Edge inputs (`edge_probe.cc`): s = inf and NaN give no minimum (NaN fails `m > 0`); s = DBL_MAX, a subnormal s, and
   R = 0 give a minimum below s; `R >= s` and `s <= 0` give none; n = 2^20 gets a minimum, n = 2^20+1 does not.
8. The cap: `n > 2^20` returns no minimum (a performance loss only, never a wrong result). 2^20 is above Lucene's
   BooleanQuery limit (1024) and Tantivy has none, so it is not a tighter limit than the peers. See CR2 for the
   comment's rationale.

## 2. The seed floor on counted queries (`search.hh:1398-1399`, `857`)

Reading: `counts_apart()` is true only for `disjunction_scorer`; `live_scorer` and everything else keep zeroing the
floor. Counted `top_k` with a floor seeds the collector (`c.seed(floor)`), does not call `set_min_competitive_score`,
and `disjunction_scorer::collect_top_k` counts on a fresh copy (`count(*copy)`) before any pruning, or, when no copy
exists, sets `min = 0` (`count_here`). So pruning cannot reach the count.

Probe: `diff_probe` runs every tree counted with floors at `nextafter(kth)`, `kth (1 - 2^-40)` and `kth / 2` (valid
strictly-lower floors), in a docid-ordered, a tie-heavy equal-length and a reordered corpus: hits equal exhaustive
scoring bit for bit and the count equals the exhaustive match count, 0 failures. Through `search()` over two segments
(with deletions in some): same, counted and uncounted. A trace build (`mut-trace`) counts calls where a counted query
kept a floor: 449 calls, 0 on a scorer that is not a `disjunction_scorer`. The engine's own counted `dump` (which passes
`seed(k)`) is identical to main on all three layouts.

The floor never exceeds the true k-th: it is the seed (`strictly lower` by construction, `(1 - 2^-40)` slack,
unchanged by this branch) or what a caller derives the same way, and `search()` takes no seed from a segment with deletions
(`searcher.hh:212-215`). The only way to exceed the threshold is a caller passing a floor above the k-th, which breaks
the documented contract (CR5).

## 3. Reachability

New optional-side minimum (`search.hh:957`): fires from `required_optional_scorer::set_min_competitive_score`, which is
called by `top_k` and `search()` only on uncounted queries, or counted ones whose count is exact from statistics
(`count` is then null); and by forwarders: `exclusion_scorer` (to its include side), `live_scorer`, and a parent
`required_optional_scorer`. `conjunction_scorer` stores the minimum and `disjunction_scorer` does not forward, so a
`req_opt` below those never receives it, as on main. The trace build recorded 24,583 optional-side and 11,798
required-side minimums in the 1,920-query run (the trace does not separate counted calls; that no counted scoring query
reaches them is from the code: `top_k` zeroes the floor and `search()` passes no minimum when `c.counting`). This matches the write-up. The
`prune()` windows (`search.hh:991-1015`) were not changed; the differential probe covers `MAYBE(x, OR(..))` and
nested shapes at 5 or more clauses (heap mode) and 4 or fewer (linear mode).

Seed floor: reachable only as in section 2.

## Findings

**CR1 (condition, correctness test).** `test/pruning_test.cc:41-130` pins the margin with fixed 3-leaf cases and
random cases of 1-5 leaves. A margin weakened to `(n+2) 2^-54 s` passes the committed test and the full ctest, and
`margin_probe` shows it dropping competing documents from n = 4 up (22,698 violations; e.g. role=1, n=4..4096). So the
n-scaling, the part that fixes main's bug at more than 3 terms, has no regression test. Add a case at several n (for
example 4, 16, 64, 1024) using `margin_probe.cc`'s `pattern(kind, n, 1.001, 1.0)` (one leaf of 1.0 plus n-1 leaves of
`1.001 * 2^-53`, random costs, built with the existing `conjunction` helper) and the exact check
`m + r < s` taken from `fake_scorer::min_`. Probe: `.scratch/review/margin_probe.cc`, mutant
`.scratch/review/mut-quarter` (`margin-mut-quarter.log` shows the violations a new test must catch).

**CR2 (note).** The comment at `search.hh:973-975` says that past 2^20 leaves the second-order term "is no longer
covered". By the derivation it is covered with room up to about 2^26 (the spare `2 * 2^-53 s` against
`2 (n-1)^2 2^-106 s`). The cap is only conservative, and past it the code safely drops the minimum. Reword or leave.

**CR3 (note).** `scorer::score_terms()` defaults to 1 (`search.hh:139`). A future composite scorer that forgets to
override it silently gets a one-leaf margin. A pure virtual, or a comment at the class that every composite must
override, would catch that. In-tree all composites override it (checked: conjunction, disjunction, required-optional,
exclusion, phrase, `live_scorer`); the test wrapper `scoring_tally` (`test/search_test.cc:26-57`) does not forward it, harmless
there since it wraps leaves.

**CR4 (note, scope of the request).** "float and double": nothing in the scoring path is float; only binary64 was
tested. See margin item 6.

**CR5 (note).** A counted `top_k` with a floor above the k-th now returns only hits above the floor; on main the
floor was ignored for counted queries. Every in-tree caller passes a strictly lower bound (the seed), and
`test/search_test.cc` only tests above-k-th floors uncounted. The contract at `search.hh:1395-1397` already says "a strictly
lower bound"; no change needed.

**CR6 (open, not reviewed here).** The write-up's caveat of a 1-3% slowdown on classes neither change reaches, with
both new virtual slots on `scorer`, is unexplained. I ran no timing. The lead's measurement on main decides.

## Conditions

1. Land CR1's test (and confirm it fails on `mut-quarter` and passes on the branch) before merging.
2. Lead decides CR6 on their own measurement.

## Files kept

All in `.scratch/review/` of the worktree: `build.sh`, `pb.sh` (probe builds), `pbe.sh` (engine from main/branch),
`mkmut.sh` and `mut-{explorer,quarter,half,nomargin,nplus0,overprune,trace,main}`, `margin_probe.cc`, `diff_probe.cc`,
`leaf_probe.cc`, `edge_probe.cc`, `pruning_test_copy.cc`, `ident.sh`, `tests.sh`, `runprobes.sh`, `queries.txt`, and the
logs (`margin-*.log`, `diff-*.log`, `ctest-*.log`, `ident.log`, `ident/`).

## Not run

Timing and every number in the write-up's Time table (no benchmarks by instruction); the 1M merged and 1M reordered
identity runs; TSan; Linux, FreeBSD and arm64; a Debug build with asserts; fuzzing beyond the `fuzz_*_replay` ctest
entries; phrase scorers inside `diff_probe` trees (phrases were covered only by the existing `search_test`); segments
with a docid-reordered layout and deletions together; `calibration.md` (absent).
