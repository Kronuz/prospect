# The query gap after merging: union counts and per-block cost

The Phase 3 "after merging" rows (`docs/gates/phase-3-ratchets.md`) miss 1.0× by 1.33–1.34×.
This record ranks the gap per query and states what was tried on 2026-10-16.

Source: the n1 "after merging" trials (5 trials, harness), per query best over trials.

- Per-query ranking: the gap is concentrated. COUNT union of two common terms ran 5-8x
  Lucene (american south 10.8 ms vs 1.36 ms); COUNT intersections with a rare lead and
  very common others ran about 3x (+time +for +kids 3.0 ms vs 0.91 ms).
- Fix 1 (this record's commit): disjunction_scorer::exact_count counted the two-term intersection by
  next() one document at a time; it now uses count(), the windowed path with P7 probes.
  In-process A/B (ab.sh, 4 rounds x 3 runs, 301 union queries): COUNT 0.932x
  [0.902-0.964], TOP_100_COUNT 1.004x [0.990-1.018], 0 mismatches. Worst queries:
  american south 12.98 -> 2.48 ms, home business 5.73 -> 1.41, interest only
  5.95 -> 1.82, times union 5.52 -> 1.39, tv party 3.68 -> 1.33.
- Rejected: one store per bitmap word in postings_iterator::fill (accumulate in a
  register): intersection COUNT 1.092x [1.066-1.117] worse, TOP_10 0.990, TOP_100_COUNT
  0.983. The word-change branch mispredicts at mid densities.
- Windows vs leapfrog: forcing leapfrog over the 300 intersection queries is 1.026x
  the current chooser; the per-query better of the two would be 0.919x. A linear refit
  of prefer_windows()' constants fits poorly (2x mean error) and does not improve the
  choice (0.995x). At most about 8% is available here.
- Profile of +time +for +kids (COUNT, windows): postings fill 22%, decode_group 14%,
  conjunction fill 10%, block_index_for 6%, advance 6%, or_bitset 5%, load_bitset 5%,
  check_increasing 4%, skip_sized_impacts 3%. About 300 ns per touched block against
  roughly 90 ns for Lucene: the remaining gap is per-block skip-entry decoding and
  validation, a format 13 question (fixed-width level 0, no impacts on the count path).
- In-process numbers (`strata_engine bench`) compare strata builds only; the harness
  adds the client pipe (floor 67 us), so they are not comparable with the baselines.
