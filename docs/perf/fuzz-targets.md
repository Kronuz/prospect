# Fuzz targets: per-input cost and the CPU-hour runs

The four libFuzzer targets (`fuzz/fuzz_{segment,postings,values,fielddict}.cc`, ASan and UBSan) ran six
independent 10-minute wall-clock starts each in release-rows, at 3 to 32 inputs a second, with each
corpus discarded. This note records what an input costs, where, what the harness cut, and the runs
that replaced them: one CPU-hour per decoder, measured as CPU time, with a corpus kept across runs.

Host: macOS x86_64, 16 CPUs, Homebrew clang 23.1.2, `-O2 -g` with `-fsanitize=fuzzer,address,undefined`;
prospect at `61e09a6` (strata `86fe7c4`, Xapiand's pins) before, `4adadb9` after.

## Per-input cost: where it goes

Measured by replaying each target's checked-in corpus under ASan and UBSan without libFuzzer
(`REPLAY_COST=1 replay_asan_T fuzz/corpus/T`: each input's CPU time, then the corpus's quantiles), and by
timing phases in scratch copies of each target (sums over the corpus, microsecond clocks).

| Target | Inputs | Mean | p50 | p90 | p99 | Max |
|---|---|---|---|---|---|---|
| segment | 986 | 13.7 ms | 6.6 ms | 36.6 ms | 76.0 ms | 297 ms (the first input builds the bases) |
| postings | 841 | 11.9 ms | 9.1 ms | 23.7 ms | 39.9 ms | 55.0 ms |
| values | 275 | 2.0 ms | 1.5 ms | 3.9 ms | 7.7 ms | 40.5 ms |
| fielddict | 751 | 8.7 ms | 1.6 ms | 15.3 ms | 65.6 ms | 73.3 ms |

A fresh libFuzzer run of 60 s from the checked-in corpus (idle host) reported 30 exec/s for segment,
51 for postings, 303 for values and 61 for fielddict: the fuzzer's own instrumentation and mutation
roughly double the replay's cost. release-rows' 3 inputs a second was a loaded host's figure, not the
target's.

Causes:

1. **segment** builds its bases once; no input builds a segment. `base()` copies one (0.3 s of the
   14 s is the one-time build, 0.06 s the checksum refresh) and `exercise` is 96% of the cost. It walks
   every document of the damaged base, so the cost is linear in the base's documents: 1.7 ms on the
   300-document base, 10.9 ms on 3,000, 14.2 and 17.0 ms on the two 9,000-document bases, 33.3 ms on
   the 20,000-document one. By phase: the per-document loop 29%, the scorer combinators 31%,
   `check_segment` 19%, the per-term walks and top-k 13%, keys scan 1%.
2. **postings** builds a segment from every input (up to 6,000 documents with positions): 46% of the
   cost. The input is the data, so this is the target's work, not overhead. The round trip and the seeded
   top-k checks are 28%, the clean walks 3.5%, the flipped-byte phase 1.5%.
3. **fielddict** rebuilt its base dictionary from the term map for every input: 29%. The oracle
   (`against_truth` on an unmutated dictionary, `check` and `agree` on a mutated one) is 51%, the
   decoders' `exercise` 13.5%, the whole-segment mode 4.5%.
4. **values** builds its three bases once; an input is 2 ms and the target runs at 300 exec/s.

## What the harness cut

1. **fielddict**: the two fixed bases' dictionaries are built once and their two strings copied per
   input (the random base still builds per input). Verdicts over the 751 corpus inputs are identical
   (610 accepted, 140 corrupt_segment, 1 unsupported); replay mean 8.7 to 7.8 ms.
2. **postings**: seeded top-k equals unseeded at 10, 100 and 1,000 hits under two presets; the shape
   byte's bits 2 and 3 now pick one size, or all three in one input of four. Replay mean 11.9 to 11.6 ms.
3. **segment**: the 20,000-document base is 6,000. Ten CPU-minutes from the checked-in corpus, same
   seed and limit (the default growth):

   | Third base | Executions | Coverage | Features |
   |---|---|---|---|
   | 20,000 documents | 14,319 | 6,620 | 18,005 |
   | 6,000 | 17,337 | 6,607 | 17,762 |
   | 4,500 | 17,861 | 6,614 | 18,041 |

   The same coverage at 21% more executions; replay mean 13.7 to 11.6 ms. The two 9,000-document bases
   still hold several skip groups.

These are small: most of an input's cost is the decoders and oracles walking the damaged data, which
is the fuzzing. Gating `exercise`'s phases by input bits would cost the same coverage per execution, so
it is not done.

## A3: the length limit

libFuzzer without `-max_len` and with the default `-len_control=100` starts at 1-byte inputs and
raises its limit as it finds coverage: release-rows' ten-minute segment runs ended at `lim: 26`, so an
input carried at most four byte changes (five bytes each). The CPU-hour runs pass `-max_len=256
-len_control=0` for segment, a limit from the first input. Ten CPU-minutes from the checked-in corpus,
third base 6,000:

| Limit | Executions | Coverage | Features |
|---|---|---|---|
| default growth (lim 26 at the end) | 17,337 | 6,607 | 17,762 |
| 64 | 23,613 | 6,629 | 18,204 |
| 256 | 23,781 | 6,668 | 18,396 |
| 1,024 | 24,269 | 6,634 | 18,019 |

256 reaches the most coverage and runs 37% more executions (larger inputs fail verification sooner).

## The CPU-hour runs

`tools/release_rows/fuzz_hour.sh` (`build`, `cost`, `run`): `run TARGET NAME CPU_SECONDS` runs the
target under `/usr/bin/time -p` in libFuzzer rounds until user plus system CPU reaches the budget
(each round's wall limit is 2% past the CPU left), over `$HOME/.scratch/fuzz-corpus/TARGET`, which
persists, with the checked-in corpus as the second directory. A job is at most 30 minutes, so each
decoder's hour is three jobs of 1,200 CPU seconds, each starting from the corpus the last left.

## Cost per input under libFuzzer, before and after

`fuzz_hour.sh cost`: each target's checked-in corpus under `-runs=0` (libFuzzer's instrumentation, ASan
and UBSan), best of three, minus the best empty-corpus startup, per input. Jobs `261113-081412-4637f9`
(before: `61e09a6`) and `261113-081415-b2fb3a` (after: `4adadb9`), strict, same inputs.

| Target | Inputs | Before ms/input | After ms/input | Change | exec/s before | exec/s after |
|---|---|---|---|---|---|---|
| segment | 986 | 36.29 | 28.22 | 22% cheaper | 27.6 | 35.4 |
| postings | 841 | 20.92 | 17.88 | 15% cheaper | 47.8 | 55.9 |
| values | 275 | 4.84 | 4.95 | unchanged (noise) | 206.6 | 202.0 |
| fielddict | 751 | 20.51 | 15.49 | 24% cheaper | 48.8 | 64.6 |

segment's change is the third base (20,000 to 6,000 documents), fielddict's the cached bases, postings'
the one top-k size per input; values is untouched. Each exec/s is 1000 over the row's ms/input, the
replay's rate and not a fuzzing run's (mutation and coverage bookkeeping add to it).

## The CPU-hour runs: results

Three jobs of 1,200 CPU seconds each per decoder, at `4adadb9` (`fuzz_hour.sh run TARGET 4adadb9d82e8
1200`, `FUZZ_SEED` 1 to 3), the corpus in `~/.scratch/fuzz-corpus/TARGET` carried across the thirds, the
checked-in corpus as the second directory. Host idle apart from the other thirds' jobs. Each third's
CPU is user plus system from `/usr/bin/time -p`.

| Decoder | CPU s | Executions | Exec/CPU s | New units | Final coverage (edges / features) | Final corpus | Peak RSS | Findings |
|---|---|---|---|---|---|---|---|---|
| segment (`-max_len=256 -len_control=0`) | 3,638.9 | 137,980 | 37.9 | 2,128 | 6,763 / 19,692 | 1,461 units, 37 KiB | 430 MiB | 0 |
| postings | 3,642.1 | 135,867 | 37.3 | 1,046 | 3,928 / 13,704 | 866 units, 143 KiB | 415 MiB | 0 |
| values | 3,645.8 | 623,388 | 171.0 | 2,450 | 2,503 / 4,636 | 483 units, 17 KiB | 404 MiB | 0 |
| fielddict | 3,669.1 | 221,349 | 60.3 | 2,645 | 4,840 / 21,717 | 1,435 units, 42 KiB | 550 MiB | 0 |

Per third (CPU s, executions, new units):

| Decoder | Third 1 | Third 2 | Third 3 |
|---|---|---|---|
| segment | 1,221.6, 54,786, 1,397 | 1,200.0, 38,384, 375 | 1,217.3, 44,810, 356 |
| postings | 1,222.5, 54,676, 633 | 1,200.5, 37,680, 223 | 1,219.1, 43,511, 190 |
| values | 1,222.1, 265,641, 1,546 | 1,205.5, 165,123, 472 | 1,218.1, 192,624, 432 |
| fielddict | 1,221.5, 73,366, 1,634 | 1,231.8, 70,086, 583 | 1,215.8, 77,897, 428 |

Each third stops when its CPU reaches the budget, so it ends within 2% past 1,200 CPU s. values and
fielddict's second thirds ran a second short round to use the CPU left (1,627 and 2,412 executions).
The final corpus is libFuzzer's merged corpus after the last third (the "corp:" of its `DONE` line);
files on disk are larger (1,752 values, 2,063 fielddict) because reduced inputs are kept beside
the originals.

Coverage still moving in the last third (3 starts at 2,400 CPU s, from its `INITED` line):

| Decoder | Edges at start | Edges at end | Features at start | Features at end |
|---|---|---|---|---|
| segment | 6,737 | 6,763 | 19,332 | 19,692 |
| postings | 3,928 | 3,928 | 13,658 | 13,704 |
| values | 2,500 | 2,503 | 4,607 | 4,636 |
| fielddict | 4,824 | 4,840 | 21,381 | 21,717 |

postings' edges are flat over the last 1,200 CPU s; the other three gain a few edges and tens to
hundreds of features, so one hour is not a plateau for segment and fielddict.

release-rows' ten-minute starts ran at 3 to 32 inputs/s with the corpus discarded; the same hour of CPU
now executes 37 to 171 inputs per CPU second, with every start from the previous one's corpus.

### A3: segment's length limit

Third 3's log shows `lim: 256` from the first line to the `DONE` line (`INFO: Seed: 301`, 1,508
files in, `-max_len=256 -len_control=0`); the other two thirds' command lines (job logs) carry the same
flags. The limit does not grow with coverage, so an input holds about ten times the byte changes (256 bytes against
26).
