# Phase 2 gate: single-segment engine

Numbers come from the Linux gate VM: QEMU with HVF on an Intel i9-9980HK host
(`-cpu host`), 8 vCPUs, 15 GB, a virtio disk with `cache=none`, Ubuntu 24.04 (kernel
6.8.0), Ubuntu clang 18.1.3 with `-O2 -march=native` (Release), OpenJDK 21.0.12, and
rustc 1.99.0. In-VM fsync of a 4 KiB append: p50 2.9 ms, p99 4.4 ms. strata ran at
`63a5339`, format 11. The G5 run (`bench/phase2/g5.sh`) took all four engines in one
session, gated on host load (`tools/vm/quiet_trials.sh`, limit 6, with no stop during
the run), with the engine order rotated per trial.

## Results

| Gate | Target | Result | Status |
|---|---|---|---|
| Query classes | Within 1.5× of the faster of Lucene and Tantivy per class; counted classes within 1.5× | Five trials: every class of every command within 1.5×, the 95% interval's upper bound included. The worst is `TOP_1000` intersection at 1.45× [1.44–1.47] floor-subtracted (1.41× raw). Counts equal Tantivy's on all 962 + 200 queries in every trial | Pass |
| Term class | Gains 200 single-term queries stratified by df | `bench/phase2/terms-stratified.txt`: 67 rare (df < 1,000), 67 medium, 66 frequent (df ≥ 100,000), seed 2026; the class is those plus `the` | Pass |
| Xapian floor | Win every class and single-thread indexing | strata wins every class of every command (0.006–0.34× Xapian's time, floor-subtracted) and indexes 8.9× faster | Pass |
| Single-thread indexing | ≥ 1.0× Lucene, ≥ 0.8× Tantivy, 1M documents, one segment | Three trials each, median: 9,660 docs/s, 1.53× Lucene (6,290) and 0.84× Tantivy (11,550) | Pass |
| Postings size | Whole postings section ≤ 1.02× Lucene's `.doc` | Format 11: 1,007.9 MB against 990.6 MB, 1.017× (format 10 was 1.026×) | Pass |
| `termdict` lookup | ≤ 1.0× Lucene's `seekExact` | 0.54× present and absent keys (645 / 686 ns against 1,190 / 1,250 ns) | Pass |
| Relevance parity | Identical match sets and BM25 top-10 within 1e-9 against the oracle | 419 of 419 queries equal in counts and top-10, counted and pruned, `AND_MAYBE`, `AND_NOT`, and `NEAR` included; worst relative error 3.6e-16 | Pass |
| Pruned against exhaustive | `verify` on every gate query, k = 10, 100, 1,000 | 962 + 200 queries on the 5M index, 0 mismatches, at `63a5339` | Pass |
| Analyzer | Passes every oracle vector before parity | 966 vectors and two per-code-point sweeps over all 1,114,112 code points | Pass |

## Query classes

Five trials, each the gate queries plus the stratified term set. Per class, the mean
over queries of each one's fastest of 10 runs (median across trials) and the
geometric mean of strata over the faster of Lucene and Tantivy, raw and with the
pipe floor subtracted: the median across trials with its 95% bootstrap interval. The
pipe floor is 55 µs, the smallest time of any engine and query, from trial 2. No
class has its floor above half its time.

| Command | Class | n | Trials | strata µs | Lucene µs | Tantivy µs | Xapian µs | Raw | Floor-subtracted | Against Xapian |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `TOP_10` | global | 657 | 5 | 1,310 | 1,166 | 1,267 | 10,515 | 1.11× [1.07–1.15] | 1.13× [1.07–1.15]  | 0.132× [0.129–0.145] |
| `TOP_10` | intersection | 300 | 5 | 1,127 | 1,062 | 998 | 8,396 | 1.19× [1.11–1.26] | 1.21× [1.13–1.28]  | 0.169× [0.166–0.190] |
| `TOP_10` | intersection_union | 40 | 5 | 4,005 | 2,930 | 3,297 | 12,570 | 1.36× [1.31–1.40] | 1.37× [1.32–1.41]  | 0.251× [0.247–0.271] |
| `TOP_10` | negated | 19 | 5 | 504 | 848 | 704 | 14,184 | 0.78× [0.71–0.80] | 0.77× [0.69–0.78]  | 0.035× [0.034–0.039] |
| `TOP_10` | phrase | 300 | 5 | 2,236 | 2,028 | 1,770 | 8,431 | 1.33× [1.27–1.34] | 1.35× [1.30–1.37]  | 0.255× [0.251–0.266] |
| `TOP_10` | term | 201 | 5 | 277 | 285 | 248 | 16,603 | 1.12× [1.09–1.13] | 1.15× [1.10–1.16]  | 0.050× [0.049–0.052] |
| `TOP_10` | two-phase-critic | 1 | 5 | 14,682 | 48,922 | 25,571 | 57,823 | 0.58× [0.55–0.58] | 0.58× [0.55–0.58]  | 0.254× [0.248–0.265] |
| `TOP_10` | union | 301 | 5 | 1,374 | 1,232 | 1,697 | 13,798 | 1.19× [1.18–1.25] | 1.21× [1.20–1.27]  | 0.101× [0.100–0.110] |
| `TOP_100` | global | 657 | 5 | 1,736 | 1,611 | 1,614 | 15,107 | 1.18× [1.17–1.19] | 1.19× [1.19–1.20]  | 0.122× [0.121–0.137] |
| `TOP_100` | intersection | 300 | 5 | 1,404 | 1,263 | 1,012 | 8,434 | 1.36× [1.36–1.40] | 1.40× [1.39–1.43]  | 0.198× [0.195–0.222] |
| `TOP_100` | intersection_union | 40 | 5 | 4,228 | 3,029 | 3,280 | 18,565 | 1.39× [1.37–1.46] | 1.40× [1.38–1.48]  | 0.181× [0.178–0.199] |
| `TOP_100` | negated | 19 | 5 | 851 | 1,264 | 718 | 14,226 | 1.18× [1.15–1.21] | 1.19× [1.16–1.23]  | 0.056× [0.055–0.066] |
| `TOP_100` | phrase | 300 | 5 | 2,368 | 2,588 | 1,775 | 7,868 | 1.35× [1.33–1.36] | 1.38× [1.35–1.38]  | 0.318× [0.313–0.340] |
| `TOP_100` | term | 201 | 5 | 433 | 437 | 337 | 16,674 | 1.13× [1.10–1.20] | 1.14× [1.11–1.24]  | 0.074× [0.067–0.091] |
| `TOP_100` | two-phase-critic | 1 | 5 | 27,318 | 48,268 | 25,769 | 91,492 | 1.06× [1.02–1.07] | 1.06× [1.02–1.07]  | 0.299× [0.298–0.302] |
| `TOP_100` | union | 301 | 5 | 2,043 | 2,029 | 2,551 | 23,115 | 1.09× [1.05–1.16] | 1.10× [1.05–1.16]  | 0.075× [0.075–0.084] |
| `TOP_1000` | global | 657 | 5 | 2,373 | 2,563 | 2,496 | 21,133 | 1.10× [1.09–1.11] | 1.11× [1.10–1.12]  | 0.123× [0.123–0.134] |
| `TOP_1000` | intersection | 300 | 5 | 1,657 | 1,474 | 1,063 | 8,643 | 1.41× [1.40–1.44] | 1.45× [1.44–1.47]  | 0.210× [0.210–0.228] |
| `TOP_1000` | intersection_union | 40 | 5 | 4,664 | 3,626 | 3,499 | 27,554 | 1.26× [1.25–1.30] | 1.27× [1.26–1.31]  | 0.135× [0.135–0.148] |
| `TOP_1000` | negated | 19 | 5 | 1,121 | 1,468 | 841 | 14,660 | 1.31× [1.27–1.33] | 1.33× [1.29–1.35]  | 0.071× [0.069–0.081] |
| `TOP_1000` | phrase | 300 | 5 | 2,423 | 2,861 | 1,777 | 7,598 | 1.36× [1.35–1.38] | 1.39× [1.37–1.41]  | 0.339× [0.336–0.356] |
| `TOP_1000` | term | 201 | 5 | 726 | 777 | 664 | 17,032 | 1.01× [1.00–1.03] | 1.01× [1.00–1.03]  | 0.103× [0.101–0.104] |
| `TOP_1000` | two-phase-critic | 1 | 5 | 32,462 | 47,643 | 25,447 | 133,592 | 1.28× [1.24–1.36] | 1.28× [1.24–1.36]  | 0.243× [0.238–0.260] |
| `TOP_1000` | union | 301 | 5 | 3,216 | 3,927 | 4,516 | 35,135 | 0.84× [0.83–0.86] | 0.84× [0.83–0.86]  | 0.074× [0.074–0.081] |
| `TOP_100_COUNT` | global | 657 | 5 | 3,411 | 5,539 | 3,376 | 35,122 | 1.17× [1.15–1.18] | 1.18× [1.16–1.19]  | 0.104× [0.103–0.118] |
| `TOP_100_COUNT` | intersection | 300 | 5 | 1,590 | 1,278 | 1,011 | 8,570 | 1.40× [1.39–1.41] | 1.44× [1.42–1.45]  | 0.198× [0.197–0.228] |
| `TOP_100_COUNT` | intersection_union | 40 | 5 | 4,286 | 3,608 | 3,366 | 33,938 | 1.19× [1.18–1.29] | 1.20× [1.19–1.30]  | 0.100× [0.099–0.110] |
| `TOP_100_COUNT` | negated | 19 | 5 | 998 | 1,263 | 699 | 14,395 | 1.39× [1.36–1.42] | 1.43× [1.40–1.47]  | 0.064× [0.062–0.080] |
| `TOP_100_COUNT` | phrase | 300 | 5 | 2,410 | 2,979 | 1,762 | 8,203 | 1.34× [1.33–1.36] | 1.37× [1.36–1.39]  | 0.332× [0.330–0.374] |
| `TOP_100_COUNT` | term | 201 | 5 | 416 | 971 | 706 | 17,918 | 0.77× [0.73–0.81] | 0.74× [0.70–0.79]  | 0.067× [0.065–0.072] |
| `TOP_100_COUNT` | two-phase-critic | 1 | 5 | 32,639 | 43,562 | 25,402 | 133,236 | 1.27× [1.20–1.30] | 1.27× [1.20–1.30]  | 0.247× [0.241–0.254] |
| `TOP_100_COUNT` | union | 301 | 5 | 5,808 | 11,069 | 6,366 | 66,862 | 0.96× [0.94–0.97] | 0.96× [0.94–0.97]  | 0.057× [0.056–0.064] |
| `COUNT` | global | 657 | 5 | 1,042 | 869 | 1,326 | 35,076 | 1.08× [1.08–1.10] | 1.10× [1.09–1.11]  | 0.046× [0.046–0.046] |
| `COUNT` | intersection | 300 | 5 | 964 | 823 | 832 | 8,551 | 1.28× [1.28–1.31] | 1.32× [1.31–1.35]  | 0.154× [0.154–0.154] |
| `COUNT` | intersection_union | 40 | 5 | 249 | 277 | 353 | 33,941 | 0.90× [0.88–0.92] | 0.87× [0.85–0.90]  | 0.006× [0.006–0.006] |
| `COUNT` | negated | 19 | 5 | 629 | 756 | 496 | 14,382 | 1.31× [1.29–1.32] | 1.35× [1.34–1.37]  | 0.042× [0.042–0.043] |
| `COUNT` | phrase | 300 | 5 | 2,333 | 2,967 | 1,735 | 8,196 | 1.36× [1.35–1.37] | 1.39× [1.38–1.40]  | 0.321× [0.319–0.322] |
| `COUNT` | term | 201 | 5 | 196 | 205 | 200 | 17,968 | 0.98× [0.96–1.02] | 0.97× [0.94–1.03]  | 0.033× [0.032–0.034] |
| `COUNT` | two-phase-critic | 1 | 5 | 31,958 | 45,342 | 26,308 | 133,058 | 1.22× [1.19–1.25] | 1.22× [1.19–1.25]  | 0.242× [0.238–0.245] |
| `COUNT` | union | 301 | 5 | 1,331 | 1,055 | 2,248 | 66,831 | 1.09× [1.09–1.10] | 1.09× [1.08–1.10]  | 0.018× [0.018–0.018] |

Tantivy is the faster of the two on most classes (intersection, phrase, negated),
Lucene on unions.

## Indexing

1M Wikipedia documents, one thread, one segment, from a dropped page cache; three
trials per engine, rotated; GNU time. Lucene at its 2,047 MB per-thread limit,
Tantivy at a 2 GB budget, Xapian at its defaults (then compacted).

| Engine | Wall time (median) | Docs/s | Peak RSS | Index | Segments |
|---|---:|---:|---:|---:|---|
| strata | 1:43.5 | 9,660 | 1.56 GB | 632 MB | 1 |
| Tantivy 0.26 | 1:26.6 | 11,550 | 1.73 GB | 674 MB | 1 (`meta.json`) |
| Lucene 10.4.0 | 2:38.9 | 6,290 | 2.21 GB | 620 MB | 1 flush (`_0.si`) |
| Xapian 2.1.0 | 15:24.0 | 1,080 | 0.52 GB | 3,341 MB | – |

The trials' system times agree within 0.2 s per engine (strata 3.1–3.2 s). An earlier
attempt, discarded whole, ran right after the query trials while the host compressed
the VM's memory: strata's system time rose from 3 s to 67 s with identical page-fault
counts.

5M documents, Phase 0's single-thread baselines: Lucene 17:30 (4,794 docs/s, 4.03 GB),
Tantivy 12:50 (6,537 docs/s, 5.09 GB), strata at format 11 12:53 (6,510 docs/s, 5.89
GB; one in-memory segment until Phase 3's budget flushes). strata's 1M rate does not
hold at 5M: the single in-memory segment's growth is the cause to measure in Phase 3.

## Memory and disk

Recorded for the ratchets (PLAN.md); Phase 3 gates them.

- Serving the 5M index through 2,886 queries (962 each for `TOP_10`, `COUNT`, and
  `TOP_1000`): own (anonymous) memory and the mapped index pages the queries made
  resident.

  | Engine | Anonymous | Mapped index pages | Peak |
  |---|---:|---:|---:|
  | strata | 3 MB | 748 MB | 750 MB |
  | Tantivy 0.26 | 1 MB | 562 MB | 563 MB |
  | Lucene 10.4.0 | 915 MB (JVM) | 638 MB | 1,553 MB |
  | Xapian 2.1.0 | 1 MB | 6 MB (reads through the page cache, which RSS omits) | 7 MB |

  strata's queries make 1.33× Tantivy's index pages resident. Region verification
  reads whole 64 KiB regions on first touch, which is one cause to measure (Track P15).
- 5M index on disk: strata 2.77 GB, Lucene 2.81 GB (0.99×), Tantivy 3.03 GB, Xapian
  15.4 GB. By section against Lucene: postings 1,007.9 MB (`.doc` 990.6 MB, 1.017×),
  positions 1,631.2 MB (`.pos` 1,714.8 MB, 0.95×), terms and term index 101.6 MB (`.tim`,
  `.tip`, `.tmd` 40.2 MB, 2.53×), stored 15.5 MB (`.fdt` and `.fdx` 36.5 MB, 0.42×),
  norms 5.0 MB (`.nvd` 5.0 MB), lengths 10.1 MB (exact lengths, Xapian's semantics; no
  Lucene counterpart).

## Review findings (phase-2-gate.md)

| Item | Resolution |
|---|---|
| G1 reader safety | Postings and positions readers bound every read by the term's verified range. Level 1 is fixed-width (format 10), so no offset is a sum; each group's ranges come from its own entry and the next, ordered and inside their areas. Skip groups' and blocks' last ordinals must increase and stay below the document count; a group's blocks must fill its bytes exactly and its level-0 entries its level-0 bytes; bitset blocks hold exactly 128 set bits with the last at the block's last ordinal and at most 512 bytes. A positions read rejects a count above the segment's maximum, or a first position and count past the stream, without wrapping, before it writes. `decode_group` invalidates its cache before it overwrites entries (C12) |
| G2 term index | Copied out of the mapping at open, after its checksum |
| G3 fuzzing | `fuzz/fuzz_segment.cc` corrupts prebuilt segments (300 to 20,000 documents, several skip groups, bitset and FOR blocks, counts that differ from wdf, positioned wdf-0 terms, stored records past one frame) and recomputes every checksum for seven inputs in eight, then runs term, conjunction, disjunction, exclusion, required-plus-optional, phrase, Xapian-weighted phrase, NEAR, `(a OR b) AND_NOT c`, `(a OR b) AND_MAYBE c`, and counted queries and stored reads, on four bases (one with 64-bit skip offsets): one CPU-hour at format 11 and `63a5339`, 64,634 runs, 8,851 PCs, ASan and UBSan, a 10 s hang timeout, clean |
| G7 skip loop | `skip_blocks` throws `corrupt_segment` when a group skip would not move past the current document; a resealed segment with a lowered level-1 last makes a pruned term query throw instead of loop |
| G8 linear disjunctions | A disjunction of up to four clauses leaving linear mode, when a pruning parent makes a clause non-essential, seeds its heap with the essential clauses where they stand. The brute-force test runs `(a OR b) AND_NOT c` and `(a OR b) AND_MAYBE c`, pruned and exhaustive, and fails without the fix; parity 419 of 419 and `verify` 1,162 queries rerun clean |
| G4 phrase bound | Lucene-mode phrase bounds use only terms whose position counts equal their wdf; a test with a positioned wdf-0 term leading the phrase compares pruned and exhaustive top-k |
| G5 benchmarks | `strata_engine verify`: pruned top-k equals an exhaustive scan for all 1,162 queries at k = 10, 100, 1000 on Wikipedia, and counts agree. Trials of all three engines in one run, engine order rotated, gated on host load (`tools/vm/quiet_trials.sh`), scored by `bench/phase2/gate.py` (geometric means of per-query ratios, each run's measured pipe floor): pending a quiet host |

Also closed: bound sums compared against the threshold allow for summation rounding,
and every remaining bound is summed rather than subtracted (conjunction lead windows,
disjunction windows) while required-plus-optional rounds the required side's minimum
down (C4); `count()`'s window end stops at `no_more_docs` instead of wrapping (C9); the
table of contents rejects duplicate and overlapping sections, stored record
lengths are checked as read, the writer refuses more documents than 32-bit ordinals
hold, `top_k` with k = 0 and `fill` on an unstarted iterator are safe (C9); the scorer
contract states the `fill` window and `max_score` semantics (C8).

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
- **Fuzzing.** `fuzz/fuzz_segment.cc` corrupts prebuilt segments and recomputes their
  checksums so changes reach the decoders, changes the table of contents with its
  checksum kept valid, and runs every scorer type and stored reads. One CPU-hour per
  target at `63a5339`, in parallel, ASan and UBSan, a 10 s hang timeout, RSS limit
  4 GB, zero findings:

  | Target | Runs | PCs | Corpus |
  |---|---:|---:|---:|
  | `fuzz_segment` (format 11) | 64,634 | 8,851 | 1,123 |
  | `snowball` | 76,670,052 | 21,912 | 5,494 |
  | `analysis` | 22,802,463 | 375 | 1,194 |
  | `bitpack` (with `encoded_size`) | 324,447,843 | 296 | 207 |
  | `unicode` | 523,661,330 | 60 | 163 |

  Earlier: `fuzz_segment` one CPU-hour at format 9 (107,932 runs, clean), and
  `termdict` 60 CPU-minutes (4.2M runs, clean) for its verifier hook.
- **Golden fixture.** `test/fixtures/small.sx` pins the writer's bytes at format 11, and
  a file of the previous version is rejected at open.
- **Sanitizers** at `63a5339`: every test clean under ASan with UBSan
  (`-fno-sanitize-recover=undefined`), the concurrency test clean under TSan (Homebrew
  LLVM).

## Changes for the architect

- **Scorer contract:** `max_score(upto)` over `[doc(), upto]` became
  `max_score(from, upto)` from skip data alone. The old contract made conjunction
  windows rescan every block a dense child had not moved past.
- **Competitive pairs** store the whole Pareto frontier (D-format said at most four);
  merging left 30,315 of 32,563 blocks of `the` above the top-10 threshold, against 21.
- **Lucene BM25** scores a one-byte quantized length, as Lucene does (the design said
  exact lengths); Xapian BM25 keeps exact lengths.
- **Format versions 2–11:** whole frontiers; position and count blocks located from skip
  data (as the design specifies); sized skip-data pairs; term byte lengths for region
  verification; dense postings blocks as bitsets; the stored section; norms; the
  segment's maximum position count (9); fixed-width level-1 skip entries, binary-searched
  in place, so opening a term no longer parses every group (10: `the` opened in 70 µs
  before); 32-bit level-1 offsets unless a term needs 64, and 2-byte competitive pairs
  when every wdf is below 256 (11).
- **Reading:** bitset blocks answer `advance` from their words by popcount rank and
  expand to ordinals only when iterated; phrases check existence and stop at the first
  occurrence, decoding a slot's positions only when reached, and compute the frequency
  only for Lucene scoring.
- **Evaluation:** two-phase iteration for phrases; windowed MaxScore for disjunctions;
  lead-block windows for conjunctions; counting from statistics where known
  (document frequency, `|A| + |B| − |A ∩ B|`, required-plus-optional, term minus term)
  and in 4,096-ordinal bitset windows otherwise; Tantivy's 2k-buffer top-k collector.

## Open

- Phase 3's ratchets (PLAN.md) against these numbers: query classes from 1.45× at
  worst to ≤ 1.0× (intersection, phrase, negated, and intersection_union are above
  1.3×); single-thread indexing from 0.84× to ≥ 1.0× Tantivy; serving working set from
  1.33× Tantivy's resident index pages; the term dictionary from 2.53×.
- `termdict` p50 and p99 for present and absent keys and the hash-map ratio: the
  benchmarks now time single lookups (`termdict` bench, `SeekExact.java`); the run is
  pending.
- The term dictionary is 2.5× Lucene's (101.6 MB for `terms` plus `term_index`,
  against 40.2 MB for `.tim`, `.tip`, and `.tmd`; 3,964,752 terms, 54% with one
  posting). Per term (`strata_engine dictstats`): keys 8.52 bytes raw; values 19.16:
  positions offset and length 5.92, postings offset or ordinal 4.12, competitive pairs
  3.99 (Lucene keeps its impacts in `.doc`), postings length or single wdf 1.06, df
  1.03, cf 1.04 (0.54 of it where cf equals df), maximum wdf 1.00, flags 1.00; prefix
  sharing saves 2.31 net of block headers; total 25.37. Offsets follow from the
  previous term's offset plus its length, since terms are written in order. Target:
  PLAN's disk ratchet, ≤ 1.0× at Phase 3 (Track P12).
- `residency` backends do not yet verify regions as they prefetch; the segment verifies
  on first read.
- Indexing memory grows with the segment until Phase 3's flushes.
