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

(Filled in as each row completes.)
