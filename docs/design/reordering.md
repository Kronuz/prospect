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
  Today each document's `stored()` decompresses its whole frame; the reordering merge
  decodes each source frame once instead, into a record spill indexed by ordinal (charged
  and reserved like the merge's other temporaries), then writes in `order`. Not built yet;
  T3 states its cost at 5M.
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
from the papers:

- **Graph.** Terms with df between `min_df` (default 4,096) and a fraction `max_df` of the
  documents (default 0.5), in the merge's output; a forward list of those term ids per
  document, built from the sources' postings in one pass over the dictionary.
- **Recursion.** Split the documents' range in halves; for up to 20 iterations, compute
  each document's move gain from the log-gap cost of each of its terms' degrees in the
  two halves, sort each half by gain, and swap pairs while the summed gain is positive.
  Recurse to a leaf of 16 documents or a depth of 30.
- **Determinism.** The same inputs give the same permutation on every platform and
  thread count: costs use a fixed integer table for log2 (fixed point, 16 fractional
  bits), ties order by current position, and parallelism splits only independent halves.
  Replicas need equal digests, not equal bytes (the digest is layout-independent, D12 of
  Phase 3), but byte determinism keeps the merge fixtures and `verify` reproducible.
- **Memory.** The forward lists and the per-term degree arrays are charged to the merge's
  `flush_charge` (D17); a merge whose graph would exceed its charge falls back to docid
  order and records why.
- **When.** `index::options::reorder` (default on for merges whose output holds at least
  `reorder_min_docs`, default 100,000) and always for `force_merge`. Small flush-sized
  merges keep docid order, so T4's indexing cost changes only for the large merges.

## D5. Queries over reordered segments

Ordinal order is no longer docid order inside a reordered segment, so:

1. **Ties.** `top_k`'s heap and the cross-segment collector order equal scores by docid.
   Within a segment flagged docid-ordered they compare ordinals, as today; within a
   reordered one they compare `docid(ord)`, read only when scores tie (P6-2's rule).
2. **The one-segment path** (`searcher.hh:187`) takes the tie rule above instead of
   assuming ordinal order.
3. **The equal-bound skip** (`phase-3.md:218–220`) applies only to docid-ordered
   segments, as R2 requires.
4. **Collapse and sort keys** (D8, D9 of Phase 4) tie by docid the same way.
5. **Counting, windows, and scorers** are order-agnostic: they work on ordinals.

The digest, `verify`, and `parity` compare by docid, so they hold unchanged.

## D6. Merging deletes and live documents

Deletes address docids and resolve through `ordinal_of`, which D1 keeps logarithmic. The
live-docs bitmap stays per ordinal.

## Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 Disk | Postings and positions ≤ 1.0× Lucene `-bp`'s `.doc` and `.pos`, and the whole index ≤ 1.0× keyed Lucene, the permuted `docids` included | `strata_engine merge` with reordering on the 5M index, `strata_engine sections` against the baselines' files, as "Disk after a full merge" |
| T2 Queries | The "after merging" rows re-run (five trials, `bench/overnight.sh`'s query step) on the reordered index; no class slower than the docid-ordered index by its interval | `bench/phase2/gate.py`, plus an in-process A/B (`bench/quick/ab.sh`) between the two indexes |
| T3 Merge cost | The forced merge's wall time and peak RSS with and without reordering, against Lucene `-bp`'s builder; reported, and the reordered merge within the budget's charge | `/usr/bin/time -v` around `strata_engine merge` and Lucene's builder |
| T4 Correctness | `verify` (C14's form) and `parity` (1, 4, 16 segments) clean on reordered indexes; the digest equal before and after a reordering merge; T1's crash points with reordering merges in the lifecycle workloads | `lifecycle_test`, `strata_engine verify`, `tools/parity.cc` |
| T5 Decoder | `fuzz_segment` one CPU-hour over the permuted kind; a fixture (`permuted.sx`) pinned; every validation check reached by a resealed unit test | `fuzz/fuzz_segment.cc`, `fixtures_test` |
| T6 Determinism | The same merge gives the same bytes on Linux x86_64, macOS, and FreeBSD, at 1 and 8 threads | A test hashing a reordered merge's output, run in CI on every platform |

## Risks

- The permuted `docids` costs about 29 MB at 5M documents; if the postings gain is small,
  T1's whole-index row can lose. The fallback is a delta-coded forward array within
  128-entry blocks, measured before the gate.
- Bisection time on 5M documents may dominate a forced merge; T3 states it.
- Tie-breaking by docid adds a docid read per tied candidate in reordered segments; T2's
  `TOP_10` term class shows its cost.
