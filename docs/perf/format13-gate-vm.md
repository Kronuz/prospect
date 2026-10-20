# Format 13 on the gate VM: the confirmation rows before the freeze

The lead's confirmation of format 13 with its query items (main `8110f1d`: M16 at A3@128, M23,
M20's seeds, M3t; M22's code reserved) at 5M on the gate VM, before `pre_freeze` goes to 13. It
answers the f13-query code review's conditions C1 (FQ1, FQ2) and C3 (FQ4), the joint selection's
bytes (F13-R8, D15), T4's guard at the run level, and format 13's owed rows (the merged index's
single-term `TOP_100`, T10's memory).

## Plan (written before the runs)

- **Binaries.** A = main `026ee10` (format 13 before the query items, `pre_freeze` 0x80000001);
  B = main `8110f1d` (0x80000007); F12 = main `69a3f8c` (format 12), for the owed rows only. Each
  from `git archive` of its commit, `strata_engine` in Release with the VM's clang 18.
- **Indexes.** The 5M gate corpus (`corpus.json`, 5,032,104 lines), `STRATA_AUTO_MERGE=0` at the
  default 256 MB buffer, then `strata_engine merge` in place: `STRATA_REORDER=0` ("merged") or
  `STRATA_REORDER_THREADS=4` ("reordered"). A merged directory keeps its sources until they are
  removed by hand (only the manifest's segment is kept). With 12 GB free, the layouts run one after
  the other: both sides reordered first, then both merged.
- **Rows and what decides them.**
  1. Identity (C1, FQ1, FQ2): `strata_engine dump` (with a count) and the review's
     `probe_search dump` (no count: union seeds apply) of A and B, byte-compared, on the 962 gate
     queries and 200 stratified terms; `strata_engine verify` and `probe_search verify` on B.
     Passes only with 0 differing lines and 0 mismatches on both layouts.
  2. Bytes (D15): B's postings and whole index from `strata_engine sections`, against keyed
     Lucene `-bp`'s `.doc` (833.7 MB) and whole index (2,542.5 MB), and beside Lucene and
     Tantivy. Exact; no hold.
  3. Merge times (C3, FQ4): each 5M merge of A and B timed with `/usr/bin/time -v`, alternating
     sides (reordered A then B, docid order B then A); one pair per kind, since a reordered pair is
     about half an hour. Indexing time and peak RSS of the same builds (two per side) reported too.
  4. Queries at the run level (`ab-run-level.md`): `session.sh` (A/A of eight rounds, then A/B)
     A against B on S′, both layouts, both presets; and strata (B, reordered) against Lucene,
     Lucene `-bp`, Tantivy, and PISA through the harness, five trials on S′ with a second strata
     engine for the A/A. S′ (361 gate queries, 100 terms) replaces the full set: the full set is
     2.5 times S′, about 75 minutes a session with six engines.
  5. Owed rows: the merged index's terms at `TOP_10` and `TOP_100`, F12 against B, in-process
     with an A/A; peak memory: indexing peak RSS of F12, A, B at 5M, and T10's serving memory
     (`bench/serve_mem.py`) of F12 and B merged in one session.
- **Holds.** Light for one-thread timing (sessions, docid merges, indexing, serving memory),
  strict for the reordered merges (four threads); builds and fixtures at `nice -n 19`, four jobs.

## Results

Gate VM: Ubuntu 24.04 under QEMU/HVF on the Intel i9-9980HK host (8 vCPUs, 16 GB), clang 18,
`-DCMAKE_BUILD_TYPE=Release`. Raw files in the VM's `~/bench/results/f13g/` (`phases.log` has
every step's wall time).

### Row 1: identity at 5M (C1, FQ1, FQ2)

Over the 962 gate queries and 200 stratified terms (1,162 lines):

| Index | `dump` A against B (k 10, 100, 1,000, both presets, with a count) | `probe_search dump` (k 1, 10, 11, 100, 1,000, 1,001, both presets, no count) | `strata_engine verify` on B | `probe_search verify` on B (with and without a count) |
|---|---|---|---|---|
| Reordered | identical, 6,972 lines | identical, 13,944 lines | 0 mismatches of 1,162 | 0 mismatches of 1,162 |
| Merged (docid order) | identical, 6,972 lines | identical, 13,944 lines | 0 mismatches of 1,162 | 0 mismatches of 1,162 |

Phase times (reordered): the two `dump`s 38 to 40 s in parallel; the no-count dumps 23 to 25 s;
`verify` 131 s; the probe's `verify` 191 s (its first attempt ran beside `verify` and was refused
by the journal lock, so it was rerun alone). Merged: dumps 41 s, no-count dumps 29 s, `verify`
128 s, the probe's `verify` 207 s. The run-level sessions (rows 4a and 5) also compare every query's count or top-k
result between the sides in every round: 0 mismatches, including format 12 against B.

The dumps are also byte-identical across layouts: A's and B's reordered and merged dumps share one
SHA-256 with a count (`136edf25…`) and one without (`92e1832f…`), so reordering changes no
result either (`id-*/dumps.sha256`).

### Row 2: bytes at 5M (D15)

`strata_engine sections` of the merged segment (bytes):

| Section | A reordered | B reordered | B − A | Keyed Lucene `-bp` | B against it |
|---|---:|---:|---:|---:|---|
| Postings (`.doc`) | 845,553,044 | 843,636,006 | −1,917,038 (−0.23%) | 833,674,096 | 1.2% larger (+9,961,910) |
| Positions (`.pos`) | 1,525,049,222 | 1,525,049,222 | 0 | 1,556,127,616 | 2.0% smaller |
| Terms, term index, keys, key index (`.tim`, `.tip`) | 85,179,526 | 85,179,481 | −45 | 75,832,514 | 12.3% larger |
| Stored (`.fdt`, `.fdx`, `.fdm`) | 24,935,785 | 24,935,785 | 0 | 51,739,215 | 51.8% smaller |
| Whole file | 2,527,529,596 | 2,525,610,636 | −1,918,960 (−0.08%) | 2,542,535,895 | 0.67% smaller |

Beside the other baselines (whole index; postings as each engine's postings file with
frequencies and skip data, positions excluded):

| Engine | Postings | B reordered's against it | Whole index | B reordered's against it |
|---|---:|---|---:|---|
| Keyed Lucene 10.4.0 (no reordering; `~/keyed-lucene`) | 985,623,085 | 14.4% smaller | 2,819,677,322 | 10.4% smaller |
| Keyed Tantivy 0.26 (`tantivy-0.26-st`, `.idx`) | 1,067,081,129 | 20.9% smaller | 3,066,216,363 | 17.6% smaller |
| PISA 0.8.2, unkeyed, reordered (`inv.simdbp`; no positions) | 1,065,643,487 | 20.8% smaller | 1,384,946,259 (no positions or stored) | not comparable |

Merged (docid order), with format 12 (F12, main `69a3f8c`, its own fresh build) beside them:

| Section | F12 merged | A merged | B merged | B − A | B against F12 |
|---|---:|---:|---:|---:|---|
| Postings | 1,007,889,746 | 1,007,889,746 | 1,005,615,335 | −2,274,411 (−0.23%) | 0.23% smaller |
| Positions | 1,631,157,618 | 1,631,157,618 | 1,631,157,618 | 0 | equal |
| Terms, term index, keys, key index (F12: terms, term index, `pk_filter`) | 97,666,972 | 85,930,465 | 85,930,263 | −202 | 12.0% smaller |
| Stored | 15,489,384 | 15,489,384 | 15,489,384 | 0 | equal |
| Whole file | 2,770,003,548 | 2,758,255,204 | 2,755,978,368 | −2,276,836 (−0.08%) | 0.51% smaller |

**Reading.** The query items take 1.92 MB off the reordered postings and 2.27 MB off the merged
ones at 5M (0.23% each), against the joint selection's projection of about 1.4 MB from 1M. The
postings row is not closed: B's reordered postings are 9.96 MB over keyed `-bp`'s `.doc` (1.2%
larger; projected about 10.5 MB), so D15's constraint misses by that much, while the whole index
is 16.9 MB under keyed `-bp`'s (0.67% smaller). A4@128 (+4.4 MB) stays unpaid for. PISA's
postings (1,065.6 MB) are larger than keyed `-bp`'s `.doc`, so keyed `-bp` stays the reference
(F13-R29).

### Row 3: merge times (C3, FQ4)

One reordering merge per side, A then B, four reorder threads, strict hold; `/usr/bin/time -v`:

| Merge | A wall | B wall | B/A | A CPU (user + sys) | B CPU | A peak RSS | B peak RSS |
|---|---:|---:|---|---:|---:|---:|---:|
| Reordering, 16 segments to one | 1,063.3 s | 1,054.7 s | 0.992 | 2,705.0 s | 2,683.0 s | 4.76 GiB | 4.85 GiB |
| Docid order, 16 segments to one (B first, then A) | 141.8 s | 155.0 s | **1.093** | 138.4 s | 154.7 s | 4.74 GiB | 4.75 GiB |

Format 12's docid merge (F12) took 184.8 s at 4.79 GiB, run once right after its own index build
read the corpus cold (184.4 s CPU, 23 s of it system time), so it is reported, not compared.

The indexes these merges read, built one thread at the 256 MB buffer without merges, the corpus
warm in the page cache (A, B, B, A by time; F12 once, from a cold corpus):

| Build | A wall (CPU) | B wall (CPU) | B/A | A peak RSS | B peak RSS | F12 |
|---|---|---|---|---|---|---|
| Reordered layout's (A first) | 132.6 s (132.1 s) | 143.7 s (143.6 s) | 1.084 | 572.0 MiB | 573.3 MiB | |
| Merged layout's (B first) | 128.6 s (128.3 s) | 143.5 s (143.1 s) | 1.116 | 573.5 MiB | 573.5 MiB | 218.0 s (216.0 s, cold corpus), 604.5 MiB |

**Reading.** The reordering merge does not move (0.992; one pair, bisection dominates). The docid
merge is 1.09 times slower with the query items (+13.2 s wall, +16.3 s CPU), and so is indexing:
1.08 and 1.12 times slower in both orders, about +13 s of CPU per 5M documents. Both are one or two
pairs, so their intervals are not established, but the index builds agree in both orders and in
CPU time, and the docid merge's CPU moves by the same share. C3's figure is therefore a loss on the
indexing axis of about 10% at 5M; the attribution is below (Row 3's profile).

**The profile.** `perf record -e cpu-clock -F 999` (no PMU in the guest) over a 1M flushed build
and its docid merge per side, light hold (`prof/`): `inverted_builder::seeds_of`, M20's writer, is
11.3% of B's build samples (2,331 of 20,607) and 11.5% of its merge's (2,219 of 19,333); A has no
such function. B's builds take 2,370 samples more than A's (18,237) and its merges 2,564 more
(16,769): the seeds are 98% of the build's difference and 87% of the merge's. The 1M docid merges
took 24.1 s (A) and 27.8 s (B) under the profiler. FQ4's reading (linear, nothing superlinear)
holds; the constant is what costs.

**Against format 12** (T5's guard, indexing at most 1.03 of format 12's time), the same 5M
build alternating F12 and B, warm corpus, after the rows above:

| Pair | F12 wall (CPU) | B wall (CPU) | B/F12 | F12 peak RSS | B peak RSS |
|---|---|---|---|---|---|
| 1 | 133.4 s (133.1 s) | 144.1 s (143.9 s) | 1.080 | 604.6 MiB | 576.8 MiB |
| 2 | 131.5 s (131.2 s) | 139.2 s (138.9 s) | 1.059 | 604.6 MiB | 576.8 MiB |

A (format 13 without the query items) sits with format 12 (130.6 s mean against 132.4 s), so the
format is neutral and the query items' writer costs 7 to 10%: T5's indexing guard misses by
about 4 to 5 points at 5M. It is writer-side work, not bytes: a cheaper `seeds_of` that writes the
same pairs leaves the format as it freezes. In cost terms the loss is small: about 2.4 µs of CPU
per document indexed and 3.3 µs per document per docid merge, so a billion documents written once
and merged three times pay about 3.4 core-hours more, roughly $0.15 at $0.045 per vCPU-hour. It
is the ratio that matters, against the indexing ratchet.

### Row 4a: B against A at the run level (T4's guard)

`session.sh` (`ab-run-level.md`): an A/A of eight rounds, then the A/B of eight, on S′ (361 gate
queries, 100 terms), every command, one CPU pinned, light hold. B/A below 1 means B is faster.
Every A/B round's results equal A's (0 mismatches in every query-level row).

| Session | Wins / losses (125 rows) | A/A band, widest | term:frequent `TOP_10` | term:frequent `TOP_1000` | union `TOP_10` | union `TOP_1000` | intersection `TOP_10` | `COUNT` union:num_tokens_>3 | phrase `TOP_1000` | term:rare `TOP_10` |
|---|---|---|---|---|---|---|---|---|---|---|
| Reordered `bm25` | 31 / 0 | 0.794–1.195 | 0.392 win | 0.470 win | 0.888 win | 0.848 win | 0.934 win | 0.878 win | 1.015 [0.995–1.035] | 1.094 [1.047–1.142], A/A to 1.123 |
| Reordered `bm25_xapian` | 22 / 0 | 0.762–1.420 | 0.385 win | 0.710 win | 0.881 win | 0.842 win | 0.956 | 0.902 | 0.989 | 1.012 |

Phases: A/A 286 s and A/B 282 s (`bm25`); 404 s and 377 s (`bm25_xapian`).

No row loses. The rows that lean the other way, all "neither": rare terms under `bm25` (1.09 at
`TOP_10`, 1.06 at `TOP_100_COUNT`, 1.05 at `TOP_1000`, inside an A/A band to 1.12 to 1.16; under
`bm25_xapian` 1.01), and two-term phrases at `TOP_1000` (1.026 [1.011–1.042] against an A/A band
of 0.984–1.006: beyond the band, under the 1.03 guard). By set (query-level, best over rounds):
B is 1.08 to 1.10 times faster on the queries under `bm25` (0.908 to 0.927 on four commands,
`COUNT` 0.911) and 1.38 to 1.55 times faster on the terms at the top-k commands (0.645 to 0.724),
with terms `COUNT` at 1.015 [1.007–1.024], 1.5% slower.

Merged (docid order), the same method; phases 263 s and 255 s (`bm25`), 316 s and 301 s
(`bm25_xapian`):

| Session | Wins / losses (125 rows) | A/A band, widest | term:frequent `TOP_10` | term:frequent `TOP_1000` | union `TOP_10` | union `TOP_1000` | intersection `TOP_10` | `COUNT` union:num_tokens_>3 | phrase `TOP_1000` | term:rare `TOP_10` |
|---|---|---|---|---|---|---|---|---|---|---|
| Merged `bm25` | 9 / 0 | 0.793–1.348 | 0.710 win | 0.796 | 0.923 | 0.936 | 0.972 | 0.976 | 1.025 [1.003–1.047] | 1.081 [1.015–1.151], A/A to 1.080 |
| Merged `bm25_xapian` | 10 / 0 | 0.743–1.475 | 0.639 win | 0.886 win | 0.962 | 0.942 | 0.991 | 0.963 | 1.033 [1.019–1.048], A/A to 1.216 | 1.021 |

**Reading (4a).** B loses no row against A in any of the four sessions (C1's identity holds in
every round), with 9 to 31 wins, the frequent terms the largest (0.39 to 0.71 at `TOP_10`). The
merged sessions' A/A bands are wider (to 1.35 and 1.48), so fewer rows clear them. Two leans recur,
each "neither" in every session:
1. **Phrases, 1.5% to 3.3% slower** at `TOP_1000` (1.015, 1.025, 0.989, 1.033). It fits FQ3 item
   2's mechanism: the positional deltas are found by stepping over a group's impact lists, so a
   phrase now parses them.
2. **Rare terms and terms' `COUNT`, up to 1.09 and 1.15 by ratio, but tenths of a microsecond:**
   rare terms' median `TOP_10` is 2.4 µs (A) against 2.6 µs (B) reordered and 1.9 against 2.4 µs
   merged; terms' `COUNT` 0.8 against 0.9 µs, at the timer's 0.1 µs resolution. A fixed cost per
   query of about 0.2 to 0.5 µs (FQ7's every-scorer-first `search()` is the candidate), visible only
   where a query costs a few microseconds.

### Row 4b: strata against the baselines (harness, run level)

`src/client.py` through `harness.sh`: B's reordered 5M index as `f13g-strata`, the same binary
and index again as `f13g-strata-aa` for the A/A, Lucene 10.4.0, Lucene 10.4.0 `-bp`, Tantivy 0.26,
and PISA 0.8.2, each over its own force-merged 5M index; five trials over S′ with the engine order
rotated, `WARMUP_TIME=5`, `NUM_ITER=5` (D17 item 7's settings), light hold. Each trial is a run;
`bench/quick/engines_ab.py` writes the runs out and `ab.py --aa` scores them, B/A being strata's
time over the engine's (below 1: strata faster). Counts equal Lucene's, Lucene `-bp`'s, and
Tantivy's on every query of every trial; PISA's differ on all 40 intersection_union and all 19
negated queries in every trial, so PISA does not answer those queries as the others do and its
rows for them are not comparable. Trials 159 to 165 s (terms) and 298 to 339 s (queries); 39 min
in all.

By set, query-level geometric mean of per-query best times (strata over engine):

| Command | Lucene, queries / terms | Lucene `-bp` | Tantivy | PISA (terms; queries include classes PISA answers differently) | A/A |
|---|---|---|---|---|---|
| `TOP_10` | 0.710 / 0.768 | 0.934 / 0.847 | 0.887 / 0.924 | 1.318 / 0.448 | 1.002 / 0.972 |
| `TOP_100` | 0.677 / 0.630 | 0.876 / 0.751 | 0.932 / 0.828 | 1.515 / 0.562 | 0.969 / 0.940 |
| `TOP_1000` | 0.675 / 0.799 | 0.882 / 0.842 | 0.970 / 0.912 | 1.917 / 0.750 | 1.006 / 0.970 |
| `TOP_100_COUNT` | 0.692 / 0.510 | 0.850 / 0.509 | 1.079 / 0.648 | 1.628 / 0.574 | 0.990 / 1.048 |
| `COUNT` | 0.748 / 0.951 | 0.913 / 0.943 | 0.938 / 0.934 | 0.557 / 0.995 | 0.991 / 1.042 |

Run level, every class and stratum row (125; PISA 100), against the same trials' A/A band:

| Against | Wins | Losses | The losses |
|---|---:|---:|---|
| Lucene 10.4.0 | 98 | 2 | union:num_tokens_>3 (8 queries): `COUNT` 2.30 times slower, `TOP_100_COUNT` 1.09 |
| Lucene 10.4.0 `-bp` | 54 | 17 | intersection_union at every command but `COUNT`, 1.11 to 1.41 times slower; union:num_tokens_>3 `COUNT` 2.77 and `TOP_100_COUNT` 1.16; intersection:num_tokens_3 `TOP_100_COUNT` 1.11 |
| Tantivy 0.26 | 27 | 32 | phrases at every command, 1.10 to 1.21; intersections at `TOP_1000` 1.15 (two terms 1.17, more than three 1.25) and `TOP_100_COUNT` 1.18; counted unions of three or more terms at `TOP_100_COUNT` 1.71 and 1.97; intersection_union at `TOP_100`, `TOP_1000`, and `TOP_100_COUNT` 1.08 to 1.24; negated `TOP_1000` 1.10 and two-term negations at `TOP_100_COUNT` 1.27; two-phase-critic 1.21 to 1.30 |
| PISA 0.8.2 (the 60 comparable rows: terms, intersections, unions) | 10 | 27 | unions of two and three terms and intersections at the top-k commands: unions 1.39 (`TOP_10`) to 2.40 (`TOP_1000`), intersections 1.25 to 1.44 at `TOP_100` and `TOP_1000`, `TOP_100_COUNT` 1.43 to 1.80; `COUNT` intersections of three or more 1.09 and 1.25. Wins: frequent terms at every top-k command (0.18 to 0.54), unions of more than three terms at `TOP_10` and `TOP_100` (0.65, 0.72), and `COUNT` unions (0.18 to 0.57) |

**The warm-up.** D17 item 7 asked for a check of the 5 s warm-up against 20 s, which I found no
record of. One trial at `WARMUP_TIME=20` (Lucene, Lucene `-bp`, Tantivy; S′; 375 s and 304 s;
`h-w20/`) ran every engine faster than the five 5 s trials did. The geometric mean of per-query
best times, 20 s over each 5 s trial, ranged 0.49 to 1.02 for Lucene `-bp`, 0.50 to 1.02 for
Lucene, and 0.66 to 1.03 for Tantivy, whose native code has no JIT to warm. With no strata engine
in that trial, host drift and warm-up cannot be told apart. The JVM engines gain somewhat more
than Tantivy at the top-k commands (`TOP_100` queries: `-bp` 0.67 to 0.87 against Tantivy's 0.73
to 1.00), so row 4b's ratios against Lucene and Lucene `-bp` may flatter strata by up to about
10%. Its wins against the JVM engines are therefore not established at that margin, and its losses
would only grow.

### Row 5: format 13's owed rows

**The merged index's single terms, F12 against B** (the C16 note's `TOP_10` 1.016 and `TOP_100`
1.021 at 1M on `format13-on-main`): `session.sh` with `TOP_10` and `TOP_100` only, the 200
stratified terms and S′'s 361 queries, eight rounds each, F12's fresh merged 5M index against B's;
phases 72 s and 68 s (`bm25`), 83 s and 80 s (`bm25_xapian`). Counts and results equal format 12's
in every round.

| Row (B/F12 [95% over runs]; A/A band) | `bm25` | `bm25_xapian` |
|---|---|---|
| Wins / losses (50 rows) | 16 / 0 | 15 / 0 |
| term `TOP_10` | 0.833 [0.817–0.850] win | 0.841 [0.833–0.850] win |
| term `TOP_100` | 0.802 [0.780–0.823] win | 0.836 [0.804–0.869] win |
| term:frequent `TOP_10` / `TOP_100` | 0.689 / 0.748 win | 0.668 / 0.756 win |
| term:rare `TOP_10` / `TOP_100` | 1.019 / 1.017 (A/A to 1.03, 1.11) | 1.026 [1.003–1.049] / 1.029 [1.008–1.050] (A/A 1.000–1.000, 0.970–1.021) |
| Terms set, query level, `TOP_10` / `TOP_100` | 0.804 / 0.836 | 0.786 / 0.853 |

The owed row is closed at 5M: format 13 with the query items is 1.20 to 1.25 times faster than
format 12 on the merged index's single terms at both commands and presets. Rare terms stay the one
lean (1.7% to 2.9% slower, "neither"; the fixed per-query cost of row 4a, item 2).

**Peak memory.**

| Measure | F12 | A | B | B against F12 |
|---|---:|---:|---:|---|
| Indexing peak RSS, 5M flushed build (MiB; each run) | 604.5, 604.6, 604.6 | 572.0, 573.5 | 573.3, 573.5, 576.8, 576.8 | 4.6% to 5.2% smaller |
| Docid merge peak RSS, 16 segments to one | 4.79 GiB | 4.74 GiB | 4.75 GiB | 0.8% smaller |
| Reordering merge peak RSS | | 4.76 GiB | 4.85 GiB | (B 2.0% larger than A) |
| Serving memory, merged (`serve_mem.py`, 962 queries × `TOP_10`, `COUNT`, `TOP_1000`, page cache dropped; three rounds, order alternating) | 581 MB (8 anonymous, 573 to 574 mapped) | | 581 MB (8, 573) | equal at 1 MB resolution |

T10's serving row passes (equal to format 12's in one session). The 1M note's indexing peak RSS of
1.056 against format 12 (with auto merges, on macOS) does not appear at 5M on Linux: format 13's
flushed builds peak 5% lower, before and after the query items. That build has no merges; the
merges' peaks are within 2% across F12, A, and B.

## Verdict for the freeze

Rows that pass:
1. **C1 (FQ1, FQ2):** results at 5M are identical to main before the query items, with and without
   a count, on both layouts, and `verify` is clean both ways. The run-level sessions found no
   differing result in any round, format 12 included.
2. **T4's guard:** B loses no row against A in four sessions (both layouts, both presets), with 9 to
   31 wins.
3. **The owed single-term row:** against format 12 on the merged index, B is 1.20 to 1.25 times
   faster on terms at `TOP_10` and `TOP_100`.
4. **T10's serving memory:** equal to format 12's. Indexing peak RSS is 5% lower.
5. **C3's merge times are recorded.** Reordering does not move (0.992).

Rows that miss:
1. **D15's postings constraint.** B's reordered postings are 9.96 MB over keyed `-bp`'s `.doc`
   (1.2% larger), as the joint selection projected (about 10.5 MB). The whole index is 0.67%
   smaller. This decides the freeze unless the row is carried.
2. **Indexing, about 10% slower.** Indexing is 1.06 to 1.08 times format 12's time and 1.08 to 1.12
   times A's; the docid merge is 1.09 times A's. M20's `seeds_of` is 11% of the CPU of both. T5's
   ≤ 1.03 misses. The fix is in the writer, not the bytes: the same pairs, computed more cheaply.
3. **Against the baselines (row 4b, informational):** strata wins most rows against Lucene. Its
   losses:
   - Lucene `-bp`: intersection_union, 1.11 to 1.41 times slower.
   - Tantivy: phrases, 1.10 to 1.21; counted unions of three or more terms at `TOP_100_COUNT`,
     1.7 to 2.0.
   - Lucene and Lucene `-bp`: counted unions of more than three terms, 2.3 and 2.8 times slower.
   - PISA's comparable rows: unions and intersections at the top-k commands, 1.25 to 2.4.

   Those ratios rest on a 5 s warm-up not shown to be enough for the JVM engines.

Leans to watch, each "neither" in every session:
1. **Phrases at `TOP_1000`, 1.5% to 3.3% slower than A.** The cause is FQ3 item 2's layout, which
   C2 rules on: the positional deltas are found by walking the impact lists.
2. **A fixed cost of about 0.2 to 0.5 µs per query,** visible on rare terms.

## Phase times

| Step | Wall |
|---|---|
| Builds (A, B, F12), `nice -n 19`, four jobs | 58 s, 64 s, 57 s |
| 5M flushed indexes | 128.6 to 144.1 s each (eight warm builds; F12's cold one 218 s) |
| Reordering merges, strict hold | 1,063 s, 1,055 s |
| Docid merges | 142 s, 155 s, 185 s (F12) |
| Identity and verify | 3.3 min (reordered, plus the 191 s rerun), 6.8 min (merged) |
| Run-level sessions A/B, four | 8.6 to 13 min each, A/A included |
| Harness, five trials of six engines | 39 min |
| Format 12 sessions, two | 2.3 and 2.7 min |
| Serving memory, three rounds | about 2 min |
| Profiles (1M builds and merges, both sides) | about 2.5 min |
| Format 12 against B builds, two pairs | 9 min |
| Warm-up check | 11 min |

## Gate VM afterwards

All 5M indexes, the three source trees and builds, the probe binaries, the harness engine
directories (`engines/f13g-*`), and the bulky dump outputs (their SHA-256s kept) are removed. The
raw results stay in `~/bench/results/f13g/` (32 MB), and a copy is in this worktree's `.scratch/f13g/`,
hashed in `format13-gate-vm.sha256`. Free space: 12,283,772,928 bytes (11.4 GiB, 91% used),
against 12 GB at the start.

## Not run

- FreeBSD, macOS, and arm64: the gate VM is Linux x86_64 only, and the developer's 1M rows are
  macOS x86_64.
- Parity against the Xapian oracle (`tools/parity`).
- Any reordered pair of merges beyond one.
- A strata anchor in the 20 s warm-up trial.
- The full 1,162-query set in the harness: S′ instead, by D17.
- `STRATA_DELETE_EVERY` indexes: segments with deletes, where the seeds are off, are not covered at
  5M.
