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
