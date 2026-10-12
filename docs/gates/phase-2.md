# Phase 2 gate: single-segment engine

Numbers come from the Linux gate VM (QEMU with HVF on an Intel i9-9980HK, 8 vCPUs).
The Lucene 10.4.0 and Tantivy 0.26 query rows are from one harness run; strata's from
later runs of the same harness in the same VM. The host was not idle for every run, so
all rows are provisional until the quiet-host re-run of every engine.

## Results

| Gate | Target | Result | Status |
|---|---|---|---|
| Query classes | Within 1.5× of the faster of Lucene and Tantivy per class (2× blocks) | Every class of every command within 1.5×, raw and with the 155 µs pipe floor subtracted; the worst is `TOP_100_COUNT` intersection at 1.44× raw (1.50× floor-subtracted). Counts equal Tantivy's on all 962 + 200 queries | Pass, provisional |
| Term class | Gains 200 single-term queries stratified by df | `bench/phase2/terms-stratified.txt`: 67 rare (df < 1,000), 67 medium, 66 frequent (df ≥ 100,000), seed 2026; the class is those plus `the` | Pass |
| Xapian floor | Win every class and single-thread indexing | Xapian 2.1.0 is 6–19× slower than Lucene and Tantivy on every command (Phase 0) and indexes at 881 docs/s | Pass |
| Single-thread indexing | ≥ 1.0× Lucene, ≥ 0.8× Tantivy, 1M documents, 2 GiB budget, one segment | 9,092 docs/s: 2.05× Lucene (4,424), 0.87× Tantivy (10,397); peak RSS 1.56 GB | Pass |
| Postings size | Whole postings section ≤ 1.02× Lucene's `.doc` | 1,008.0 MB against 990.6 MB: 1.018× | Pass |
| `termdict` lookup | ≤ 1.0× Lucene's `seekExact` | 0.54× present and absent keys (645 / 686 ns against 1,190 / 1,250 ns) | Pass |
| Relevance parity | Identical match sets and BM25 top-10 within 1e-9 against the oracle | 300 of 300 queries equal in counts and top-10; worst relative error 3.3e-16, indexed through `analysis::term_generator` | Pass |
| Analyzer | Passes every oracle vector before parity | 966 vectors and two per-code-point sweeps over all 1,114,112 code points | Pass |

## Query classes

Mean over each class of every query's fastest of 10 runs, in microseconds including the
pipe; "Raw" and "Floor-subtracted" are strata over the faster engine.

| Command | Class | n | strata µs | Lucene µs | Tantivy µs | Raw | Floor-subtracted |
|---|---|---:|---:|---:|---:|---:|---:|
| `TOP_10` | global | 657 | 1,401 | 1,436 | 1,618 | 0.98× | 0.97× |
| `TOP_10` | term | 201 | 298 | 331 | 257 | 1.16× | 1.40× |
| `TOP_10` | intersection | 300 | 1,188 | 1,303 | 1,279 | 0.93× | 0.92× |
| `TOP_10` | union | 301 | 1,492 | 1,506 | 2,141 | 0.99× | 0.99× |
| `TOP_10` | intersection_union | 40 | 4,333 | 3,619 | 4,106 | 1.20× | 1.21× |
| `TOP_10` | negated | 19 | 521 | 1,126 | 968 | 0.54× | 0.45× |
| `TOP_10` | phrase | 300 | 2,621 | 2,421 | 2,164 | 1.21× | 1.23× |
| `TOP_10` | two-phase-critic | 1 | 19,781 | 52,479 | 31,359 | 0.63× | 0.63× |
| `TOP_100` | global | 657 | 1,884 | 1,873 | 2,045 | 1.01× | 1.01× |
| `TOP_100` | term | 201 | 446 | 482 | 405 | 1.10× | 1.16× |
| `TOP_100` | intersection | 300 | 1,525 | 1,503 | 1,281 | 1.19× | 1.22× |
| `TOP_100` | union | 301 | 2,242 | 2,274 | 3,185 | 0.99× | 0.98× |
| `TOP_100` | intersection_union | 40 | 4,613 | 3,742 | 3,977 | 1.23× | 1.24× |
| `TOP_100` | negated | 19 | 912 | 1,571 | 999 | 0.91× | 0.90× |
| `TOP_100` | phrase | 300 | 2,782 | 2,971 | 2,135 | 1.30× | 1.33× |
| `TOP_100` | two-phase-critic | 1 | 34,866 | 51,505 | 30,949 | 1.13× | 1.13× |
| `TOP_1000` | global | 657 | 2,630 | 3,038 | 3,152 | 0.87× | 0.86× |
| `TOP_1000` | term | 201 | 758 | 829 | 709 | 1.07× | 1.09× |
| `TOP_1000` | intersection | 300 | 1,819 | 1,767 | 1,371 | 1.33× | 1.37× |
| `TOP_1000` | union | 301 | 3,627 | 4,599 | 5,600 | 0.79× | 0.78× |
| `TOP_1000` | intersection_union | 40 | 5,057 | 4,447 | 4,384 | 1.15× | 1.16× |
| `TOP_1000` | negated | 19 | 1,202 | 1,908 | 1,158 | 1.04× | 1.04× |
| `TOP_1000` | phrase | 300 | 2,836 | 3,269 | 2,151 | 1.32× | 1.34× |
| `TOP_1000` | two-phase-critic | 1 | 40,490 | 51,885 | 31,254 | 1.30× | 1.30× |
| `TOP_100_COUNT` | global | 657 | 3,824 | 6,293 | 3,950 | 0.97× | 0.97× |
| `TOP_100_COUNT` | term | 201 | 452 | 1,118 | 747 | 0.60× | 0.50× |
| `TOP_100_COUNT` | intersection | 300 | 1,810 | 1,521 | 1,256 | 1.44× | 1.50× |
| `TOP_100_COUNT` | union | 301 | 6,492 | 12,470 | 7,332 | 0.89× | 0.88× |
| `TOP_100_COUNT` | intersection_union | 40 | 4,701 | 4,259 | 4,063 | 1.16× | 1.16× |
| `TOP_100_COUNT` | negated | 19 | 1,050 | 1,590 | 995 | 1.06× | 1.07× |
| `TOP_100_COUNT` | phrase | 300 | 2,822 | 3,409 | 2,051 | 1.38× | 1.41× |
| `TOP_100_COUNT` | two-phase-critic | 1 | 40,719 | 46,509 | 30,281 | 1.34× | 1.35× |
| `COUNT` | global | 657 | 1,179 | 986 | 1,459 | 1.20× | 1.23× |
| `COUNT` | term | 201 | 199 | 208 | 202 | 0.98× | 0.92× |
| `COUNT` | intersection | 300 | 1,220 | 944 | 911 | 1.34× | 1.41× |
| `COUNT` | union | 301 | 1,373 | 1,182 | 2,480 | 1.16× | 1.19× |
| `COUNT` | intersection_union | 40 | 252 | 316 | 378 | 0.80× | 0.60× |
| `COUNT` | negated | 19 | 634 | 854 | 523 | 1.21× | 1.30× |
| `COUNT` | phrase | 300 | 2,764 | 3,357 | 1,914 | 1.44× | 1.48× |
| `COUNT` | two-phase-critic | 1 | 40,448 | 47,001 | 30,368 | 1.33× | 1.33× |

