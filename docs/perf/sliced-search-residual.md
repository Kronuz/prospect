# Sliced search: the residual cost of a query that fits one slice

Branch `perf/sliced-residual` from main `prospect:f817398`. The sliced search (`prospect:c77d6eb`, `docs/status/phase7c-l4.md` S6) costs
1.4-2.2% on multi-term queries against the unsliced loops (`prospect:0d7c43d`), also on a scheduler that never yields;
single words are within noise. The 7c gate's T12 compares throughput against pre-7c.

## Method

- A = `prospect:0d7c43d` (the last commit before the merge) with main's library pins, so only the search code differs;
  B = the branch. Release build, Apple clang, Intel Mac, synthetic 300,000-document index
  (`bench/quick/synth_corpus.py`, one segment), the 280 multi-term and 21 single-word queries.
- `bench/quick/ab.sh` interleaved, sides alternating by round, best of RUNS per query and round, geometric mean of B/A
  over queries; under `quiet strict`. A/A is the same binary on both sides.
- Per class (from the query text): conjunction `+a +b`, required/optional `+a b`, exclusion `a -b`, union of 2 and of 3,
  phrase. Profiles: `sample` over one class at a time, self time by function.

## Measuring

Wall time on this host is noisy: per-round ratios of one binary against a copy of itself run 0.9-1.1, and the A/A of the
whole-set geometric mean sits at 1.000-1.013. So the attribution uses two instruments:

- Instructions retired and cycles from `/usr/bin/time -l` (`.scratch/ic.py`): per pass over a query class, the difference
  of a 45-run and a 5-run `bench` (startup and page faults cancel), minimum over 7 interleaved repetitions. Instructions are
  deterministic (A against a copy of A: 1.0000 to 1.0001); cycles carry about 1.5% noise.
- `bench/quick/ab.sh` interleaved wall-time A/Bs, 10 rounds x 5 runs, per-query best, under `quiet exclusive`, with an A/A.

## Baseline: where the 2% sits (TOP_10, B = main `prospect:f817398`, A = `prospect:0d7c43d`)

| Class | n | instructions B/A | cycles B/A |
|---|---:|---:|---:|
| conjunction `+a +b` | 60 | 1.102-1.108 | 1.07-1.08 |
| exclusion `a -b` | 40 | 1.020 | 1.03 |
| union of 2 | 60 | 1.021 | 1.014 |
| union of 3 | 40 | 1.019 | 1.012 |
| required/optional `+a b` | 40 | 1.003 | 1.00 |
| phrase | 40 | 1.001 | 0.99 |
| single words | 21 | 1.005 | 0.92 (noise) |

Wall time, 8 rounds x 5 runs: `TOP_10` multi-term 1.028 [1.024, 1.031] (A/A in the same session 1.013); on the 140 required,
optional and excluded queries under `quiet exclusive`: `TOP_10` 1.052, `TOP_100` 1.048 (A/A 1.010, 1.033).

The profile (`sample`, conjunction): A's single `collect_top_k` held the scoring inline (46% of samples); B's `collect_block`
had `term_scorer::score_of` as a separate function (20% of samples), a call per scored posting. The same call sat under
`term_scorer::score()` for exclusion. Hypotheses read and excluded without an experiment: the frame allocation (the task
frame pool covers every frame here, all under 2 KiB; single words and `COUNT` carry the same frames and show 0.46% instructions
per query, about 2.7k), the vector result (moved, once per query), the budget clock (a null slice never reads it, and a
scheduler that never yields measures the same as the synchronous path).

## E1: score at the call site (kept)

- Hypothesis: the coroutine split moved the conjunction's per-posting loop into `collect_block` and `score()`'s `score_of`
  out of line; inlining it restores A's code.
- Procedure: `[[gnu::always_inline]]` on `score_of`, then the variant that survives: `score_now` always-inline, used by
  `term_scorer::score()` and the conjunction's block loop; `score_of` keeps the compiler's choice for the union's
  `score_window` and the term's own bulk loop.
