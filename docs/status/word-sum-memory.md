# perf/word-sum-memory ledger

## State
CR3 (`docs/reviews/count-union-bits.md`) closed: `postings_iterator` no longer holds the scatter's
1 KB of zeroed sums. `scatter` is now static and keeps them in a 25-slot stack array (a span under
768 bits touches at most 25 32-bit words), zeroed per call. Intel Mac measurements only.

## Done
- `include/prospect/reader.hh`: `word_sum_[128]` removed; `scatter` sums into a local `sums[25]`
  indexed from the first word, so no clearing pass either. The `fill_test_access` friend is gone
  (CR4's scratch check has nothing left to check).
- `test/fill_test.cc`: `sizeof(postings_iterator) < 3600` check; `test/fill_mutants.py`: scatter
  mutants rewritten for the new body, plus one that puts the array back in the iterator.
- `docs/perf/count-union-bits.md` memory notes updated.

## Evidence (Release, Intel i9-9980HK, base = origin/main fc5dce7)
- Bytes per iterator: `sizeof` 4,424 -> 3,400; malloc block 4,608 -> 3,584.
- 1,000 open indexes (clones of 100k merged), 1,000 queries in flight (994 scorers built and
  advanced once), then the 962-query mix x3 as COUNT and TOP_10 (`.scratch/probe.cc`, three runs
  each): heap held by the scorers 18.5 KB -> 16.0 KB each (-2.5 MB per 1,000 in flight); process
  RSS during the flight +71.3 MB -> +69.0 MB; peak RSS 711.6 -> 708.2/709.6 MB (-2 to -3 MB,
  0.3%: RSS is dominated by the mapped index pages the 1,000 indexes touch).
- `dump` of the 962 queries, counted and `PROSPECT_DUMP_COUNT=0`, byte-identical to main on 100k
  merged, 100k flushed (4 segments), 100k reordered, 1M reordered (5,772 lines each); `verify`
  962 queries, 0 mismatches on the same four.
- Time, 1M reordered, `quiet strict`, interleaved, main twice (A/A) against this build twice:
  962 queries, 12 rounds: `COUNT` B/A 0.998 [0.998, 0.999] and 0.998 against A/A 1.003;
  `TOP_10` 1.004 and 1.001 against A/A 1.001; first run (6 rounds) `COUNT` 1.008, `TOP_10` 1.002,
  `TOP_100` 1.002 against A/A 1.004, 1.003, 1.004. The 30 count-union gate queries (14 rounds):
  1.009 and 1.004 against A/A 1.008. Neutral: all inside the A/A band, buckets by query length
  also flat (1.001 to 1.010). Sums 178.1 -> 178.3 ms (`COUNT`).
- Revert check: with the sums put back in the iterator (mutant "the sums go back into the
  iterator") `fill_test` fails at the sizeof check; the 14 other fill mutants are still caught
  (15 of 15, `.scratch/mutants.log`).
- ASan+UBSan (Homebrew LLVM, `-O1`): all 39 ctest suites pass, no report.

## Next
- Other per-iterator weight: `buf_ords_`, `bits_`, `buf_wdfs_` are 512 bytes each of the 3.4 KB.
- Linux, FreeBSD, arm64 timings (lead's hosts).

## Blocked on
Nothing.
