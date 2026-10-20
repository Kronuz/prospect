# f13-gate: format 13's confirmation rows on the gate VM

- Task: the lead's confirmation of main `8110f1d` (format 13 with the query items) at 5M on the
  gate VM before the freeze: identity (C1), bytes (D15), merge times (C3), run-level queries
  (T4, against the baselines), and the owed rows (single-term `TOP_100`, peak memory).
- Branch / worktree: `perf/f13-gate` from main `8110f1d`, at
  `/Users/kronuz/code/Kronuz/strata-wt/f13-gate`. Record: `docs/perf/format13-gate-vm.md`.
- Gate VM layout: sources in `~/libs/f13g-{a,b,f12}` (A `026ee10`, B `8110f1d`, F12 `69a3f8c`),
  results and indexes in `~/bench/results/f13g/`.

## State

Reordered layout done (rows 1 to 4); merged layout in progress: B merged and F12 merged built, row 5's format-12 sessions running.

## Done

- Plan, ddaa0c9. Builds on the VM (Release, clang 18): A 58 s, B 64 s, F12 57 s; the review's
  `probe_search` built against A and B. Dry run at 20k: dumps identical, verify clean, the
  harness with all six engines, `bench/quick/engines_ab.py` scoring through `ab.py --aa`.
- 5M flushed indexes (16 segments, 256 MB buffer): A 132.6 s, 572 MiB peak; B 143.7 s, 573 MiB.
- A's reordering merge: 1,063 s, 4.76 GiB peak RSS; postings 845,553,044 bytes.
- B's reordering merge: 1,055 s, 4.85 GiB; postings 843,636,006 (9.96 MB over keyed `-bp`'s
  `.doc`), whole file 2,525,610,636 (0.67% under keyed `-bp`'s).
- Identity, reordered: dumps identical with and without a count, verify and the probe's verify 0
  mismatches of 1,162.
- Run-level A/B, reordered: 31 wins / 0 losses (`bm25`), 22 / 0 (`bm25_xapian`).
- Harness, B reordered against Lucene, Lucene `-bp`, Tantivy, PISA: five trials of S′, scored at
  the run level (`docs/perf/format13-gate-vm.md`, row 4b). Reordered indexes then removed.
- F12 (69a3f8c) 5M index 218 s (cold corpus, sys 74 s), 605 MiB peak; docid merge 185 s,
  4.79 GiB; its merged segment is not byte-identical to the engine's older format-12 index
  (2,500 bytes apart, postings equal in size), so row 5 uses this fresh one.
- B merged: index 143.5 s (warm corpus), 574 MiB; docid merge 155 s, 4.75 GiB; postings
  1,005,615,335, whole 2,755,978,368.

## Next

1. Row 5: format 12 against B on the merged index (`TOP_10`, `TOP_100`), serving memory.
2. A merged (after removing F12's index for space); identity and A/B sessions on merged.
3. A warm-up check of the harness (20 s against D17's 5 s) for the JVM baselines.

## Blocked on

Nothing.