## Indexing

| Engine | Documents | Threads | Wall time | Docs/s | Peak RSS | Index |
|---|---:|---:|---:|---:|---:|---:|
| Lucene 10.4.0 | 1,000,000 | 1 (1 GB RAM buffer) | 3:46 | 4,424 | 2.26 GB | 620 MB |
| Tantivy 0.26 | 1,000,000 | 1 (2 GB budget) | 1:36 | 10,397 | 1.23 GB | 674 MB |
| strata | 1,000,000 | 1 | 1:50 | 9,092 | 1.56 GB | 632 MB |
| Lucene 10.4.0 | 5,032,104 | 1 | 17:30 | 4,794 | 4.03 GB | 2.79 GB |
| Tantivy 0.26 | 5,032,104 | 1 | 12:50 | 6,537 | 5.09 GB | 3.02 GB |
| strata | 5,032,104 | 1 | 9:53 | 8,490 | 5.91 GB | 2.77 GB |

Every run produced one segment. The 5M rows are Phase 0's single-thread baselines; strata
holds one in-memory segment, so its memory grows with the corpus until Phase 3's
memory budget flushes segments.

Section sizes on the 5M corpus: postings 1,008.0 MB (Lucene `.doc` 990.6 MB), positions
1,631.2 MB (`.pos` 1,714.8 MB), terms 100.6 MB (`.tim` 39.5 MB), norms 5.0 MB, lengths
10.1 MB.

## Robustness

- **Integrity.** Small sections are verified in full at open; terms, postings,
  positions, and stored records are verified per 64 KiB region on first read
  (`residency::verified_regions`): term blocks by `termdict`'s verifier hook, a term's
  postings and positions ranges when they open, a stored frame before it decompresses.
  The corruption test changes random bytes and requires every change outside section
  padding to be detected.
- **Bounded decoding.** Every length and offset is checked against its section; bitset
  blocks are masked to their span and must hold 128 documents; stored frames decompress
  into exactly their recorded size (`compressors`' bounded `decompress_zstd`).
- **Fuzzing.** `fuzz/fuzz_segment.cc` (segments from fuzz input, then corrupted, read in
  full including positions and stored records), plus the libraries' targets: `unicode`
  35M runs, `snowball` 50M, `analysis` 6.2M, `bitpack` with `encoded_size` 27.9M,
  `termdict` 4.2M, all clean.
- **Golden fixture.** `test/fixtures/small.sx` pins the writer's bytes at format 8.

## Changes for the architect

- **Scorer contract:** `max_score(upto)` over `[doc(), upto]` became
  `max_score(from, upto)` from skip data alone. The old contract made conjunction
  windows rescan every block a dense child had not moved past.
- **Competitive pairs** store the whole Pareto frontier (D-format said at most four);
  merging left 30,315 of 32,563 blocks of `the` above the top-10 threshold, against 21.
- **Lucene BM25** scores a one-byte quantized length, as Lucene does (the design said
  exact lengths); Xapian BM25 keeps exact lengths.
- **Format versions 2–8:** whole frontiers; position and count blocks located from skip
  data (as the design specifies); sized skip-data pairs; term byte lengths for region
  verification; dense postings blocks as bitsets; the stored section; norms.
- **Evaluation:** two-phase iteration for phrases; windowed MaxScore for disjunctions;
  lead-block windows for conjunctions; counting from statistics where known
  (document frequency, `|A| + |B| − |A ∩ B|`, required-plus-optional, term minus term)
  and in 4,096-ordinal bitset windows otherwise; Tantivy's 2k-buffer top-k collector.

## Open

- Quiet-host re-runs of every engine, including the Xapian rows.
- The term dictionary is 2.5× Lucene's `.tim`.
- `residency` backends do not yet verify regions as they prefetch; the segment verifies
  on first read.
- Indexing memory grows with the segment until Phase 3's flushes.
