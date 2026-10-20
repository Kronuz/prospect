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

Phase times (reordered): the two `dump`s 38 to 40 s in parallel; the no-count dumps 23 to 25 s;
`verify` 131 s; the probe's `verify` 191 s (its first attempt ran beside `verify` and was refused
by the journal lock, so it was rerun alone).

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

### Row 3: merge times (C3, FQ4)

One reordering merge per side, A then B, four reorder threads, strict hold; `/usr/bin/time -v`:

| Merge | A wall | B wall | B/A | A CPU (user + sys) | B CPU | A peak RSS | B peak RSS |
|---|---:|---:|---|---:|---:|---:|---:|
| Reordering, 16 segments to one | 1,063.3 s | 1,054.7 s | 0.992 | 2,705.0 s | 2,683.0 s | 4.76 GiB | 4.85 GiB |

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

### Row 4b: strata against the baselines (harness, run level)

`src/client.py` through `harness.sh`: B's reordered 5M index as `f13g-strata`, the same binary
and index again as `f13g-strata-aa` for the A/A, Lucene 10.4.0, Lucene 10.4.0 `-bp`, Tantivy 0.26,
and PISA 0.8.2, each over its own force-merged 5M index; five trials over S′ with the engine order
rotated, `WARMUP_TIME=5`, `NUM_ITER=5` (D17 item 7's settings), light hold. Each trial is a run;
`bench/quick/engines_ab.py` writes the runs out and `ab.py --aa` scores them, B/A being strata's
time over the engine's (below 1: strata faster). Counts equal Lucene's, Lucene `-bp`'s, and
Tantivy's on every query of every trial; PISA's differ on all 40 intersection_union and all 19
negated queries in every trial, so PISA does not answer those queries as the others do and its
rows for them are not comparable. Trials 160 to 165 s (terms) and 298 to 339 s (queries); 42 min
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
