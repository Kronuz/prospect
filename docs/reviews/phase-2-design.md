# strata review: Phase 2 design

- Date: 2026-10-11.
- Reviewed: `docs/design/phase-2.md` at strata `2ad17fd`.
- Checked against:
  - PLAN's Phase 2 gate at `dfd6112`.
  - ARCHITECTURE.
  - The Phase 1 review.
  - The Phase 0 baselines.
  - The oracle vectors in `strata-oracle/vectors`.

## Verdict

**APPROVE WITH REQUIRED CHANGES.**

The structure is sound:

- One segment file with 64 KiB region checksums.
- A block-tree term dictionary keyed by each block's first term.
- FOR and PFOR postings with two skip levels and competitive (wdf, doclen) pairs.
- An arena-backed in-memory segment.
- Block-max MaxScore and WAND scorers.
- Both BM25 formulas.
- Exact counting.

The design is not yet complete for the gate as PLAN states it. It has no analyzer, no
parity harness, no required-plus-optional scorer, no count shortcut, no reader
corruption rules, and no test plan. Several format details are open, and the design
promises that later phases add to these formats "without changing them", so those
details must be settled before the writer exists.

D1–D11 land before implementation starts. D12–D18 land before the work they cover,
and all of them before the gate.

## Required changes

### Format (before the segment writer)

1. **D1. Positions as one stream per term.** Each document's positions are deltas that
   restart at the document. The documents' runs are concatenated in ordinal order and
   packed in PFOR blocks of 128, with an LEB128 tail. A skip entry records the stream
   offset, and the index within that block where its posting block starts. "Per
   document ... PFOR blocks of 128" would leave nearly every list as LEB128, because
   most (term, document) pairs have a handful of positions.
2. **D2. The term block's offset table.** Say whether it points at every entry or at
   restart points (entries with a shared length of 0).
   - Prefix-compressed entries decode sequentially from the last restart, so an offset
     per entry buys nothing unless entries restart.
   - The choice sets lookup cost, and the `termdict` row measures lookup cost against
     `seekExact`.
3. **D3. Competitive pairs within a byte budget.**
   1. When the Pareto frontier exceeds four pairs, the stored pairs must dominate the
      dropped ones: merge neighbors into (max wdf, min doclen). Bounds then stay valid
      but loosen.
   2. Show that skip data fits the postings row (the whole section at most 1.02×
      Lucene's `.doc`).
      - That leaves about 75 MB beyond strata's 934.9 MB of deltas and wdf; Lucene's
        skip data takes 26–44 MB.
      - Exact doclens in LEB128 make each pair about twice the size of Lucene's 1-byte
        norm.
      - If the estimate exceeds the budget, store pairs rounded in the safe direction:
        wdf up, doclen down to a 1-byte log scale. Bounds stay valid, and scoring still
        reads exact lengths from `lengths`.
4. **D4. Term-level bounds in the term info.** Store the term's maximum wdf and its
   whole-list competitive pairs in the term info:
   - MaxScore partitions query terms by each term's global bound.
   - Xapian's BM25 bound uses the wdf upper bound.
   - Tail-only terms (`df < 128`) have no skip data to read either from.
5. **D5. A "no positions" flag.** Keyword and boolean terms, and text indexed without
   positions, then pay nothing for a positions offset.
6. **D6. The footer.**
   - Give every field's width. The listed fields come to 36 bytes if the TOC length is
     a `u64`.
   - Add a footer magic, so a truncated or foreign file fails on the first read.
   - Define "the segment's ordinal range". Ordinals are segment-local, so the useful
     fields are the minimum and maximum docid.
7. **D7. The docid base.** Xapian docids start at 1. State the base: in a fresh single
   segment, docid = ordinal + 1. State also that `docids` maps ordinals to docids from
   the first format version.
8. **D8. A golden segment fixture and byte order.** Check in a small segment and compare
   it byte for byte, per PLAN's fixture rule. State that every fixed-width field is
   little-endian, matching `bitpack`.

### Reader safety (before the reader)

9. **D9. Corruption rules.**
   - Every offset and length read from the file (TOC entries, term-info offsets, skip
     offsets, position offsets) is checked against its section's bounds before use.
   - A decoded block's last ordinal must equal its skip entry's last ordinal; this
     check costs one compare per block.
   - Ordinals must increase and stay below the document count.
   - Any violation fences the segment.

   Memory-safe decoders do not make corrupt deltas monotone, and a non-monotone ordinal
   can stall a leapfrog.
10. **D10. Region verification in Phase 2.** The synchronous backend never reads a
    region, so it cannot verify one. Either verify every region at open in Phase 2
    (2.6 GB at 15 GB/s takes about 0.2 s), or state that per-region verification arrives
    with the asynchronous backends. Either way, specify how region checksums reach
    `residency`. PLAN lists that path as a Phase 2 addition, and `prefetch(file, ranges)`
    carries no checksums today.
11. **D11. A `SIGBUS` policy.** A media error under mmap raises `SIGBUS`. State whether
    that is a node failure, which REPLICATION's fault model covers, or is caught per
    thread. This is carried from the Phase 1 review.

### Query execution (before the scorers and collectors gate)

12. **D12. A required-plus-optional scorer with block-max pruning.** This is Xapian's
    `AND_MAYBE` and Lucene's MUST plus SHOULD. The harness's intersection_union classes
    need it, and Xapian's `QueryParser` turns `+a b` into it. The scorer table has no
    row for it.
13. **D13. Count shortcuts.** A single-term count is the document frequency, since Phase
    2 has no deletes. Lucene and Tantivy answer the term COUNT at the pipe floor, so
    strata cannot iterate there. For TOP_100_COUNT, count every match but score only the
    documents whose block bound beats the heap threshold. Tantivy leads Lucene by
    1.1–2.2× on that command.
14. **D14. Automaton intersection.** "Intersect the automaton with the index to find
    candidate blocks" misses terms, because a block's later terms need not lie on its
    first term's path in the FST. Use automaton-driven seeking instead: compute the next
    string the automaton can accept, `floor` to its block, and scan. That works with
    first-term keys. Prefix-aligned blocks are the other option. Phase 5 builds the
    intersection, but the block key is a format decision made now.

### Analysis, parity, and measurement (before the work they cover)

15. **D15. The analyzer.** PLAN puts the Xapian-compatible analyzer in Phase 2, and the
    design has no section for it. Specify:
    - Word rules.
    - Case folding.
    - `Z`-prefixed stems.
    - Stop strategies.
    - Field prefixes.
    - wdf increments.
    - Indexing without positions.

    Extend the oracle's term-generation vectors to what Xapiand passes: a prefix, a wdf
    increment, stop strategies with a stopper, and positionless indexing. Today they
    cover English stemming strategies and CJK n-grams only.
16. **D16. The parity harness.**
    - The oracle's rank index uses `TermGenerator` with no stemmer, and `QueryParser`
      with `FLAG_DEFAULT`.
    - Build strata's parity queries from the recorded descriptions (the `Query(...)`
      strings, which carry operators and wqf), instead of reimplementing `QueryParser`
      in Phase 2.
    - Compare match sets, and top-10 (docid, weight) at relative error ≤ 1e-9. Treat
      scores within the tolerance as ties ordered by docid; summation order differs
      between engines, so exact order on near-ties is not guaranteed.
