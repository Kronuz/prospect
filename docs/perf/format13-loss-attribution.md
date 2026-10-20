# Format 13's remaining query losses, attributed before the freeze

At 5M on the gate VM, strata (main, reordered) loses rows to every baseline
(`format13-gate-vm.md`, row 4b). The lead's ruling of 2026-10-19 (F13-R45): before the freeze,
each loss is attributed, and each format-dependent cause gets its place in format 13. This file
does the attribution, class by class.

## Plan (written before the runs)

- **Classes** (5M, harness, five trials on S′; strata time over the engine's):
  1. Counted unions of more than three terms (`COUNT`): 2.30 times slower than Lucene, 2.77
     than Lucene `-bp`; `TOP_100_COUNT` 1.09 and 1.16.
  2. `TOP_100_COUNT` unions of three or more terms: 1.71 and 1.97 times slower than Tantivy.
  3. intersection_union (one required term, one optional): 1.11 to 1.41 times slower than
     Lucene `-bp` at every command but `COUNT`; 1.08 to 1.24 than Tantivy at `TOP_100`,
     `TOP_1000`, `TOP_100_COUNT`.
  4. Phrases: 1.10 to 1.21 times slower than Tantivy at every command, with FQ3's positional walk
     (the positional deltas are found by walking each group's impact lists).
  5. Against Tantivy also: intersections at `TOP_1000` (1.15) and `TOP_100_COUNT` (1.18),
     negations at `TOP_1000` and two-term negations at `TOP_100_COUNT`, the two-phase-critic.
  6. PISA's 27 comparable rows: unions of two and three terms and intersections at the top-k
     commands, counted intersections of three or more.
- **Hypothesis per class:** none fixed in advance; each class gets its counters and profile first.
- **Procedure.**
  1. A counting build: main with perf/query-counters merged (its hooks ported to main's reader),
     plus counters for the impact-list walks and position blocks (`.scratch/wt-cnt`; not for
     landing). `strata_engine bench` on the 1M reordered index (identical bytes to main's), each
     class's gate queries, both presets where they differ.
  2. A profile per class: a Release build with symbols, `sample` over a looping bench.
  3. Where a mechanism is suspected, a scratch A/B of the format-free change on the 1M index under
     a light hold, one thread, A/A first, so the expected gain is measured, not guessed.
  4. The baselines' code read for how they do the same work (Tantivy 0.26, Lucene 10.4, PISA
     0.8.2; Apache 2.0 and MIT, so nothing ported here needs more than a notice).
  5. FQ3: the positional walk's share of phrase time from the counters and the profile, and a
     scratch build that stores a level-1 positional offset, timed against main on phrases.
- **Classification.** Format-dependent when closing the loss needs bytes, a layout, or a reserved
  place the frozen format must carry; format-free when an algorithm change over format 13's bytes
  closes it. Each row names the place (or "none") and the expected gain with its source.
