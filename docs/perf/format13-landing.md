# Format 13 landing: C16's targeted A/Bs (T4's guard, T5)

Evidence for the lead, not gate rows (SYS-6): the fourth re-check's C16 item 3 asks for T4's
guard and T5's lookups as A/Bs of the rebased tip against main, since CR7's probe, CR12's
accounting, and F13-R36's test in `norm()` changed per-lookup and per-posting paths unmeasured.

- A: main `69a3f8c` (format 12), with `tools/keybench.cc` from `4df9451` added uncommitted.
- B: `phase4/format13-on-main` at `4df9451` (the merge `ff5392e`, the tuple buffer's key arena
  `76b6b5a`, keybench). The commits after it change no lookup or query path (merges' and the
  digest's key walk, `9fcd823`; C17 `492cd6e`; F13-R40 `d9a54f1`, the footer word's type only).
- Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, `-DCMAKE_BUILD_TYPE=Release`, both
  builds in their own worktree. Shared with other agents' light holds (`nostalls-*`, two or
  three threads), so every run here took a light hold of its own (`quiet run`, `QUIET_LEVEL=light`)
  and every comparison alternates sides; one thread throughout.
- Corpus: `strata-wt/index-size/.scratch/corpus-1000000.json` (and its 100,000-document
  prefix file), the gate corpus's JSON lines, indexed by `strata_engine index` (keys `Q` + id).
- Raw files: `format13/.scratch/ab/` in the developer's worktree (`idx100k/`, `idx1m/`,
  `keys_ab.out`, `q-merged/`, `q-reordered/`, `q-merged2/`, and the scripts), untracked.

## T4's guard: queries, B/A (`bench/quick/ab.sh`, `ab.py`)

Indexes: 1M documents flushed at `STRATA_BUFFER_MB=40` without merges (240 segments), then
`strata_engine merge` to one segment in docid order (`STRATA_REORDER=0`) and reordered (four
reorder threads); 962 gate queries and the 200 stratified terms. Six rounds, best of three per
query and round; geometric mean of per-query best B over best A, 95% bootstrap over queries.
0 result mismatches in every row.

| Command | Set | Merged | Reordered |
|---|---|---|---|
| `TOP_10` | queries | 0.997 [0.995, 0.998] | 0.999 [0.996, 1.001] |
| `TOP_10` | terms | 1.011 [1.008, 1.014] | 0.877 [0.867, 0.886] |
| `TOP_100` | queries | 1.002 [1.001, 1.004] | 0.997 [0.996, 0.999] |
| `TOP_100` | terms | **1.037 [1.033, 1.042]** | 0.892 [0.883, 0.902] |
| `TOP_1000` | queries | 0.996 [0.994, 0.998] | 0.994 [0.993, 0.995] |
| `TOP_1000` | terms | 0.994 [0.991, 0.997] | 0.991 [0.986, 0.995] |
| `TOP_100_COUNT` | queries | 0.993 [0.992, 0.995] | 0.993 [0.992, 0.993] |
| `TOP_100_COUNT` | terms | 1.008 [1.004, 1.012] | 0.994 [0.989, 0.999] |
| `COUNT` | queries | 0.997 [0.996, 0.998] | 0.992 [0.991, 0.993] |
| `COUNT` | terms | 0.999 [0.994, 1.005] | 0.856 [0.849, 0.863] |

The merged `TOP_100` row on the terms passed 1.03 by its upper bound, with rounds from 0.86 to
1.18, so it was rerun alone with `TOP_10`, twelve rounds of best of five:

| Command | Set | Merged, rerun |
|---|---|---|
| `TOP_10` | queries | 1.007 [1.005, 1.008] |
| `TOP_10` | terms | 1.016 [1.014, 1.019] |
| `TOP_100` | queries | 1.000 [0.999, 1.002] |
| `TOP_100` | terms | 1.021 [1.017, 1.024] |

Reading: within the guard on the rerun, but the single-term rows on the merged index sit 1% to
2% slower in both runs, consistent with F13-R36's per-posting test in `norm()` (the fourth
re-check's note); the reordered terms rows' 0.86 to 0.89 have round spreads of 0.84 to 1.18 and
are not read as a gain. The guard's row on the gate VM is the lead's.

## T5: key lookups (`tools/keybench`)

200,000 keys sampled from the corpus (seed 13), each looked up as a hit and with `~` appended
as a miss inside the key range; best of three passes per run, six runs per shape, sides
alternating; ns per lookup, median over runs, and the median of the paired B/A ratios with
their range.

| Shape | Hits A | Hits B | B/A | Misses A | Misses B | B/A |
|---|---:|---:|---|---:|---:|---|
| 1 segment (merged) | 1,209.8 | 871.6 | 0.713 (0.685 to 0.751) | 63.0 | 59.9 | 0.968 (0.891 to 1.012) |
| About 50 segments | 1,774.0 (51 segments) | 1,423.8 (49) | 0.802 (0.770 to 0.896) | 1,029.3 | 1,018.7 | 0.990 (0.972 to 1.011) |

The 50-segment indexes were flushed at `STRATA_BUFFER_MB=60` without merges; the two sides cut
51 and 49 segments from the same documents, so the misses compare 51 probes with 49. Hits are
faster in format 13 (F8's key dictionary against format 12's key term in the term dictionary);
misses are equal within the spread. Lucene's id lookup was not run (no Lucene harness on this
host).

## T5: indexing, one thread (`strata_engine index`, `/usr/bin/time -l`)

Six runs per side, alternating, default budget and auto merges; wall and CPU (user + sys) in
seconds, peak RSS in MiB; the median of the paired B/A ratios with a 95% bootstrap over runs.

| Corpus | Wall A | Wall B | B/A | CPU B/A | Peak RSS A | Peak RSS B | B/A |
|---|---:|---:|---|---|---:|---:|---|
| 100,000 | 2.315 | 2.275 | 0.980 [0.954, 1.004] | 0.980 [0.945, 0.993] | 235.2 | 237.4 | 1.016 [0.993, 1.021] |
| 1,000,000 | 30.06 | 27.80 | 0.952 [0.860, 1.012] | 0.967 [0.873, 1.012] | 715.0 | 801.4 | **1.056 [1.018, 1.227]** |

Reading: time within 1.03 by the median at both sizes, with wide intervals at 1M from the
shared host. Peak RSS at 1M is higher in format 13 in five of six pairs (A 642 to 830 MiB, B 699
to 821 MiB); not explained here. Candidates are the flush's `keys_builder`, which holds each key
as a `std::string` with its ordinal (about 40 bytes a key) where format 12 held an 8-byte hash,
and the merge's key map; both are charged to the budget, so this is peak RSS, not a budget
overrun. It is T10's to measure on the gate VM.

## Not run

The gate VM, Lucene's id lookup, the shared-pool single-index row and `perf/small-indexes`' M2
aggregate row (T5), T10's serving memory, and any run under a strict hold.
