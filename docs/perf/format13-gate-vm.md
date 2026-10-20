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
