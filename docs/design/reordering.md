# Reordering at merge (Track P1)

A merge may write its output in an order other than docid order, chosen by recursive
graph bisection, so that documents sharing terms sit near each other: postings deltas
shrink and block-max bounds tighten. Phase 3's design allows it (`phase-3.md`, the
document model and the `docids` section; review `phase-3-design.md`, R2); format 12
reserves the permuted `docids` kind, which no code writes yet. This design builds it.

## Why now

The Phase 3 disk row after a full merge (`docs/gates/phase-3-ratchets.md`, "Disk after a
full merge"): strata's postings are 1,007.9 MB, 1.02× keyed Lucene's `.doc` and 1.21×
Lucene with reordering (`-bp`, 833.2 MB); positions 1,631.2 MB, 1.05× `-bp`'s 1,556.9 MB.
Lucene's reordering takes 15.9% off `.doc` and 9.2% off `.pos` on this corpus. Lucene
`-bp` also sets the reference on several query classes (the "after merging" table), so
the query rows are measured against a reordered index while strata's is not.

## D1. The permuted `docids` kind

**An amendment to format 12** (review P1-R1). No reader before P1 accepts kind 2: it
rejected any kind but 0 and 1 as `corrupt_segment("docids")`. The format's version stays
12, on the premise of the Phase 4 review's R20: no index exists outside tests and
benchmark runs. Every Phase 3 gate index is rebuilt by one binary with P1, and no binary
built before P1 reads a directory P1 wrote (the VM's engines are rebuilt first).

The codec is one unit, `include/strata/docids.hh`, for all three kinds; main's
`segment` and `segment_writer` call it, and so will the layers branch's core (P1-R17).

```
contiguous (0) [kind u8][base u64]
monotone (1)   [kind u8][count u64][per 128 ordinals: first docid u64, delta offset u64]
               [LEB128 deltas, each at least 1]
permuted (2)   [kind u8][count u64][docid width u8][ordinal width u8]
               [forward: count entries of docid − min_docid, docid width bits][8 zero bytes]
               [sorted: the ordinals in docid order, ordinal width bits][8 zero bytes]
               [index: per 128 sorted entries, the first one's docid u64]
```

- **The kind follows the order written.** The encoder writes kind 0 or 1 whenever the
  docids increase, and kind 2 only otherwise, so `docid_ordered()` is exact and the
  equal-bound skip survives wherever it can. A merge whose bisection returns the
  identity (any test-sized merge: min_df leaves the graph empty) writes the same bytes as
  today.
- **Widths.** The docid width is `bit_width(max_docid − min_docid)`, 0 to 64; the ordinal
  width is `bit_width(count − 1)`. Both are stored and checked equal to those values.
  Bit arrays are little-endian, entry i at bit i × width. The 8 zero bytes after each
  array let every entry be read with one 8-byte load and at most one more byte, never
  past the section; the reader checks the padding is zero.
- **Reads.** `docid(ord)` is one entry read. `ordinal_of(docid)` binary-searches the
  index, then the block's at most 128 sorted entries, each a sorted read and a forward
  read.
- **Validation at open** (`phase-3.md`'s rule, reviewed in its R9): the count equals the
  footer's before any size is computed from it; count ≥ 2 and 1 ≤ min_docid < max_docid;
  the length is exactly the header, both arrays with their padding, and the index; every
  forward docid lies in [min_docid, max_docid]; the forward docids do not increase (else
  the section should be kind 0 or 1); the sorted ordinals are a permutation of [0, count)
  whose docids strictly increase from min_docid to max_docid; and each index entry equals
  the docid at its position. Each failure is `corrupt_segment("docids")`.
- **Open cost and serving pages** (P1-R2). Validation reads the forward array once in
  order and once at random through the sorted ordinals, with a count-bit scratch bitmap.
  At 5M documents that is about 29 MB of section and 0.6 MB of scratch; its open time and
  the pages it leaves resident are measured on the first reordered 5M index and decide
  between keeping eager validation and validating on first use, as `lengths` does.
- The footer's `min_docid` and `max_docid` keep their meaning, so the cross-segment
  range checks (`search.hh:252–263`) stay valid.
- **Size** at 5M: 23-bit forward entries and 23-bit sorted ordinals plus the index, about
  29 MB. Where its bytes count is P1-R10's.

## D2. The writer

`segment_writer` takes documents in output order. In the permuted mode
(`segment_writer::permuted = true`), docids must be distinct and nonzero but need not
increase; the docids encoder writes kind 2. Flushes stay in docid order (D15 of Phase 3:
"flushes sort by docid"); only merges set the mode, and `begin_document`'s increasing check
stays for flushes. The encoder sorts (docid, ordinal) pairs to check distinctness and build
the sorted array, 16 bytes a document and as much again for the arrays it builds from them
(about 160 MB at 5M); that working memory is charged to the writer's `flush_charge` while it
lives (P1-R3).

## D3. The merge

`merge_segments` (on `main`; layers step 5 carries the same contract through
`merge_context::order` and `docid_order`, `layers.md` L6) gains an optional permutation,
`order[new ordinal] = merge_ref{source, old ordinal}`, which must name every live document
exactly once (`std::invalid_argument` otherwise). Without one it writes docid order exactly
as today, byte for byte (`merge_test`: a merge equals one flush of its live documents).
With one, every per-document column (lengths, norms, stored records, versions, key hashes,
docids) follows `order`; each term's postings are remapped through the plan and sorted by
new ordinal before encoding, as the merge's term loop already does. The writer runs in its
permuted mode, and the docids section's kind follows the order actually written (D1).

- **(a) The plan** (P1-R4). `merge_plan::ordinals[source][old ordinal]` is the new ordinal
  in the order written, so `install_merge`'s carry-over of deletes made during the merge,
  which maps through the plan, removes exactly those documents. `merge_test` checks the plan
  against the order; Phase 3's T2 schedule with permuted merges is T4's.
- **(b) Stored records.** A permuted order never keeps a source frame's documents
  contiguous, so I7's raw frame copy never applies and the stored section is re-encoded.
  `stored()` alone decompresses a record's whole frame. A merge reads each source through
  a one-frame cache (`segment::stored_frame`), so docid order decodes each frame once. Out
  of docid order the merge first copies every source's live records, frame by frame, into
  a record spill (`seg-<id>.rec`, written through the merge's file writes and mapped back,
  8 bytes an ordinal charged for its offsets), then writes them in `order`. Each source
  frame is decoded exactly once either way. T3 states the cost at 5M.
- **(c) A single permuted source keeps its order.** Without `order`, a merge of one source
  whose ordinals are not in docid order writes its live documents in that source's order:
  the policy's rewrites past the deleted-fraction bound and `force_merge_deletes` do not
  bisect again. With several sources and no `order`, the output is docid order. Bisection's
  starting order is docid order (D4).
- **(d) Under the layers refactor**, the core cannot compute `order` itself (bisection reads
  `inverted`'s postings). The layout gains an optional order source, called by the core
  before any plugin's merge; Phase 4's D14 index sort and the RocksDB survey's key order
  use the same hook. `layers.md` L6 records it before step 5.

Merging reordered sources reads them through `docid`, never assuming their ordinal order;
the merge sorts by docid for the duplicate check and writes the output order separately.
`merge_test` checks: a shuffled order's plan and bytes (equal to the permuted writer's
flush of the same documents in the same order), a single permuted source re-merged
unchanged, a permuted source merged back to docid order (equal to the docid-order flush),
and orders that miss or repeat a document refused.

## D4. Recursive graph bisection

The permutation comes from recursive graph bisection over the document-term graph, as
Dhulipala et al. (KDD 2016) define it and Mackenzie et al. (ECIR 2019) reproduce it, written
from the papers. Its presets start from Lucene 10.4's documented `BPIndexReorderer` defaults
(P1-R7).

- **The graph** (P1-R6). The merge's output documents, numbered in docid order, and every
  term whose live document frequency lies in [`min_df`, `max_df` × documents]. Term ids are
  dense, in the merged dictionary's order. df is counted over live postings in the pass that
  builds the forward index, so deleted documents never shape the order.
- **The forward index** (P1-R5, P1-R19) is never held whole in memory. It is a merge
  temporary file written through `IO` (a name in strata's orphan patterns, reserved by
  storage admission, its bytes counted in T3's write amplification): per document, its graph
  terms' ids in increasing order, delta-coded LEB128, and a row-offset array beside them.
  It takes 1 + P passes over the candidate terms (summed source df ≥ `min_df`):
  - **The first pass** counts each candidate's live df, which decides graph membership, and
    each document's degree in the graph.
  - **The ranges** are then cut on those degrees, at most `reorder_memory` / 2 / 9 edges
    each (an edge's 4-byte cell and its worst 5-byte LEB128 encoding), so every range's rows
    fit.
  - **Each of the P passes** reads every graph term's live postings in its range and appends
    them to the rows, which are then written out. A docid-ordered source maps the range to
    an ordinal range, and its iterator is advanced to the range's start. A permuted source
    (a forced merge of a reordered segment with newer ones) maps no range, so each pass reads
    its lists whole, through a (source, ordinal) → output-rank map of 4 bytes a document,
    charged with the working set.
  - **The transposition checks the cancel token** per term, as the merge does.
  At max_df 1.0 the graph has about 336M edges at 5M documents (the review's `forward_size`
  probe), roughly 0.5 GB on disk, so 4 or 5 passes at a 256 MB preset. Bisection reads rows
  through a mapping of that file (page cache, which T3's RSS counts; see T3).
- **The cost** (P1-R6, P1-R18). The papers' log-gap estimate for a term of degree d in a
  part of n documents is d · log2(n / (d + 1)). Its difference when a document moves from
  part 1 (degree d₁, size n₁) to part 2 (d₂, n₂) is computed directly, never as a difference
  of d · L products, whose table error the degree multiplies:
  g = L(n₁) − L(n₂) − D(d₁) + D(d₂ + 1), with D(d) = d · log2(d + 1) − (d − 1) · log2 d
  = log2 d + d · log2(1 + 1/d). All in integers, 2^-16 bits: L(x) = 2^16 · ⌊log2 x⌋ + T[m],
  where m is the 10 bits after x's leading one, left-aligned and zero-padded below 2^10, so L
  is exact below 2^11 and truncates above it (by up to 92 units); D(d) = L(d) + E(d), with E a
  table below 2^11 and the constant round(2^16 / ln 2) above it, which errs by under 24 units.
  T and E are checked-in constants whose hashes a test pins, with no libm at run time, and a
  second test pins the gains of fixed (d₁, d₂, n₁, n₂) against values computed offline. Each
  per-term gain is under 2^23 in magnitude, and a document's sum over at most 2^32 terms fits
  `int64`. A document's move gain sums g over its graph terms; the other direction is
  symmetric. (The review's probe `gain_corpus`: this form's swap decisions agree with the
  papers' real-valued formula in 99.1% of pairs on the corpus sample, against 52.6% for the
  product form.)
- **The step.** For a part [a, b): the left half is its first ⌊(b − a) / 2⌋ documents of the
  current order. Per iteration: count each graph term's degree in both halves (one pass over
  the part's rows), compute every document's gain (a second pass), sort each half by gain
  descending with ties by current position ascending, and swap the i-th pairs while their
  summed gain is positive. At most `iterations` (20) per part, stopping early when no pair
  swaps, which does not change the result. The part recurses into its halves until it holds
  at most `min_partition` (32) documents. The depth cap is gone: 32-document leaves bound the
  depth at 27 for a 2^32-document segment.
- **Threads.** Bisection runs on the merge's own slot of the host executor. With
  `reorder_threads` above 1 (default 1), independent halves run on the executor's workers,
  bounded per node; each half's result depends only on its own documents, so 1 and 8
  threads give the same bytes by construction.
- **Cancellation.** Bisection checks the merge's token and the executor's checkpoint once
  per iteration of each part, so `abandon_merges` is never held behind it.
- **Memory and the fallback** (P1-R5, P1-R20). Bisection's anonymous working set is the
  order and each document's gain and position (20 bytes a document), and two degree counters
  per graph term for each of the largest `reorder_threads` that the preset allows (8),
  whatever the run uses (`reorder::working_set`). The forward index's build holds less: a
  4-byte rank per source document, a 4-byte degree and an 8-byte row offset per document,
  and one range's rows. The fallback compares
  that figure, a function of the document count and the graph's term count alone, with
  `reorder_memory` (256 MB, the reference `-bp` builder's budget): above it the merge writes
  docid order and counts it in a metric. So 1 and 8 threads always make the same choice and
  the same bytes. It is about 100 MB at 5M; a segment of about 12M documents still reorders.
  The working set is charged to the merge's `flush_charge` while bisection runs, and a merge
  without room waits as merges already do (`merge_memory_wait`).
- **Cost, projected** (P1-R6). About 18 levels × at most 20 iterations × 2 passes over
  308M edges, about 2 × 10^11 edge reads, plus 18 × 20 sorts of 5M gains: on the order of
  20–30 minutes on one thread at 5M, before the merge itself. T3 compares the forced merge
  against the `-bp` builder's force-merge phase, which runs bisection with no executor.
- **Presets** (P1-R7; `limits.md` gains a presets row): `min_df` 4,096, `max_df` 1.0 (every
  term), `min_partition` 32, `iterations` 20, `reorder_memory` 256 MB, `reorder_threads`
  1. They bound no input, so the limits rule's "at least the largest" does not apply; a
  measured faster preset replaces one.
- **When** (P1-R7). `index::options::reorder` is `off`, `force_merge` (the default, as the
  reference builder reorders: only at force merge), or `merges` (every merge whose output
  holds at least `reorder_min_docs`, Lucene's suggested 262,144). At 5M every natural merge
  passes 100,000 documents, so reordering natural merges would change T4's indexing cost
  wholesale; the default leaves it unchanged, and the indexing rows decide whether `merges`
  becomes the default. `off` also turns off `force_merge`'s reordering.

## D5. Queries over reordered segments

Ordinal order is no longer docid order inside a reordered segment. The paths that assume it
(P1-R8), each changed to break ties by docid when the segment is not `docid_ordered()`:

1. **`top_collector`** (`search.hh`): `collect` rejects `score <= min_` without a segment;
   `docid()` returns the ordinal without one; `order` compares ordinals within a segment;
   `before_kth` compares ordinals when the k-th is in the current segment; `update_min`
   without `begin()` makes the k-th score the threshold (the equal-bound skip). A reordered
   segment always goes through `begin()`, so ties read docids and the threshold stays just
   below the k-th score, as across segments today.
2. **`top_k`** ties by ordinal, admits only scores above the k-th, and hands the k-th score
   to the scorer. It gains the segment, and over a reordered one breaks ties by docid and
   hands down the score just below the k-th.
3. **Callers:** `searcher.hh`'s one-segment path, `tools/engine_query.hh`'s engine, and
   `search_test`'s "every path keeps the smallest ordinals" (which holds for docid-ordered
   segments only, and gains its reordered counterpart: the smallest docids).
4. **Sort keys and collapse** (Phase 4): `sortkey::may_compete`'s `docid_ordered` argument
   means every docid in the block exceeds the worst's, which a segment's flag alone does not
   give across segments; it is computed from the segment's docid range and order.
   `collapse::collapser`'s default arrival breaks ties in ordinal order; search passes it a
   ranking total over (score, docid).
5. **Docid-order walks** (dump with `skip_to` resume, Bool weighting's docid order) go
   through the sorted ordinals over a reordered segment, one stored-frame decode per
   document; they land at Phase 7's seam, and their cost is stated there.
6. **Counting, windows, and scorers** work on ordinals and are order-agnostic.

**The equal-bound skip** (D5.3 of the first draft; `search.hh`'s `update_min`) is lost in a
reordered segment: blocks whose bound equals the k-th score are decoded. Its cost on the
docid-ordered 5M index, per class and both schemes, is measured before D5's code (P1-R9;
`bench/night_p1.sh`, step 2). The tie rule itself stays: Xapiand's E2E asserts the docid
tie-break exactly.

Measured (night of 2026-10-16, `ab.sh` in-process, both binaries from `aad1770`, the merged
5M index, four rounds of three runs, no mismatches), without the skip against with it:

| Command | Gate queries, BM25 | Terms, BM25 | Gate queries, Xapian's BM25 | Terms, Xapian's BM25 |
|---|---:|---:|---:|---:|
| `TOP_10` | 1.001 [0.999–1.003] | 0.999 [0.993–1.005] | 1.001 [0.999–1.003] | 1.051 [1.036–1.067] |
| `TOP_100` | 1.003 [1.001–1.004] | 0.988 [0.982–0.993] | 1.002 [0.999–1.004] | 1.074 [1.065–1.084] |
| `TOP_1000` | 1.006 [1.005–1.008] | 1.019 [1.013–1.024] | 1.007 [1.004–1.010] | 1.024 [1.018–1.031] |

Accepted: the gate's queries lose at most 0.7%, and single terms under the gate's BM25 at
most 1.9%. Single terms under Xapian's BM25, whose coarser scores tie more, lose up to 7.4%;
that scheme is not a gate row, and the loss is stated where Xapiand reports it.

**What checks it.** `verify` compares ties as sets, so it cannot see a tie broken by
ordinal. The exact checks are `multisegment_test`'s and `engine_query_test`'s R2 case,
extended to permuted segments: exactly equal scores straddling the k-th at k = 1, 10, and
100, both schemes, one segment and many, with deletes and floors. And at scale: the
docid-ordered and the reordered 5M indexes must give every gate query bit-identical top-k
docids and scores and equal counts, at k = 10, 100, and 1,000, both schemes. `tools/parity.cc`
maps hits through `docid()` instead of ordinal + 1, and gains a reordered build through a
reordering merge.

## D6. Merging deletes and live documents

Deletes from `apply` address docids and resolve through `ordinal_of`, which D1 keeps
logarithmic. Deletes made while a merge runs are carried over at install through the merge's
plan (D3 (a)). The live-docs bitmap stays per ordinal.

## Scale (P1-R16)

Per node, at 1,000 indexes: at most W merges run at once, so bisection's anonymous memory
peaks at W × `reorder_memory`; its forward index is a temporary file per running merge, under
storage admission. Its CPU, about 2 × 20 × 18 edge reads per graph edge (roughly 44,000 per
document at this corpus's 61 edges per document), is not bounded by the merge rate limiter's
bytes; with the default preset it runs only at force merge, so natural merge throughput and
merge debt do not change. Each open reordered segment's `docids` section, about 5.8 bytes a
document (29 MB at 5M), is mapped and verified at open.

## Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 Disk (P1-R10) | Every section ≤ 1.0× its smallest counterpart among keyed Lucene, keyed Lucene `-bp`, and Tantivy, and the whole index ≤ 1.0× keyed `-bp`; the permuted `docids` section counted in BENCHMARKS' "docids, versions, `pk_filter`" row against the id field's terms and doc values; the stored row included | `strata_engine merge` with reordering on the 5M index, `strata_engine sections`, against the keyed `-bp` build (`bench/night_p1.sh`, step 3: the one-thread builder with `BP=1`, the reference's bisection settings, so the pair differs only by reordering) |
| T2 Queries (P1-R12) | Per class, the reordered index over the docid-ordered one ≤ 1.03 by the interval's upper bound; a class above it runs again with ten trials and blocks if it stays above. The carried "after merging" rows run on the reordered index and are scored against PLAN's target, ≤ 1.0× the faster engine, with each ratio's movement stated | Five trials, `bench/phase2/gate.py` (which prints the `-bp` column), and the in-process A/B between the two indexes; blocks decoded per query beside time |
| T3 Merge cost (P1-R11) | The forced merge's wall time ≤ 1.0× the `-bp` builder's force-merge phase (from its "Merge" line to exit), each side's threads stated, at the same budget; its peak RSS within the budget plus 10%; write amplification (Phase 3's T3), the forward index's temporary file included, ≤ 1.0× | `/usr/bin/time -v` on Linux (`-l` elsewhere) around `strata_engine merge` and the builder's phases |
| T4 Other axes (P1-R11) | Phase 3's T4 indexing rows with the default preset (1 and 8 threads, keyed, equal memory, five trials) at ≥ 1.0× the faster engine, the indexing peak within the budget plus 10%; serving memory on the reordered index (`bench/serve_mem.py`) ≤ 1.0×; no allocation per match on the tie path, by the allocation-counting build | `bench/index_trials.sh`, `bench/serve_mem.py`, the allocation counter |
| T5 Correctness (P1-R13) | Tests that hold permuted segments, which test-sized merges do not get from bisection (min_df leaves their graph empty): explicit permutations (reversed, seeded random) through D3's `order`, plus bisection with test presets, asserting permuted segments exist. Merges of permuted sources; deletes during a reordering merge; replace, delete, and get by docid and by key, and expected versions, absent docids inside [min, max] included; the digest and verdict sequence equal to a docid-order run; `check()`; the byte oracle (`merge_test`); parity on a reordered index; T1's crash points with permuted merges | `merge_test`, `docids_test`, `index_test`, `lifecycle_test`, `tools/parity.cc` |
| T6 Decoder (P1-R14) | `fuzz_segment` one CPU-hour over its permuted, keyed base at the gate's commit; every validation check reached by a resealed segment-level test as well as `docids_test`'s direct ones; `permuted.sx` pinned | `fuzz/fuzz_segment.cc`, `test/docids_test.cc` |
| T7 Determinism (P1-R15) | The same reordering merge gives the same bytes everywhere: a test compares its output's hash with a pinned constant on Linux x86_64 and arm64, macOS x86_64 and arm64, and FreeBSD (CI's jobs), at `reorder_threads` 1 and 8; zstd is the pinned release (`db0a891`) | A test in CI on every platform |

**T1's projection** (P1-R10). The keyed one-thread `-bp` build (night of 2026-10-16,
`results/keyed-bp-n2.sizes`, 22.5 minutes, 6.7 GB peak) differs from the keyed one-thread
build only by reordering. It takes 15.4% off `.doc` (833.7 MB) and 9.0% off `.pos`
(1,556.1 MB); its stored fields grow from 31.9 to 51.7 MB, its terms are 75.8 MB, and the
whole index is 2,542.5 MB. Today strata is 1.09× that whole. The same gains put strata's
postings at about 852.5 MB, 1.02×: a miss; positions at about 1,485 MB (0.95×); stored at
about 25 MB; the whole index, the 29 MB map included, at about 2,507 MB, 0.986×. If P1
leaves postings above 1.0×, what closes them is format 13's postings change (P4's per-block
containers, P13 for positions), to which PLAN carries the disk rows.

## Risks

- Reordering can grow the stored section: the two Lucene builds store the same ids in 31.9 MB
  in corpus order and 53.1 MB with `-bp`. T1 includes the stored row.
- Delta-coded key-term ordinals (the format-13 dictionary projection) lose their locality
  after reordering, about 14.5 MB at 5M instead of 9.8; that projection is redone with P1.
- Bisection time may dominate a forced merge; T3 bounds it against the `-bp` builder.
- The lost equal-bound skip: measured and accepted (D5).
