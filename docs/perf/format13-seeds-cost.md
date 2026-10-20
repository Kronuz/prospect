# M20's seeds at a lower writer cost

M20's writer, `inverted_builder::seeds_of`, is 11.3% of a 1M flushed build's CPU and 11.5% of its
docid merge's on the gate VM, 98% and 87% of the query items' extra CPU, and 5M builds run 1.08 to
1.12 times A's time (main `026ee10`, format 13 before the query items) and docid merges 1.09 times
(`format13-gate-vm.md`, row 3). T5's guard is 1.03. This item makes the writer cheaper while it
writes the same seed pairs, so no segment byte, fixture, or digest moves.

## Plan (written before the runs)

- **Class.** Indexing and docid merges: every term with skip blocks and wdfs (df ≥ 128) pays
  `seeds_of` once per flush and once per merge.
- **Hypothesis.** The cost is per posting, not per term: a counting sort of every posting by wdf
  (three passes and a scatter), then for every posting visited a third gather of its document's
  length (`lengths_[ords[i]]`; the term's whole-list pairs and each block's pairs gather it too)
  and up to three heap tests. The output at each wdf level depends only on the multiset of lengths
  with wdf at least that level, so the order within a level is free: the bulk level (wdf 1) needs
  no sort, and one rejection test against the largest full heap answers all three ks for most
  postings.
- **Procedure.**
  1. Count first: an instrumented build of main (B) records, per flush and merge, the postings
     `seeds_of` sorts and visits and the cycles of its phases (max, sort, visit), so the change
     targets the phase that costs.
  2. One change at a time on this branch, each checked byte for byte: segment files of a 100k
     and a 1M flushed build and their docid and reordering merges, `cmp` against B's.
  3. Timing at 1M on the Mac (light hold, one thread): A/A of B first, then interleaved builds and
     docid merges of A, B, and C (this branch), five trials each, CPU and wall from
     `/usr/bin/time -l`; the ratio's spread is stated with the A/A's.
  4. Then the gate VM at 5M: two alternating build pairs (A against C) and one docid merge pair.
- **Keep or remove.** Kept when the segment bytes are identical at 100k and 1M (digest and
  fixtures unchanged) and C's builds and docid merges are within 1.03 of A's at the run level,
  beyond B's A/A spread; a change that does not move its phase is removed and recorded here.

## Where the time went

Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, Release. Corpus: the gate corpus's first
1M lines (`strata-wt/index-size/.scratch/corpus-1000000.json`). Binaries: A = main `026ee10`, B =
main `06a730b` (the query items), C1 = this branch's `5e7a426`, C2 = `59d9d08`, each built from a
detached worktree of its commit in `.scratch/`.

An instrumented B (rdtsc around the phases of `seeds_of`, `.scratch/wt-p`) over a 1M flushed build
(110.5M postings in terms with skip blocks, 87,393 calls) and its docid merge (118.8M, 43,043):

| Phase | Build cycles | Merge cycles | Per posting (build) |
|---|---:|---:|---:|
| Largest wdf | 0.12 G | 0.11 G | 1.1 |
| Counting sort by wdf (every posting) | 0.56 G | 0.71 G | 5.1 |
| Visit: length gather, three heaps | 6.05 G | 5.88 G | 54.8 |

The stop rule almost never fires: 95% of postings are visited, and 86,967 of 87,393 terms reach
wdf 1. The heaps did 75.5M pushes for 104.8M visits: 29.4M while filling and 46.1M replacements
(4.5M, 17.2M, 24.4M for k = 10, 100, 1,000), because most of the postings are in terms with df
between 1,000 and 10,000, where k = 1,000 keeps nearly every length. The visit is 8.8% of the
build's CPU (28.6 s), and with the sort about 9.7%: the profile's 11% on the gate VM.

## The change

A level's k-th shortest length is a function of the multiset of lengths with wdf at least that
level, so the order inside a level is free and a heap is not needed. The writer now:
1. Splits postings into wdf-1 lengths and (wdf, length) pairs above 1 by branchless compaction
   (no counter is incremented through memory per posting), and counting-sorts only the pairs.
