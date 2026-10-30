> Status: CURRENT (2026-10-27) in progress on `phase4/host-collectors`.

# host-collectors: P4, collectors and one snapshot over several indexes

- Design: strata `docs/design/xapiand-on-strata.md`, step P4 and rows G5, G7, G11; census: Xapiand
  `docs/dev/engine-census.md`; `docs/mechanisms/aggregation-scope.md` (aggregations check every match).
- Branch / worktree: `phase4/host-collectors` at `/Users/kronuz/code/Kronuz/prospect-wt/host-collectors`, from main
  `9de01a1`. Oracle side: `strata-oracle` branch `shard-vectors` at
  `/Users/kronuz/code/Kronuz/strata-oracle-wt/shards` (local, not merged; `strata-oracle:25be4a3`).
- How it works: `docs/mechanisms/multi-index-search.md`.
- State: in review (every criterion met; rebased onto main `prospect:7f1d915`, pushed).
- Done criteria (set by the lead; contributors tick, never edit):
  - [x] H1 Identity: a search over several indexes equals a search over one merged index at the interleaved
    docids: `identity()` in `test/shards_test.cc`, 50,400 comparisons over 60 random rounds (1 to 7 shards, 1 to 4
    segments each, 22 empty shards, 30 rounds with deletions), for pruned, key (both directions), descending and
    `dont_care`, k from 0 to 5000: hits equal bit for bit (docid and score) to the merged index and to a brute-force model.
  - [x] H2 Docid interleave matches Xapian's: 3,696 oracle rows (`test/fixtures/xapian-shards`, six configurations
    of 1, 2, 3, 5, 5 with two empty shards, and 7 shards, glass backend): docids, weights, key order, counts, document
    count, average length, term frequencies, last docid, document-length bounds.
  - [x] H3 Property test over random shard splits (`identity()`, `visitor()`, `numbering()`: 20,000 round trips).
  - [x] H4 The exhaustive visitor sees every match exactly once (`visitor()`: 145,089 matches over 40 rounds, with
    deletions, k = 0, 3 and 1000; each docid seen once, shard order non-decreasing, score equals the model's).
  - [x] H5 A mutant per claim, each caught (`test/shards_mutants.py`, 33 mutants: 31 on `shards.hh`, 2 on `reader.hh`).
  - [x] H6 ASan+UBSan and TSan, each in its own build directory with its own dependencies (Homebrew LLVM clang):
    `shards_test` clean under ASan+UBSan at the full 60 rounds (50,400 comparisons) and under TSan at 40 rounds (33,600),
    with four threads searching one snapshot; `segment_test` clean under ASan+UBSan (the reader change). CI's linux-asan-ubsan
    and linux-tsan jobs ran the whole suite green.
  - [x] H7 CI green: run 40629357689 on `prospect:83e8152` (pushed before the rebase onto `prospect:7f1d915`), all 8 jobs success:
    linux-tsan, macos-arm64, linux-x86_64-clang, linux-asan-ubsan, linux-x86_64-gcc, macos-x86_64, freebsd-x86_64, linux-arm64-gcc.
    The rebased head's run is the lead's to watch.
  - [x] H8 `docs/mechanisms/multi-index-search.md`.
  - [x] H9 Smoke A/B of single-index search (screen only, `bench/quick/ab.sh`, main `prospect:9de01a1` against the branch, one
    80,000-document synthetic index, 494 queries, 4 rounds of 3 runs, strict hold): results identical (0 mismatches); B/A of
    the per-query best 0.98 to 1.04 per command (`TOP_10` 0.995 and 1.004, `TOP_100` 1.035 and 0.984, `COUNT` 1.040 and 1.002),
    round spread 0.83 to 1.24: noise, no consistent direction. The single-index path is untouched; the only change it
    meets is two more fields in the reader.
- Done:
  - `prospect:40fd4bf` `include/prospect/shards.hh` (docid map, `multi_snapshot`, pruned merge, exhaustive search with host
    key and visitor, document-length bounds), `reader.hh` exposes `min_length()`/`max_length()`.
  - `prospect:c57c126` `test/shards_test.cc`, the oracle fixtures, `test/shards_mutants.py`.
  - `prospect:bd27c84` lazy-scoring, `dont_care`, threads and empty-segment tests; the mechanism note.
  - `strata-oracle:25be4a3` the `shards` command, `gen-shards.py`, inputs and vectors, README section.
- Doing: nothing.
- Next: code review; then S2 (Xapiand `phase7a/strata-engine-search`) builds on `shards.hh`.
- Blocked on: nothing.
- Findings folded in: none yet (no review).
- Last verified (2026-10-27, rebased head): `shards_test` passes in a Release build (6 s), ASan+UBSan and TSan; all 33 mutants caught (the first run missed 3: an empty
  segment in the bounds, `dont_care`, and the key's early rejection; each got a test).

## Scope: what was built and what was not

| Design item | Built |
|---|---|
| Key-then-relevance collector with a host key | `search_options::key`, `reverse_key`, `order`; `exhaustive_search` (Xapian's order, vectors for `reverse` and both docid orders) |
| Exhaustive visitor | `search_options::visit` / `match`: every live match once, with shard, segment, ordinal, docids and a lazy score; one pass also fills the hits |
| One snapshot over several indexes | `multi_snapshot`: flat segments, global statistics and document frequencies, `last_docid()`, `doc_length_bounds()` |
| Docid map | `interleave_docid` / `split_docid` (checked against 64-bit overflow) |
| Doc-length bounds | reader accessors and `multi_snapshot::doc_length_bounds()`; equal to glass's on the vectors |

Collapse is built since (`collapse.hh`, exact for collapse_max > 1; `docs/mechanisms/collapse.md`).
Not built (and why): thresholds and `get_eset` (census: zero uses; removed from Xapiand); percent (the caller's, from the first hit);
Xapian-style estimates at `check_at_least` 0 (strata counts exactly, Xapian's tier 1); a threshold shared between shards in
the pruned path (an optimization; the lead measures); parallel shards (the snapshot is read-only and is searched from four
threads in the test).

Findings for the lead:

1. Xapian's bounds meet only when matches are fewer than `check_at_least`: at `check_at_least` equal to the number of
   matches (220 of 220) it reports lower 220, estimate 230, upper 255. The test asserts exact bounds only below
   `check_at_least`, and `lower <= count <= upper` always.
2. Statistics count deleted documents (phase-3 D13); Xapian's exclude them. The oracle vectors have no deletions, so
   that difference is not exercised against Xapian; it is exercised against a merged index with the same deletions.
3. The glass backend's document-length bounds are exact (3 and 14 on the vectors); the in-memory backend's are not
   (1 and the total length), so the vectors use glass.
4. The pruned search needs every consumer's weights to come from `multi_snapshot::stats()` and `df()`; a builder that
   reads a single shard's statistics silently ranks differently (the identity test catches it).

## Log

- 2026-10-27: rebased onto main `prospect:7f1d915` (P3 landed; the CMake test list merged cleanly); sanitizers, smoke A/B and CI recorded.

- 2026-10-27: branch and ledger created; oracle `shards` command and vectors; `shards.hh`, tests, mutants, note.
