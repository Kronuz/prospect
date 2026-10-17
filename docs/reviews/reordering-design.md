# strata review: reordering at merge (Track P1) design

- Date: 2026-10-16.
- Reviewed: `docs/design/reordering.md` at `880e87a` (D1–D6, T1–T6, Risks). The code it
  changes at main `8a369e3`, which is `880e87a` plus a reader change to region CRCs and
  the key filter; `c96b271` after it changes only PLAN, BENCHMARKS, and bench scripts.
  Branch `phase4/layers-refactor` at `8e8e420` (steps 0–4b), read, not built.
- Checked against:
  - PLAN at `c96b271`: Goals 1, 2, and 5; Ratchets (Baselines, Reference, Passing,
    Carried rows, the Disk and Memory rows, and "every technique reports its effect on
    all four axes"); Every gate; Phase 3's scope and gate; Track P (P1, P20, P21, the
    platform rule, and the new stretch goal).
  - `docs/design/phase-3.md` (Conventions, format 12's `docids` kinds, Statistics and
    ranking, Merges with I7, the memory budget as built, Scale, the digest), my Phase 3
    design review (R2, R9) and its re-check.
  - `docs/design/layers.md` (L6, L14, L15) and my review of it (condition 1, R4, R8);
    my Phase 4 review (R4, R5, R9, R15, R20); my P6 review (R1, R2, R6).
  - `docs/gates/phase-3-ratchets.md` ("Disk after a full merge", "Serving memory", the
    "after merging" table, "Gate status"); BENCHMARKS (rule 1, the counterpart table,
    Tiers); `docs/design/limits.md`.
  - Lucene 10.4.0's published javadoc for `BPIndexReorderer` and
    `BPReorderingMergePolicy`, and the harness's `lucene-10.4.0-bp/BuildIndex.java` on
    search-benchmark-game master (MIT). Dhulipala et al. (KDD 2016) and Mackenzie et al.
    (ECIR 2019) as the design cites them. No Lucene source was read.
- Checked by:
  - Building main at `8a369e3` (`cmake -S . -B .scratch/review-p1/build
    -DCMAKE_BUILD_TYPE=RelWithDebInfo`, Apple clang 17.0.0, macOS x86_64, `-j4`): no
    warnings. `ctest -j4`: 29 of 29 passed in 91.7 s.
  - Probes in `.scratch/review-p1/probes/`:
    - `kind2_rejected.cc`: a format-12 segment whose `docids` kind byte is set to 2 and
      every checksum resealed.
    - `graph_size.py`: D4's graph on the corpus's first 10,000 documents
      (`.scratch/corpus10k.json`), extrapolated to 5,032,102.
    - `tie_skip.py`: block-max top-k on the same sample, with and without the
      equal-bound skip, in docid order and in a random order.
  - Not run: benchmarks of any tier, the VMs, fuzzing, sanitizer builds, Linux, FreeBSD,
    and arm64 builds, and the layers branch. The 5M corpus and indexes are not on this
    host, and the n1 raw results are not either (`.scratch/quiet-n1.out` is empty), so
    the engine that sets each row's reference was inferred from ratios.

## Verdict

**APPROVE WITH CONDITIONS: the permuted kind, the merge's permutation, and the docid tie
rule are the right shape; D4's memory model and fallback are not, D5 misses code paths
and understates its cost, and T1 measures against the wrong reference.**

What holds, and reproduces:

- One permuted kind recorded per segment, `docid_ordered()` derived at open, and no
  other section changed (D1). Flushes keep docid order and only merges permute (D2).
- The merge already reads every source through `docid()` and sorts by docid
  (`merge.hh:79-88`), and its term loop already sorts each term's postings by new
  ordinal (`merge.hh:158`). A permutation adds a remap, not a new algorithm (D3).
- The cross-segment collector compares docid ranges and reads docids only on ties
  (`search.hh:245-264`). Its cross-segment threshold is already independent of order
  (`search.hh:281`).
- The digest is order-independent (`index.hh:509-578`). Apply's docid-addressed
  operations resolve through `ordinal_of` (`index.hh:1337-1344`).
- `docid(ord)` becomes one read, where the monotone kind decodes up to 127 LEB128 deltas
  (`reader.hh:153-161`). That helps `lookup_key`, the digest, and collection on merged
  segments with gaps.
- Integer costs with ties by position are the right basis for bisection that gives the
  same bytes on every platform (D4).
- P1 is Phase 3 work and belongs on main before the layers refactor lands (P1-R1,
  P1-R17).

What blocks, and what it blocks:

- P1-R5 to P1-R7 (D4's memory, its specification, its presets) block the bisection code.
- P1-R10 to P1-R16 (the rows) block the gate run.
- P1-R4 (a) (carry-over) and P1-R8 (the tie paths) are correctness. The code that needs
  them can start now with them.

A revision check covers P1-R1 to P1-R17 before the bisection code starts. The work that
can start now is in the last table.

## Rulings on D1–D6

1. **D1, the permuted kind: sound with changes** (P1-R1, P1-R2).
   - The layout is near the floor for its job. Two arrays of ⌈log2⌉ bits per document
     give a constant-time `docid(ord)` and a logarithmic `ordinal_of`. A succinct
     inverse (cycle shortcuts every t entries) would save about 14 MB at 5M, for up to t
     dependent reads per `ordinal_of` on the delete and get path; it is not worth it.
   - Missing: the format-12 amendment (no reader accepts kind 2 today), the encoding's
     widths and bounds, validation of the sparse index itself, and the open-time cost
     and the pages it keeps resident.
2. **D2, the writer: sound with changes** (P1-R1's kind rule, P1-R3).
3. **D3, the merge: sound with changes** (P1-R4). The plan's permuted map, the stored
   path, singleton merges of permuted sources, and where `order` comes from under L6
   are unstated.
4. **D4, bisection: wrong as specified** (P1-R5), not yet implementable
   deterministically (P1-R6), and its presets unmeasured (P1-R7).
   - Bisection itself stays: PLAN names it.
   - The alternative to its memory model is a forward index that is not held whole in
     memory, as Lucene's documented O(maxDoc + numTerms × numThreads) implies; the
     reference builder runs it with a 256 MB budget. The fallback becomes a
     deterministic preset, and the presets start from Lucene's documented values until a
     measurement moves them.
5. **D5, queries: sound in its rules, incomplete against the code** (P1-R8), **and its
   cost understated** (P1-R9). The rules are Phase 3's R2 and the P6 review's R1.
6. **D6, deletes: sound with changes** (P1-R4 (a)). It is right for apply's deletes.
   Carry-over at install goes through the merge plan, not through `ordinal_of`.

Placement: P1 is Phase 3 work in format 12, amended in place (P1-R1), and it lands on
main now, ahead of layers step 5 (P1-R17).

## Required changes

### Format and the writer

1. **P1-R1. Format 12 is amended, not reserved** (`reordering.md:6-7`; PLAN, Phase 3
   scope; `phase-3.md:85-91`).
   - **Evidence.** `format.hh:78-80` defines kinds 0 and 1 only, and `validate_docids`
     rejects any other kind (`reader.hh:403`). Probe `kind2_rejected`: kind 2 and kind 3
     each fail as `corrupt_segment: docids`; kind 1, the segment's own, opens.
   - **Why it matters.** Every binary built before P1 reads a P1 segment as corrupt: an
     open of its index fails closed after one retry (`index.hh:1997-2012`), and `heal()`
     reports the segment damaged (`index.hh:450-476`). "Format 12 reserves the permuted
     kind" is true of `phase-3.md`'s text and of no reader.
   - **Ruling.** It is Phase 3 work in format 12. PLAN's Phase 3 scope names this kind
     for P1, and the disk row it serves cannot be carried: PLAN's carried rows are timed
     rows (`PLAN.md:58-63`). Amending format 12 in place, with its version unchanged, is
     acceptable on my Phase 4 review's R20 premise: no index exists outside tests and
     benchmark runs.
   - **Required in the design:**
     - the amendment stated as one, with that premise;
     - every Phase 3 gate index rebuilt by one binary that has P1, and no binary built
       before P1 (the VM's engines, `strata-merged`) reading a directory that P1 wrote;
     - a `permuted.sx` fixture, and a test that kinds 3 and up fail as corruption;
     - **the kind follows the order actually written.** `encode_docids` writes kind 0 or
       1 whenever the output's docids increase, and kind 2 only otherwise. Then
       `docid_ordered()` is exact, and the equal-bound skip survives wherever it can.
       Merges of test-sized inputs also stay byte-identical, since min_df 4,096 leaves
       their graph empty and bisection returns the identity. That keeps `merge_test`'s
       property (`merge_test.cc:1-2`: a merge equals one flush of its live documents in
       docid order) with reordering on.

2. **P1-R2. D1's encoding and its open-time cost** (`reordering.md:18-43`).
   - **Widths.** `docid − min_docid` is 64 bits, so widths run 0–64. The design names
     the layout and its one-entry read, either of:
     - `bitpack`'s 128-value blocks, which pack widths up to 64 since the pinned
       `b2de971` (low and high halves through the 32-bit packers), read by lane
       extraction as `segment::length` does (`reader.hh:282-291`), twice past 32 bits;
     - a plain little-endian bit array read by words.
     Not `version()`'s loop of one bit at a time (`reader.hh:184-193`).
   - **The ordinal width** is derived from the count (`bit_width(count − 1)`), or stored
     and checked equal to it.
   - **Bounds.** With a plain bit array, the sorted ordinals end the section, so an
     8-byte load at its last entry runs past it. Either the section carries zero padding
     that the reader validates, or the reader splits the load. Whole blocks avoid it.
   - **Validation**, which `phase-3.md:89-91` requires and my Phase 3 re-check (R9)
     approved:
     - `count` equals the footer's document count before any size is computed from it;
     - the section's length is exactly the header, both arrays, and the index;
     - `min_docid` is at least 1;
     - the first sorted docid equals `min_docid` and the last `max_docid`, as the
       monotone check does (`reader.hh:413, 421`);
     - each sparse-index entry equals the docid at its position. D1 lists no check of
       the index itself, and a wrong entry misroutes `ordinal_of`.
   - **Cost at open.** `docids` is checksummed whole at open (`reader.hh:79`), about
     29 MB at 5M. The cross-check is then one dependent random read into the 14.5 MB
     forward array per document, 5,032,102 of them. Those pages stay resident.
     - Serving memory is a miss today: 1.05× Tantivy in the gate report, about 1.03×
       after `8a369e3` took anonymous memory from 15.7 to 7.5 MB. A further 29 MB of
       mapped pages is +5% of Tantivy's 563 MB.
     - The design states the measured open time of a 5M permuted segment and its pages,
       and chooses one of:
       - eager validation, as specified now;
       - a sequential cross-check: store the docid-sorted docids (about 2–3 bits per
         document as Elias–Fano when dense), and compare a multiset hash of (ordinal,
         docid) pairs taken in both orders. That is O(n) sequential reads;
       - first-use validation like `lengths` (`reader.hh:283`), with `docid(ord)`
         bounds-checked until then.
   - **The Risks fallback** (a delta-coded forward array within 128-entry blocks,
     `reordering.md:125-127`) assumes docid locality, which bisection removes. The
     design drops it or measures it before relying on it.
   - Not required: when `max_docid − min_docid + 1` equals the count, the sorted
     ordinals are the inverse permutation indexed by `docid − min_docid`. `ordinal_of`
     becomes one read, and the sparse index is not needed.

3. **P1-R3. D2's writer memory** (`reordering.md:45-50`). Checking distinctness and
   building the docid-sorted index sort (docid, ordinal) pairs at flush, about 80 MB at
   5M. The sort is charged to `flush_charge`, as the other per-document columns are
   (`writer.hh:551-558`). `begin_document`'s increasing check (`writer.hh:532`) stays
   for flushes.

### The merge

4. **P1-R4. D3 and D6: the plan, the stored path, singleton merges, and L6's order
   source.**
   - **(a) Carry-over.**
     - `install_merge` carries the deletes made during a merge through
       `job.plan.ordinals` (`index.hh:1170-1196`, the map at 1175), not through
       `ordinal_of`. D6's sentence (`reordering.md:109-110`) holds for apply's deletes
       and says nothing of install.
     - `merge_segments` returns `plan.ordinals` as the inverse of `order`, as it returns
       the docid-order map today (`merge.hh:99`). The design states it.
     - **Check:** a delete made while a reordering merge runs removes exactly that
       document once installed. That is Phase 3's T2 schedule ("merges losing sources'
       documents while they run") with every merge permuted.
   - **(b) Stored records.**
     - The merge reads `seg.stored(d.ord)` per document (`merge.hh:99`), and
       `segment::stored` decompresses the record's whole frame on every call
       (`reader.hh:220-260`). Docid order pays one frame decode per document today (my
       layers review, finding 1); a permutation pays the same.
     - I7's raw copy never applies under a permutation: a source frame's documents are
       never contiguous in the output. So every reordering merge re-encodes the stored
       section.
     - The design states the stored path: decode each source frame once, for example
       into a record spill indexed by ordinal (charged and reserved), then write in
       `order`. T3 states its cost at 5M.
     - Reordering can also grow the section. The gate report's two Lucene builds store
       the same ids: 31.9 MB in corpus order (keyed, one thread) against 53.1 MB with
       `-bp`. That is consistent with the ids' increasing `curid`s losing their order.
       T1's projection includes strata's stored row.
   - **(c) Singleton merges.** A merge whose single source is permuted keeps that
     order: the policy's rewrites past the deleted-fraction bound and
     `force_merge_deletes` do not bisect again to drop deletes. The design also states
     bisection's initial order when sources are permuted: docid order, or the sources'
     orders concatenated.
   - **(d) Where `order` comes from under L6.**
     - The core computes `order` (`layers.md:359-361`), but it cannot run bisection,
       which reads `inverted`'s postings.
     - Phase 4's D14 (an index sort, from `values`) and the RocksDB survey's K5 (key
       order, from `keys`) need the same hook. My RocksDB-survey ruling calls it one
       mechanism with two sort keys.
     - The design names the hook, for example an optional order source per layout that
       the core calls before every plugin's merge. `layers.md` L6 records it before
       step 5.

### Bisection

5. **P1-R5. D4's memory model is wrong at the target size** (`reordering.md:84-86`).
   - **Evidence.** Probe `graph_size` on the corpus's first 10,000 documents:
     - 79.2 postings per document, and 61.3 graph edges (77% of postings), with min_df
       scaled from 4,096 at 5,032,102 documents and max_df 0.5;
     - linearly extrapolated, about 308M edges at 5M, 1.15 GiB as 32-bit term ids;
     - the sample's documents run short: strata's 1,007.9 MB of postings would be 2.53
       bytes per extrapolated posting. The real graph is likely larger.
   - **Against what.**
     - The gate's indexing budget is calibrated at 1,039 MB (gate report, Gate status).
     - Lucene documents bisection's memory as O(maxDoc + numTerms × numThreads), and
       `reorder` and `computeDocMap` take a temporary `Directory` (10.4 javadoc).
     - The reference builder sets `setRAMBudgetMB(256)`.
     - An in-memory forward index is 4–8× that at 5M, and it grows without bound
       toward a 2^32-document segment.
   - **The fallback has no threshold.** "A merge whose graph would exceed its charge"
     cannot be evaluated, because a merge's `flush_charge` draws "past the capacity if
     need be" (`phase-3.md:344`, `budget.hh:42`). Either the fallback never fires, or it
     fires on the budget's momentary pressure. Then the bytes depend on timing, and T6
     cannot pin them.
   - **Required.**
     - The forward index is not held whole in memory. It is written once, document
       major, as a merge temporary through `IO`: a name in strata's orphan patterns,
       reserved by storage admission, its bytes counted in T3's write amplification. It
       is then streamed per level. A compressed in-memory form is the alternative, with
       its bytes at 5M stated and charged.
     - The in-memory working set is stated at 5M and at the segment cap.
     - The fallback is a deterministic function of the merge's inputs and options: a
       `reorder_memory` preset against the graph's computed size, never the budget's
       state. A merge with room waits as merges already do (`merge_memory_wait`). Each
       fallback is counted in a metric.

6. **P1-R6. D4 specified enough to implement deterministically** (`reordering.md:72-83`).
   - **The cost.** The design writes out the one it implements. For example, the
     papers' estimate per term and half is d·log2(n/(d + 1)), and a document's move gain
     is the cost before the move minus the cost after it, summed over its graph terms.
   - **Fixed point.**
     - L(x) = round(2^16·log2 x) comes from integer arithmetic (`constexpr`), or from a
       checked-in table whose hash a test pins. It is never computed with libm at run
       time, whose last bits are not specified alike across platforms.
     - The widths and the bound are stated. For n ≤ 2^32, one term's gain is under 2^23
       in 2^-16 units, and a document's sum over at most 2^32 terms is under 2^55, so
       `int64` holds it.
   - **The step.**
     - The left half is ⌊n/2⌋ of the range.
     - Each half is sorted by gain descending, ties by current position ascending.
     - The i-th pair swaps while its summed gain is positive.
     - At most 20 iterations run, stopping early when no pair swaps; that does not change
       the output.
     - Recursion stops at the leaf size.
   - **The graph.**
     - Term ids follow the merged dictionary's order.
     - df is counted over live postings (exact, one more pass) or summed over the
       sources (deleted documents included). The design picks one.
     - The forward index is built as compressed rows: a counting pass, then a fill pass.
       The design states how many passes over the postings that is.
   - **Threads.** Background merges run one per index on the host's executor
     (`phase-3.md:306-320`). The design states where bisection's parallel halves run:
     inside that slot, or on how many extra threads, bounded per node. T6's 1 and 8
     threads then agree by construction.
   - **Cancellation.** The merge checks its token per document, term, and stored frame
     (`phase-3.md:315-316`). Bisection checks it, and the executor's checkpoint, per
     iteration of each subproblem, so `abandon_merges` (install, close) is not held for
     minutes.
   - **Cost, projected before code.** About log2(5M/16) ≈ 18 levels × 20 iterations,
     each a pass over the edges plus two sorts, is on the order of 10^11 edge visits.
     The comparison is the `-bp` builder's forced merge, which sets no executor for
     bisection.

7. **P1-R7. D4's presets are presets, measured, starting from Lucene's documented
   values** (`reordering.md:72-78, 87-89`).
   - **Not limits.** `min_df`, `max_df`, the leaf size, the iteration count, and
     `reorder_min_docs` bound no allocation and cap no input. The limits rule's "at
     least the largest" does not apply. Its preset rule does: "the fastest measured one
     is the default" (`limits.md:6-8`), as for the merge policy's row (`limits.md:22`).
   - **Lucene 10.4's documented defaults:**
     - minimum document frequency 4,096;
     - maximum document frequency, a ratio of maxDoc, 1 (every term);
     - minimum partition size 32;
     - 20 iterations per level.
     `BPReorderingMergePolicy` suggests 2^18 = 262,144 documents for natural merges and
     reorders forced merges. The reference builder reorders only at force merge
     (`setMinNaturalMergeNumDocs(Integer.MAX_VALUE)`).
   - **strata's 0.5, 16, and 100,000 differ, with no reason given.** The design starts
     from Lucene's values, or states each difference with a measurement of compression,
     merge time, and the query rows. limits.md gains a presets row.
   - **Depth 30 is dead.** With 16-document leaves, a 2^32 − 2 document segment
     recurses at most 28 levels (16 · 2^28 = 2^32). The design deletes it, or names what
     it bounds.
   - **"When."**
     - At 5M, a 256 MB buffer flushes 16 segments of about 314K documents, so every
       natural merge passes 100,000. The default reorders nearly every merge during
       indexing, which neither Lucene baseline does: plain Lucene never, `-bp` only at
       force merge.
     - "T4's indexing cost changes only for the large merges" (`reordering.md:89`)
       therefore understates it. P1-R11's indexing rows choose the default.
     - `reorder = off` also turns off `force_merge`'s reordering. The design says so.

### Queries

8. **P1-R8. D5's list, completed against the code** (`reordering.md:91-105`).
   - **The one-segment collector and `top_k`:**
     - `top_collector::collect` rejects `score <= min_` when no segment is named
       (`search.hh:203`);
     - `docid(e)` returns the ordinal when no segment is named (241);
     - `order` compares ordinals within a segment (250), and `before_kth` does the same
       (260);
     - `update_min` without `begin()` makes the k-th score the threshold (279): that is
       the equal-bound skip;
     - `top_k` ties by ordinal (1257), admits only scores above the k-th (1285), and
       hands the k-th score to the scorer (1284, 1289). It holds no segment, so its
       signature changes, or one-segment searches go through `begin()`.
   - **Its callers:** `searcher.hh:187-194` (the one-segment path), `tools/
     engine_query.hh:159` (the benchmark engine's), and `search_test.cc:444` ("every
     path keeps the smallest ordinals"), which holds for docid-ordered segments only.
   - **"The digest, `verify`, and `parity` compare by docid, so they hold unchanged"**
     (`reordering.md:105`) is wrong for parity, and it misses what matters for verify:
     - `tools/parity.cc` writes documents with docid d + 1 into new segments (204) and
       never merges. It maps hits to docids as ordinal + 1 (222, 226, 262, 265). It
       cannot run on a reordered index, and on one it would compare the wrong documents.
       It gains a reordered build, through a reordering merge, and maps through
       `docid()`.
     - `verify` accepts tied documents within 1e-9 as a set (`strata_engine.cc:397-402,
       445, 498`), so it cannot see a tie resolved by ordinal.
     - The exact checks are `multisegment_test`'s (`:129-146`) and `engine_query_test`'s
       R2 case (`:241-242`). T4 extends them to permuted segments: exactly equal scores
       straddling the k-th, at k = 1, 10, and 100, both schemes, one segment and many,
       with deletes and floors. That is the P6 review's R1 check, owed since
       `docs/perf/p6-multisegment.md:145`.
     - **A check at scale.** The docid-ordered and the reordered 5M indexes hold the
       same documents under the same statistics, and scores sum in clause order. So
       every gate query's top-k (docids and scores, bit for bit) and count must be
       identical between them, at k = 10, 100, and 1,000, both schemes.
   - **Docid-order walks.** ARCHITECTURE's codec rules route "docid tie-breaks and
     docid-order walks" through the permutation, and FEASIBILITY lists dump with
     `skip_to` resume and Bool weighting. Over a permuted segment, such a walk goes
     through the sorted ordinals, with one stored-frame decode per document. The design
     states that cost and where those paths land (Phase 7's seam).
   - **Phase 4.**
     - `sortkey::may_compete`'s `docid_ordered` argument means "every docid in the block
       exceeds worst's" (`sortkey.hh:336-342, 383`; `sortkey_test.cc:295-299`). A
       segment's flag alone does not give that across segments.
     - `collapse::collapser`'s default `arrival` (`collapse.hh:93-97`) breaks ties by
       arrival, which in a permuted segment is ordinal order. Search passes it a ranking
       that is total over (score, docid).

9. **P1-R9. The equal-bound skip costs more than "a docid read per tied candidate"**
   (`reordering.md:100-101, 129-130`).
   - **The mechanism.** Scorers skip blocks whose bound is at or below the threshold
     (`search.hh:392-407`). Without the skip (D5.3), every block whose bound equals the
     k-th score is decoded and scored. A docid-ordered segment skips those once k such
     hits are in.
   - **Why it grows.** The gate's rows score with Lucene-compatible BM25 over one-byte
     length codes. A single term's score is then a function of (wdf, norm byte), so the
     k-th score's cell holds more documents as df grows, while k stays fixed.
   - **Probe `tie_skip`** (the first 10,000 documents; 40 terms with at least 10 blocks;
     block-max top-k): the skip saves 3% of blocks at k = 10 (506 against 519 in docid
     order, 549 against 563 in a random order) and none at k = 100. The sample's longest
     list has 78 blocks; a frequent term at 5M has 20,000–39,000. The probe does not
     bound the 5M cost.
   - **P21 makes it matter more.** P21 (added at `c96b271`) seeds each term's k-th
     score, which removes the warm-up blocks. What a permuted segment then still pays is
     the tied blocks.
   - **Required before D5's code:** an A/B-tier run on the docid-ordered merged 5M index
     with the equal-bound skip disabled (`update_min` and `top_k` forced to `nextafter`).
     That is D5.3's cost alone, per class, both schemes. T2 reports blocks decoded per
     query beside time. If a class loses, the design states what is accepted. The tie
     rule itself stays: Xapiand's E2E asserts the docid tie-break exactly (FEASIBILITY,
     `Enquire`).

### The gate rows

10. **P1-R10. T1's references and its projection** (`reordering.md:116`).
    - **The whole index.**
      - PLAN's reference is the smallest figure in the set with keys configured alike
        (`PLAN.md:45-51, 106-116`). The whole-index row is therefore ≤ 1.0× a keyed
        `-bp` build, not keyed Lucene.
      - strata is already 0.98× keyed Lucene (2,770.0 MB against 2,819.7), and 0.99×
        with the permuted map's 29 MB. T1 as written passes with no reordering gain.
      - The keyed `-bp` build is owed (`phase-3-ratchets.md:197`). Build it with the
        keyed single-thread builder and the reference's settings for bisection, so that
        the pair differs only by reordering.
    - **Every section** ≤ 1.0× its smallest counterpart: postings ≤ 833.2 MB, positions
      ≤ 1,556.9 MB, and the rest.
      - The 29 MB map lands in BENCHMARKS' counterpart row "docids, versions,
        `pk_filter`" against the id field's terms and doc values (`BENCHMARKS.md:173`).
      - Or the design rules it data the baselines do not keep (BENCHMARKS rule 1) and
        reports it apart.
      - Today's table has no such row.
    - **The projection, from the gate report's own numbers.**
      - Against the keyed single-thread build, `-bp` takes 15.5% off `.doc` and 8.9% off
        `.pos`, not 15.9% and 9.2% (`reordering.md:14`). That pair also differs in keys,
        indexing threads, and buffer, so the difference is not reordering alone.
      - The same relative gains put strata's postings at about 852 MB, 1.02× `-bp`'s
        833.2: a miss. Positions land at about 1,486 MB (0.95×).
      - The whole index lands at about 2,498 MB, before stored growth (P1-R4 (b)).
        Against keyed `-bp` at about 2,536 MB (2,508.1 plus Lucene's 27.7 MB of key
        terms), that is about 0.985×, a margin of roughly 1%.
      - The design states this projection, and what closes postings if P1 alone does not.
        PLAN's carried rows are timed rows, so carrying a disk row is German's decision.

11. **P1-R11. The four axes** (PLAN, Ratchets: "every technique reports its effect on
    all four axes"; Track P's rule). Add rows:
    - **Indexing:** Phase 3's T4 rows (1 and 8 threads, keyed, equal memory, five
      trials) with the default reorder preset, at ≥ 1.0× the faster engine. The
      indexing peak stays within the budget plus 10%, and at ≤ 1.0×.
    - **Serving memory:** the cgroup row (`bench/serve_mem.py`) on the reordered index.
      The gate report shows `-bp`'s mapped pages at 567 MB against plain Lucene's 639
      (−11%). Smaller postings can bring strata under 1.0×; a pinned 29 MB map pushes the
      other way (P1-R2).
    - **T3 with targets**, not "reported":
      - the forced merge's wall time and peak RSS against the `-bp` builder's force-merge
        phase, isolated (from its "Merge" line to exit), each side's thread count
        stated, at the same budget;
      - write amplification (Phase 3's T3), any spill included.
      `/usr/bin/time -v` is GNU time; the procedure names each platform's equivalent.
    - **Allocations per query:** the docid reads at ties allocate nothing per match, as
      the allocation-counting build shows (PLAN's Phase 3 memory row).

12. **P1-R12. T2's decision rule and its table** (`reordering.md:117`).
    - **The rule.** "No class slower than the docid-ordered index by its interval" is
      undefined. The design uses the rule the layers refactor and P6 use: per class, the
      reordered index over the docid-ordered one passes when the interval's upper bound
      is ≤ 1.03; a class above it runs again with ten trials and blocks if it stays above
      (layers L15; P6 review R6).
    - **The table names the reference engine per row.**
      - `gate.py` prints only `lucene-10.4.0`'s mean (`gate.py:187`), while the ratio may
        be against `-bp`.
      - The "after merging" rows suggest `-bp` sets several references. `TOP_10`
        negated, 509 µs at 0.86×, points to a reference near 590 µs, below both printed
        engines (`phase-3-ratchets.md:136`). That is approximate, since the columns are
        means and the ratios geometric means, and the report cannot settle it. T2 prints
        the `-bp` column.
    - **The carried rows.** From P1 on, `force_merge` reorders, so the carried "after
      merging" index is the reordered one. The design says so, and later gates measure
      the carried rows on it. T2's table therefore also scores every class against
      PLAN's target, ≤ 1.0× the faster engine by the interval's upper bound, and states
      each ratio's movement from the docid-ordered run. The owed "before merging" rows
      also run on reordered segments when natural merges reorder by default.

13. **P1-R13. T4 runnable, and covering the risks** (`reordering.md:119`).
    - **Permuted segments in the tests.** With min_df 4,096, bisection over a test-sized
      merge sees an empty graph and returns the identity. `lifecycle_test`, T1's crash
      workloads, and T2's schedules would never hold a permuted segment. So tests pass
      explicit permutations (reversed, seeded random) through D3's input, plus bisection
      with test presets, and assert that permuted segments exist.
    - **Cases:**
      - merges of permuted sources;
      - deletes during a reordering merge (P1-R4 (a));
      - replace, delete, and get by docid and by key, and expected versions, on permuted
        segments, including absent docids inside [`min_docid`, `max_docid`];
      - the digest and the verdict sequence equal to a docid-order run;
      - `check()`.
    - **A byte oracle:** a reordered merge equals the permuted writer's flush of the same
      live documents in the same order. That is `merge_test`'s property, extended.
    - parity on reordered indexes, once P1-R8's tool change lands. My Phase 4 review's
      reordering checks (R5, R9, R15) land with those plugins.

14. **P1-R14. T5's fuzz bases** (`reordering.md:120`).
    - `fuzz_segment` builds its bases with docids in order (`fuzz_segment.cc:22-57`). It
      gains a permuted, keyed base with docid gaps, so byte changes reach kind 2's
      checks.
    - The CPU-hour runs at the gate's commit, and each check is reached by a resealed
      unit test (C13).
    - `fixtures_test` exists only on the refactor branch. On main, the format-12 fixture
      is pinned in `segment_test.cc:309`.

15. **P1-R15. T6 on main** (`reordering.md:121`).
    - **Stored bytes differ by platform.** Main links the system's zstd (this build:
      `/usr/local/lib/libzstd.1.5.7.dylib`), and its frames differ by version: `d51f2aa`,
      on the refactor branch, measured `merge-deletes.sx` at 9,092 bytes with Ubuntu's
      1.5.5 and 9,028 with Homebrew's 1.5.7. T6 cannot pass on main.
    - `d51f2aa` is a build change, not a format change, and lands on main first.
    - The test compares against a pinned hash constant, so every platform checks one
      value. It runs on x86_64 and arm64 (Track P's platform rule), at 1 and 8 threads,
      with the reorder memory preset fixed (P1-R5).

16. **P1-R16. Behavior at cluster scale** (PLAN, Every gate; `phase-3.md`, Scale). Per
    node, at 1,000 indexes:
    - at most W merges run, each with bisection's working set (P1-R5), so the node's peak
      is W times it;
    - bisection's CPU per merged document, which the byte rate limiter does not bound,
      and its effect on merge throughput and merge debt;
    - the permuted map's pinned bytes, about 5.8 bytes per document at 5M, across open
      segments.

### Order

17. **P1-R17. Order with the layers refactor.**
    - **P1 lands on main before Phase 3's ratchet rows are recorded** (layers review,
      condition 1). That order is right. The refactor cannot land before the rows, and
      the rows need P1, so waiting for step 5 would deadlock.
    - **Keep the rebase small.** The branch has rewritten `reader.hh` and `writer.hh`
      (939 and 687 lines changed since `4e97839`). The docids codec (kinds 0–2: encode,
      validate, `docid`, `ordinal_of`) and bisection (a forward index in, a permutation
      out) become self-contained units. Main's `segment` and `merge_segments` call them,
      and so do the branch's `core_segment` and L6.
    - **The branch.**
      - Step 0 adds `permuted.sx` and a reordered merge output to its fixtures.
      - Step 5's oracle (the old `merge_segments`) includes the permutation path.
      - L15's step-5 merge row times the default forced merge, reordering included, with
        the output's hash equal between A and B. That needs P1-R15's determinism.

## Findings

Not covered by the required changes:

1. **A disk row carried without PLAN's rule.** The gate report carries the term
   dictionary row to format 13 ("carried to format 13 on that projection",
   `phase-3-ratchets.md:210`). PLAN's carried rows are timed rows (`PLAN.md:58-63`), so
   that carry needs German's decision recorded in PLAN. The same applies to postings if
   P1 leaves them above 1.0× (P1-R10).
2. **Costs that predate P1 will show in T3.**
   - The forced merge of 5M keyed documents charges a map entry and the key for every
     live document (`merge.hh:58-66`): (64 + 44.6 bytes) × 5,032,102 ≈ 547 MB,
     before bisection.
   - With the frame decode per document (P1-R4 (b)), the merge's peak and time carry
     both costs with or without P1. T3's with-and-without pair separates them.
3. **Reordering costs sections whose content follows docid order.** The gate report's
   format-13 dictionary projection codes key terms' single ordinals as deltas, 18.0 MB
   down to 9.8 MB (`phase-3-ratchets.md:200-210`). That works because URL order follows
   docid order here. After reordering, the ordinals are random, about 2.9 bytes per key,
   roughly 14.5 MB at 5M. The projection, on which the dictionary row is carried, is
   redone with P1, as is the stored row (P1-R4 (b)).
4. **The baselines keep no stable id to break ties by.** Lucene and PISA renumber
   documents when they reorder, so a tie follows the new order, and Lucene's threshold
   past a tie stays valid as it always is. strata's stable-docid tie rule is a
   structural cost relative to both (P1-R9). The stretch goal (`c96b271`), whose
   reference is PISA, measures P1 and P21 together.
5. **The design claims, against the evidence:**
   - "format 12 reserves the permuted kind": no reader accepts it (P1-R1);
   - "the digest, `verify`, and `parity` hold unchanged": parity cannot run, and verify
     cannot see tie order (P1-R8);
   - 15.9% and 9.2%: the cited table gives 15.5% and 8.9% (P1-R10);
   - the whole-index target passes without reordering, and postings project to 1.02×
     (P1-R10);
   - "only the large merges": at 5M, every natural merge is large (P1-R7);
   - "a docid read per tied candidate": the larger cost is the lost skip (P1-R9).
   It also claims too little:
   - where `-bp` sets a row's reference, P1 removes a handicap from strata's ratio, which
     T2 reports per class;
   - serving memory may improve (P1-R11).
   P1 does not close the 1.33× global gap: the gap record puts most of it in decoding
   skip entries, about 300 ns per touched block against Lucene's 90
   (`docs/perf/query-gap-merged.md`), a cost per block that reordering does not change.
6. **The reference builder** (search-benchmark-game master): the id is a `StoredField`
   added with `addDocument` (unkeyed); a 1,024 MB buffer; one indexing thread per
   processor; a 256 MB budget for bisection, which runs only at force merge. The gate
   report states the settings of the checkout it ran.
7. **Text.**
   - `phase-3.md:86-87` calls the monotone kind "bit-packed deltas". The code writes
     LEB128 deltas with a 16-byte index entry per 128 ordinals (`writer.hh:900-921`).
     The amendment in P1-R1 fixes the text with it.
   - D5.3 cites `phase-3.md:218–220`; the rule is now at 281–286.
   - D3's "merge fixtures" exist only on the branch. Main checks merge bytes through
     `merge_test`'s property.

## Conditions

1. **C1. P1's `docs/perf/` entry** holds the four-axis table at one stated commit: the
   Linux and FreeBSD VMs, macOS as a report row, and arm64 when its machine exists,
   before Phase 3R's gate. Timed rows run five interleaved trials.
2. **C2. Phase 3's ratchet rows** are recorded at a commit that includes P1, or with P1
   removed by Track P's rule. The layers refactor's baseline (my layers review,
   condition 1) is that commit.
3. **C3. Sanitizers.** The kind-2 decoder and the permuted merge run under ASan and
   UBSan. Any threads bisection starts run under TSan beside flushes and searches
   (`multisegment_test`).
4. **C4. Coverage** of the new code at TESTING's targets: the docids codec's three
   kinds, bisection, the permuted merge, and the tie paths in permuted segments.

## Which work starts when

| Work | Starts | Waits for |
|---|---|---|
| Kind 2 (reader, writer), `permuted.sx`, the permuted fuzz base, resealed tests | Now | P1-R1 and P1-R2 in the design, in the same commit |
| The permuted writer mode | Now | P1-R3 |
| `merge_segments` with an explicit permutation, the permuted plan, explicit-permutation tests | Now | P1-R4 (a)–(c) |
| D5's tie paths, `top_k`'s segment, parity's mapping, the exact tie tests, the 5M identical-results check | Now | P1-R8 |
| The equal-bound skip A/B on the docid-ordered 5M index | Now, A/B tier | — |
| `d51f2aa` (the pinned zstd) on main | Now | — |
| The keyed `-bp` build | The next VM slot | P1-R10's settings |
| Bisection code | After a revision check | P1-R5, P1-R6, P1-R7 |
| L6's order source | Before layers step 5 | P1-R4 (d) |
| The gate run (T1–T6 and P1-R11's rows) | After bisection lands | P1-R10 to P1-R16 |

## Verdict and required changes, in one line each

**APPROVE WITH CONDITIONS.** A revision check of P1-R1 to P1-R17 comes before the
bisection code and the gate run.

1. P1-R1: format 12 is amended in place, not reserved; the kind follows the order
   written.
2. P1-R2: D1's widths, bounds, and validation, and its open-time cost and serving pages,
   stated and measured.
3. P1-R3: the permuted writer's (docid, ordinal) sort charged.
4. P1-R4: the permuted merge plan for carry-over; one decode per stored frame; singleton
   merges keep their order; L6's order source.
5. P1-R5: the forward index not held whole in memory; a deterministic fallback preset.
6. P1-R6: the cost formula, integer log table, widths, step, graph, threads,
   cancellation, and projected cost written out.
7. P1-R7: the presets start from Lucene's documented values or are measured; depth 30
   deleted; the default "when" chosen by the indexing rows.
8. P1-R8: D5 completed against the code: the one-segment collector, `top_k`, parity's
   mapping, exact tie tests, docid-order walks, sort keys, and collapse.
9. P1-R9: the equal-bound skip's cost measured on the 5M index before D5's code.
10. P1-R10: T1 against keyed `-bp` and every section's smallest counterpart, with the
    projection stated.
11. P1-R11: indexing, serving-memory, merge-cost, and allocation rows with targets.
12. P1-R12: T2's ≤ 1.03 rule, the `-bp` column, and the carried rows on the reordered
    index.
13. P1-R13: T4's tests that hold permuted segments, with the carry-over, lookup,
    byte-oracle, and parity cases.
14. P1-R14: a permuted base in `fuzz_segment`.
15. P1-R15: the pinned zstd on main; a pinned hash checked on x86_64 and arm64.
16. P1-R16: the scale statement.
17. P1-R17: P1 on main first, the codec and bisection as self-contained units, and the
    refactor rebased with fixtures.

Not run: benchmarks of any tier, the VMs, fuzzing, sanitizer builds, Linux, FreeBSD,
and arm64 builds, the layers branch build, the 5M corpus and indexes, the n1 raw results,
and Lucene's source.