2. Adds each level's lengths to exact counts and counts per 16 lengths (65,536 + 4,097 slots,
   allocated on first use and cleared by the postings that set them).
3. Finds each k's k-th shortest by one upward scan the first time the level holds k postings,
   then moves it down as later levels add postings at or below it (whole 16-length blocks when
   they can go). Lengths from 65,536 on share one count and are kept in a list; when the k-th
   lands there it is selected exactly from that list.
4. Scores, keeps the first strictly best, and stops exactly as before, so the pairs are the
   same bytes.

C2 adds one more change: `finish` gathers each skip-block term's lengths once, for the whole-list
pairs, every block's pairs, and the seeds, where each of the three read `lengths_[ords[i]]`.

**Why this mechanism.** The work per posting becomes a few sequential loads and two counter
increments; the counts' scans cost per level, not per posting, and the k-th only moves one way.
A heap's replacement is a logarithmic sift with mispredicted branches, and in these terms almost
every length is a replacement. A buffer cut by `nth_element` (tried first, `.scratch/wt-q`) gave
the same total: its cuts at every level end cost what the heaps had.

## Microbenchmark (one dumped flush of the 1M build)

`.scratch/mb/bench.cc` replays the first flush's 15,642 skip-block terms (21.9M postings, 253,000
documents) through each variant and compares every term's four pairs with the old writer's;
best of five rounds, cycles from rdtsc:

| Variant | Cycles per posting | Mismatching terms |
|---|---:|---:|
| Old (main): counting sort and three heaps | 57 to 59 | (the reference) |
| Floor: the largest wdf and one length gather, nothing else | 3.4 to 3.6 | |
| Buffer and `nth_element` cuts | about the old | 0 |
| Length-code counts, levels grouped, wdf 1 in place | 27.9 | 0 |
| Counts per code and exact, code moved down | 18.8 | 0 |
| Exact counts and per-16 counts, upward then downward | 19.6 | 0 |
| The same with one branchless split pass (the writer's C1) | 13.8 to 14.4 | 0 |

Per phase of the last (cycles per posting): the split pass 6.8 (it includes the gather), the
counting sort of the pairs 2.1, the counts 3.6, the scans 1.5, clearing 1.6. The old algorithm's
cost by df: 92, 71, 85, 50, 35 cycles per posting for df in [128, 256), [256, 1,000),
[1,000, 10,000), [10,000, 100,000), and above, with 9.1M of the 21.9M postings in the middle band.

In situ (C2's writer with rdtsc around the call, `.scratch/wt-q2`): 1.66 G cycles over the 1M
build's 110.5M postings (15.0 per posting, 2.3% of its CPU) and 1.70 G over the merge's 118.8M
(14.3, 2.7%), against the old writer's 6.5 G and 6.2 G.

## Identity

Segments compared byte for byte (`cmp`) between B and C, with `STRATA_IDENTITY_SEED=1` on both
sides (the merge tool now honors it too, `7fc0054`; without it every segment id differs):

| Corpus | Flushed | Docid merge | Reordering merge (2 threads) |
|---|---|---|---|
| 100k, 8 MB buffer (51 segments) | 51 of 51 identical | identical | identical |
| 1M, 256 MB buffer (6 segments) | 6 of 6 identical (641,964,500 bytes) | identical (merged segment of 616.9 MB) | identical (576.3 MB) |

Both C1 and C2 were checked; the digest and fixtures are untouched (no fixture or `digest.txt`
changes, and `postings_test` passes). `postings_test` gains a case that pins the pairs to the
writer's rule exactly on 6,000 documents with lengths past 65,536, a term whose 100th shortest
is past the cap, and wdfs past 65,536 (the sort path). Mutants of the new code, each built and run
against it: the long list's rank (`k - p` for `k - p - 1`), the downward step's `>=`, the
upward scan's `<`, and the at-or-below count's `<=` all fail it; two survivors are equivalent
(the block skip's `>=` as `>`, which only steps one block in lengths, and a cap of 65,528, which
only sends more lengths to the exact list).

## Reproducible seeds (the lead's decision, 2026-10-20)

The durability review's replay (DM-R51) found 8 of 37.7M seed comparisons flip between x86-64
and arm64: the writer compared floating-point BM25 scores, and arm64 contracts multiply-adds
where x86-64's baseline cannot, and `log` is not correctly rounded across libms. The choice is
now exact (`294fd85`):

- Each preset's score is w / (w + K(len)) times a positive constant (idf, and k1 + 1 for
  `bm25_xapian`), and K is a positive multiple of an integer: `bm25` (k1 1.2, b 0.75, average
  max(1, T / N)) of T + 3·len·N; `bm25_xapian` (k1 1, b 0.5, min_normlen 0.5, average T / N, or
  1 when T is 0) of 2T + max(2·len·N, T). So w1 / (w1 + n1) > w2 / (w2 + n2) exactly when
  w1·n2 > w2·n1, which the writer evaluates in unsigned 128-bit integers (w below 2^32, n below
  2^67). No libm call, no floating point, no contraction; ties keep the higher wdf, as before.
- Safety does not depend on the choice: every stored pair is a dominance pair (at least k
  postings with wdf ≥ w and length ≤ l), and the query side scores it with its own statistics
  and the 2^-40 margin, so no seed exceeds a true k-th score.
- Pruning power: on x86-64 the exact rule picks the floating-point rule's pairs for all 15,642
  terms of a 1M flush and all 43,043 of the 1M docid merge (the microbenchmark's `exact-rule`
  variant against the old writer, 0 mismatches), and 1M flushed segments built by `59d9d08` and
  `294fd85` differ only in bytes 8 to 15 (the version and its prefix CRC): every seed byte and
  every section is the same, so query time cannot move. A run-level A/B would compare identical
  postings and is not run. Cost: 62.1 against 62.3 cycles per posting in that variant, within
  noise; the comparisons are per level, not per posting.
- Pins: `fixtures_test` writes a fixed corpus (6,000 documents, average length exactly 5, 24
  terms) and pins its 52 seed pairs in `test/fixtures/seeds.txt`, so CI's arm64 jobs (Linux and
  macOS) check the same pairs; `postings_test` pins an exact tie (average length 9, wdf 3 at
  length 3 against wdf 2 at length 1, 3/21 each) to the higher wdf, and its exact-rule case
  now uses the rational comparison. A mutant comparing with `>=` fails the tie case.
