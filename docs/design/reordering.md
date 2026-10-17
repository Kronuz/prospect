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

`docids` gains kind 2, permuted, as `phase-3.md` specifies:

```
[kind u8 = 2][count u64][width u8][ordinal → docid - min_docid, `width` bits each]
[sorted index: per 128 entries of the docid-sorted order, first docid u64]
[docid-sorted ordinals, `ordinal width` bits each]
```

- `docid(ord)` is one bit-packed read. `ordinal_of(docid)` binary-searches the sparse
  index, then the 128 docid-sorted entries of that block (decoding their docids through
  the forward array).
- Validation at open decodes both arrays once: every docid in [min_docid, max_docid]
  (the footer's), the sorted ordinals a permutation of [0, count), and their docids
  strictly increasing. A duplicate docid, a missing ordinal, or an order violation is
  `corrupt_segment("docids")`.
- The footer's `min_docid` and `max_docid` keep their meaning, so the cross-segment
  range checks (`search.hh:252–263`) stay valid.
- The kind records the order (`phase-3.md`: "the segment's docids kind records which"):
  the reader derives `docid_ordered()` at open, true for kinds 0 and 1, and code branches
  on that, never on the kind. No other section changes.
- Size: at 5M documents, 23-bit forward entries and 23-bit sorted ordinals plus the
  index, about 29 MB against today's 9 bytes (contiguous docids): 2.9% of the postings
  section, so it costs more than it saves only if reordering gains less than that; T1
  measures both.

## D2. The writer

`segment_writer` takes documents in output order. In the permuted mode
(`segment_writer::permuted = true`), docids must be distinct and nonzero but need not
increase; the docids encoder writes kind 2. Flushes stay in docid order (D15 of Phase 3:
"flushes sort by docid"); only merges set the mode.

## D3. The merge

`merge_segments` (on `main`; layers step 5 carries the same contract through
`merge_context::order` and `docid_order`, `layers.md` L6) gains an optional permutation:
`order[new ordinal] = (source, old ordinal)`. Without one it writes docid order exactly as
today, byte for byte (the merge fixtures stay pinned). With one, every per-document
column (lengths, norms, stored records, versions, key hashes, docids) follows `order`;
each term's postings are remapped through the inverse map and sorted by new ordinal
before encoding, as the merge's term loop already re-encodes them.

Merging reordered sources reads them through `ordinal_of` and `docid`, never assuming
their ordinal order; the merge computes docid order (for the duplicate check) and the
output order separately.

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
