# f13-gate: format 13's confirmation rows on the gate VM

- Task: the lead's confirmation of main `8110f1d` (format 13 with the query items) at 5M on the
  gate VM before the freeze: identity (C1), bytes (D15), merge times (C3), run-level queries
  (T4, against the baselines), and the owed rows (single-term `TOP_100`, peak memory).
- Branch / worktree: `perf/f13-gate` from main `8110f1d`, at
  `/Users/kronuz/code/Kronuz/strata-wt/f13-gate`. Record: `docs/perf/format13-gate-vm.md`, raw
  files' hashes in `docs/perf/format13-gate-vm.sha256`.
- Gate VM layout while running: sources in `~/libs/f13g-{a,b,f12}` (A `026ee10`, B `8110f1d`, F12
  `69a3f8c`), indexes and results in `~/bench/results/f13g/`. Now only the results remain (32 MB).

## State

Done. Every row is measured and recorded. Passing: C1 at 5M on both layouts, with and without a
count; T4's guard (no row loses in four sessions); the owed single-term row against format 12
(1.20 to 1.25 times faster); T10's serving memory (equal). Missing: D15's postings constraint,
9.96 MB over keyed `-bp`'s `.doc`; and indexing, about 10% slower from M20's `seeds_of`, which
misses T5's 1.03 guard against format 12. That miss is writer-side and changes no bytes.

## Done

- Plan, ddaa0c9. Builds on the VM (Release, clang 18): A 58 s, B 64 s, F12 57 s; the review's
  `probe_search` built against A and B. Dry run at 20k: dumps identical, verify clean, the
  harness with all six engines, `bench/quick/engines_ab.py` scoring through `ab.py --aa` (6636fdc).
- Row 1 (C1, FQ1, FQ2): `dump` and the no-count `probe_search dump` identical A against B on both
  layouts, 1,162 queries; `verify` and the probe's `verify` 0 mismatches on B, both layouts. The
  dumps are also identical across layouts (one SHA-256 each).
- Row 2 (D15): B reordered postings 843,636,006 (A 845,553,044; keyed `-bp` `.doc` 833,674,096,
  so 1.2% larger), whole 2,525,610,636 (0.67% under keyed `-bp`); merged postings −0.23% against A
  and F12; Lucene, Tantivy, and PISA beside them.
- Row 3 (C3, FQ4): reordering merges 1,063 s (A) and 1,055 s (B); docid merges 141.8 s (A) and
  155.0 s (B); indexing B/A 1.08 and 1.12, B/F12 1.08 and 1.06. The profile puts M20's
  `seeds_of` at 11% of B's build and merge CPU (98% and 87% of the difference).
- Row 4a: B against A at the run level, four sessions, 31, 22, 9, 10 wins, 0 losses. Leans:
  phrases `TOP_1000` +1.5 to 3.3%, rare terms about 0.2 to 0.5 µs per query.
- Row 4b: the harness on S′, five trials, B reordered against Lucene (98 wins, 2 losses), Lucene
  `-bp` (54, 17), Tantivy (27, 32), and PISA (10 and 27 of its 60 comparable rows). One 20 s
  warm-up trial ran every engine faster, so the JVM ratios may flatter strata by up to about 10%.
- Row 5: merged terms against format 12, 16 and 15 wins, 0 losses (term `TOP_10` 0.83 to 0.84,
  `TOP_100` 0.80 to 0.84); indexing peak RSS 5% under F12's; serving memory 581 MB both.
- Records: 927ae19 and the commits before it on this branch.
- The gate VM was cleaned (indexes, sources, builds, probe binaries, engine dirs, bulky dumps):
  12,283,772,928 bytes free.

## Next

1. For the lead: whether D15's 9.96 MB miss is carried or blocks the freeze.
2. A writer-side `seeds_of` that writes the same pairs at under T5's 1.03 (a performance item of
   its own: the profile and the 5M pairs are its baseline).
3. A strata anchor in a 20 s warm-up session, if row 4b's JVM ratios are to be read at better
   than about 10%.

## Blocked on

Nothing on this branch. The freeze decisions (D15's carry, C2's ruling on FQ3) are the lead's and
the architect's.