- `format::pre_freeze` 0x80000008 (C17): fixtures re-pinned, the prefix CRC (`0x6b637f48`) and
  the T7 reordering hash (`0x7e6f1ad2a99f9b80`) re-pinned; `digest.txt` unchanged. ctest 39/39
  (macOS x86_64, Apple clang, Release). CI not waited on.

## 5M on the gate VM

Binaries: A = `026ee10`, C = `0d1b3ad` (C2's writer; the exact comparisons of `294fd85` cost the
same, per the microbenchmark), clang 18, Release, built on the VM. The 5M gate corpus, one thread,
256 MB buffer, no merges, `/usr/bin/time -v`. The host is shared and the VM's time moves with the
host's load, so four sessions ran; the first three were disturbed (another session's builds in the
VM, host load to 100; system time 6 to 35 s per run where an undisturbed run has 4 to 5 s), and the
last gated every step on the host's 1-minute load below 3 (the highest seen during a step 3.4 to
6.4, `.scratch/vm/steps.log`), with the page cache dropped before each merge.

| Session | Build pair (CPU s, user + sys) | C/A | Docid merge pair (CPU s) | C/A |
|---|---|---|---|---|
| 2 (VM load gated, host not) | A 138.6, C 134.5 | 0.970 | C 228.6 first, A 177.3 (C's merge 33.9 s system) | order-confounded |
| 2 | C 135.2, A 140.6 | 0.962 | | |
| Final (host gated) | A 122.7, C 128.2 | 1.045 | A 146.7, C (instrumented) 158.2 | 1.078 (C 12.8 s system) |
| Final | C 126.6, A 117.8 | 1.075 | C 138.0, A 137.8 | **1.002** |

The four build pairs average 1.012 (geometric) with a spread of ±8% per pair, so the 5M run neither
confirms nor refutes 1.03 at the pair level: the VM's noise under a shared host is about the size
of the effect. The cleanest merge pair (same session, page cache dropped, both under the gate) is
1.002. The 1M run level (13 trials) is the evidence for K2; at 5M the robust measure is the in-situ
cycle count, which does not depend on the host's load as much as wall and CPU time do:

| 5M docid merge (565.7M postings in skip-block terms, 103,578 terms) | Cycles | Per posting | Share of the merge's CPU |
|---|---:|---:|---:|
| Old `seeds_of` (wt-p, the disturbed session) | 38.3 G | 67.7 | about 10% (the gate's profile: 11.5%) |
| New `seeds_of` (wt-q2, the final session) | 7.96 G | 14.1 | 2.1% (3.3 s of 158 s) |

So at 5M the seeds take 14 cycles per posting, as at 1M (15), against 68: the 1.09 docid merge and
the 1.08 to 1.12 builds of `format13-gate-vm.md`'s row 3 lose their seeds term, and what is left
of it (about 2%) is offset by the shared length gather.

## Before and after

| Measure | Main (B) | This branch (C) | Source |
|---|---:|---:|---|
| `seeds_of`, cycles per posting, 1M build | 59 | 15.0 | in situ (`wt-p`, `wt-q2`) |
| `seeds_of`, cycles per posting, 5M docid merge | 67.7 | 14.1 | in situ, gate VM |
| `seeds_of`, share of build CPU, 1M | 9.7% | 2.3% | in situ |
| `seeds_of`, share of merge CPU, 1M / 5M | 9.4% / about 10% | 2.7% / 2.1% | in situ |
| 1M build CPU against `026ee10` (13 trials pooled) | 1.103 [1.080–1.127] | 1.002 [0.961–1.045] | Mac, A/A 0.995 [0.950–1.041] |
| 1M docid merge CPU against `026ee10` | 1.085 [1.063–1.107] | 1.005 [0.953–1.059] | Mac, A/A 0.988 [0.951–1.026] |
| 5M build CPU against `026ee10` | 1.08, 1.12 (gate) | 0.96 to 1.08 per pair, mean 1.012 | gate VM, noisy |
| 5M docid merge CPU against `026ee10` | 1.093 (gate) | 1.002 (cleanest pair) | gate VM |
| Peak RSS, 1M build (MiB, mean) | 807 | 776 to 794 (A 767 to 791) | Mac, noisy to ±10% |
| Peak RSS, 5M build / docid merge | 573 MiB / 4.75 GiB (gate) | 571.6 MiB / 4.83 GiB (A 573.4 MiB, 4.74 GiB) | gate VM |
| Segment bytes | | identical (C1, C2); version and prefix CRC only (exact seeds) | `cmp` |

The merge's peak grows by about 100 MB at 5M (2%): the split pass holds a term's wdf-1 lengths and
its (wdf, length) pairs (12 bytes per posting of the largest term, about 60 MB for a 5M-posting
term) beside the gathered lengths. Cost in money is small either way: the old writer's extra
2.4 µs per document indexed and 3.3 µs per document merged (the gate's figures) came to about 3.4
core-hours per billion documents written once and merged three times, roughly $0.15 at $0.045 per
vCPU-hour; this removes most of it. What it buys is T5's indexing ratio.

## Phase times

| Phase | Wall |
|---|---|
| Reading, plans | 20:26 to 20:28 |
| Builds A, B | 5 min |
| Instrumented profile, 1M build and merge | 2 runs, about 1 min each (waits for holds) |
| Microbenchmark iterations (seven variants) | about 40 min |
| C1, C2 builds and tests | about 10 min |
| Identity 100k and 1M (B and C, three layouts) | 1 min and 8 min (C2 4 min) |
| 1M timing, two sessions (one aborted) | 24 min and 25 min |
| Gate VM: sync and builds | 4 s and 3 min |
| Gate VM: four sessions and the in-situ run | 21, 17, 14, 17 min, and 98 min gated (52 min of it waiting for the host) |
| Reproducible seeds: code, re-pin, ctest | about 25 min (ctest 121 s) |

The gate VM has 11,088,744,448 bytes free afterwards (12,086,738,944 before; this work's sources,
builds, and indexes are removed, its 320 KB of logs kept in `~/bench/results/f13p/` and copied to
`.scratch/vm/results/`; the rest of the difference is another session's files).