- Rule: keep if instructions fall on conjunction and exclusion and none rises.

| TOP_10 instructions B/A | conj | not | reqopt | union 2 | union 3 | phrase |
|---|---:|---:|---:|---:|---:|---:|
| main | 1.102 | 1.020 | 1.003 | 1.021 | 1.019 | 1.001 |
| `score_of` always-inline everywhere | 1.016 | 0.998 | 0.989 | 1.114 | 1.081 | 1.001 |
| `score_now` at two call sites (kept) | 1.021 | 0.997 | 0.989 | 1.019 | 1.017 | - |

Global always-inline is a loser for unions (the union's `score_window` lambda grows; +11% instructions); the call-site form
keeps its gain on conjunction (1.102 to 1.021) and exclusion (1.020 to 0.997).

## E2: the union window's state in locals and pointers (kept)

- Hypothesis: `window_buffers` lives in the coroutine frame, so `below` (a `std::vector&`) and `window_min` (a `ordinal_t&`)
  are reloaded after every virtual call in the window loop, which A kept in registers as locals.
- Procedure: `below` sized once (`clauses + 1`) and used through a `double*`, filled per window; `window_min` copied to a
  local and written back at the exits.
- Rule: keep if union instructions reach parity.

| TOP_10 instructions B/A | union 2 | union 3 |
|---|---:|---:|
| E1 | 1.0205 | 1.0176 |
| E1 + E2 (kept) | 1.0016 | 1.0016 |

## E3: one call per window of blocks in the conjunction (removed)

- Hypothesis: the +2.1% left on conjunction is the per-block call, gate test and frame traffic of `collect_top_k`;
  `collect_window` loops the blocks of one 4,096-ordinal window in a plain function.
- Result: conjunction instructions 1.0193 (E2) to 1.0208: no change. Removed. The remaining difference is inside the block
  loop (B inlines different parts of `postings_iterator` than A did; `collect_block` is 857 lines of assembly against
  1,248 for A's loop, with `block_bound`, `group_bound` and `load_group` now inlined). Not attributed.

## Result (E1 + E2 against A, `quiet exclusive`, 10 rounds x 5 runs, 280 multi-term queries)

| Command | main (before) | E1 + E2 (95%) | A/A |
|---|---:|---:|---:|
| `TOP_10` | 1.028 | 1.010 (1.007-1.013) | 1.001 |
| `TOP_100` | - | 1.009 (1.005-1.012) | 0.999 |
| `TOP_100_COUNT` | - | 1.010 (1.007-1.013) | - |
| `COUNT` | - | 1.025 (1.023-1.026) | not taken |
| single words, `TOP_10` | - | 0.980 | 0.989 |

Instructions per pass, TOP_10, final: conjunction 1.021, exclusion 0.997, required/optional 0.989, union 1.002, phrase 1.001.
`COUNT` multi-term instructions are 1.0007-1.003 (conjunction, exclusion, union; required/optional 1.02 on 1.8M) with cycles
1.01-1.03, so its wall-time residual (36-77 us queries, 1-2 us each) is not instructions and is not attributed.

## Other axes and cost

- Memory: the same frames; `below` is allocated at construction (clauses + 1 doubles) instead of on the first window.
- Disk, indexing: untouched.
- Magnitude: about 1.8 points of the multi-term `TOP_k` CPU. At a 200 us mean query that is 3.6 CPU-seconds per million
  queries, a fraction of a cent; it matters for T12's ratio against pre-7c, not for cost.

## Next

1. The conjunction's remaining +2.1% instructions: compare the inlining of `postings_iterator::next/advance` between the
   loops; try outlining the group-bound path.
2. `COUNT` multi-term wall time, +2.5% with equal instructions: look at cycles with the same-core pinning unavailable on
   macOS; run on Linux where `perf stat` is available.
3. Linux, FreeBSD, arm64 numbers for the landed version (CI covers correctness only).