17. **D17. Benchmark settings.**
    - **Analyzer.** The gate rows use whitespace analysis, as the baselines do, not the
      Xapian analyzer.
    - **Indexing row definition.** It defines:
      - its pipeline, from input parsing to a committed segment;
      - its document count;
      - every engine's memory budget.

      Tantivy limits a writer thread's memory budget to under 4 GB, so a 5M-document
      single segment can force it to merge. Pick a count that fits one segment for every
      engine, or include merges for all.
    - **`termdict` row.** It needs a microbenchmark against Lucene's BlockTree
      `seekExact` on the same vocabulary, with present and absent keys. With first-term
      keys, every absent lookup scans a block, while Lucene's prefix index rejects some
      without reading one. Expect absent keys to be the hard half; that is the
      PK-lookup path in Phase 3.
18. **D18. The test plan.**
    - **Scorers against a naive evaluator** on random corpora, comparing match sets and
      exact scores for every operator.
    - **Pruning against no pruning:** top-k with pruning equals top-k without it.
    - **`termdict` against `std::map`.**
    - **Reader fuzzing** of the TOC, term blocks, skip data, positions, and lengths, one
      CPU-hour each at native speed, recording executions per second.
    - **The golden segment fixture** from D8.

## Minor

1. **Pinning list.** "Skip roots" has no section; name what is pinned.
2. **Lucene BM25.** State that it follows Lucene 10's formula, which since Lucene 8 has
   no (k1 + 1) factor in the numerator. Ranking is unaffected.
3. **Long-term blocks.** The 1 KiB cut means blocks of long terms hold fewer than 25
   terms. State it.
4. **Stored fields.** The baselines store only an id; match that in the `stored`
   section for the indexing row.

## Unaffected

These parts stand as written:

- The header, the section table, and the TOC.
- `wdf − 1` in blocks and tails.
- The inlined `df == 1` posting, whose wdf equals its collection frequency.
- The two skip levels, each per block and per 32 blocks.
- The 4,096-ordinal evaluation windows with `task::budget` checks between them.
- The exhaustive bitset windows for counts.
- The collectors' heap-to-threshold feedback.
- The Xapian estimate triple, exact when counting.
