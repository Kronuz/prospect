# f13-query: format 13's query freeze items (M16, M20, M22, M23, M3t)

- Design: docs/design/format-13.md (F10 to F13, "Choosing F10 to F13 together" F13-R8, F13-R44,
  F13-R45) on main 026ee10; the decisions are perf/query-exp's freeze table
  (`docs/perf/query-gap-format13.md` at fac1e8d) with `query-gap-skip-entries.md` (E6),
  `query-gap-seeds.md` (E4), `query-gap-m3.md` (E10), and `ab-run-level.md`; D14 and D15 in
  design/query-gap's `docs/design/query-gap.md`.
- Branch / worktree: phase4/f13-query from origin/main 026ee10, at
  /Users/kronuz/code/Kronuz/strata-wt/f13-query.
- Items, one commit or more each, each format change bumping `format::pre_freeze` (C17):
  1. M16: advance entries at A3@128, and the joint selection's record (F13-R8).
  2. M23: ordinal gaps stored as gap - 1, checked by the header width (25 bits) or a 64-bit sum.
  3. M22: Elias-Fano as a block ordinal codec, chosen per block by size.
  4. M20: two dominance pairs per k (10, 100, 1,000) per term with skip blocks; seeds off in
     segments with deletes.
  5. M3t: bound-ordered traversal for single terms in reordered segments (format-free).

## State

All five items built and pushed, origin/main d058cd9 merged; the Mac measurements done (`docs/perf/format13-query-items.md`). M16, M23, M20, and M3t pass on this host with no loss; M22 closes the postings row by projection but fails D14's guard (intersections, phrases, negations, counted unions 1.09 to 1.42 against main), which blocks on a decision that is not mine (below).

## Done

- M16 (F10 at A3@128), the commit after cd98766: each group's level 0 becomes one record of
  advance entries bit-packed at widths level 1 implies (lasts of all but the last block, offsets
  of all but the first, each block's codec code at the term's width from the skip flags), then
  the blocks' pairs, then the later blocks' positional deltas. The reader locates a block by a
  search over the entries it reads (the found block ends at or after the target, its
  predecessor before it), with no group decode; each block's entry is checked when decoded
  (last above the previous and at most the group's, bytes inside the group's, a kept codec).
  Skip flags: bit 0 wide, bits 1-2 the codec width, bit 3 reserved for F12's partitions (P1,
  F13-R45's place) and refused, the rest refused. `format::pre_freeze` 0x80000002 (C17);
  fixtures, pre_freeze.txt, the T8 hash, and format13_test's prefix CRC re-pinned; digest.txt
  unchanged. Tests: postings_test (new: every read path of seven term shapes against a model,
  both offset widths; damaged entries, flags, and every advance-entry bit flipped fence or
  answer in order; a bitset block made consistent with a last past the document count),
  segment_test's C13 case on the new layout. fuzz_postings (new: round trip, then flips inside
  a term's postings, aborting on any ordinal out of order or range; corpus of 600 from 5 min,
  17,204 runs, no finding) and fuzz_segment's walk now aborts on such an ordinal.
  - Deviations from F10's text, for the architect: the pairs and positional deltas are found
    after the advance entries (their start from the entries' derived size) instead of by
    level-1 offsets of their own, so level 1 keeps format 12's size; the codec field is inside
    A3 at a per-term width (0, 1, or 2 bits) instead of a separate per-block field.
  - Revert checks (.scratch/revert/mutate.py, each mutant built and run): removing the entry's
    check against its group's last fails postings_test:358 (the consistent bitset case);
    removing the flags check fails postings_test:292. The other entry checks (last above the
    previous, offsets ordered and inside the group, the codec width) are defense in depth: with
    any one removed every damaged case still fences through the block decoders' own checks.

- M23 (F11, F10's check), the commit after 88f23ee: FOR ordinal blocks store gap - 1
  (`format::encode_gaps`, `decode_gaps`), so ordinals increase by construction; a block whose
  header declares at most 25 bits is checked by its last ordinal against the entry's, a wider
  one by the 64-bit sum of its gaps against last - base; a constant block by (value + 1) x 128;
  an exception (PFOR) header is refused, since the writer never stores one. check_increasing
  stays for tails only. The writer's bitset-or-FOR choice now compares against FOR at gap - 1,
  so dense blocks with gaps of 1 and 2 move from bitset to FOR (17 bytes against 18 or more).
  `format::pre_freeze` 0x80000003; fixtures, the T8 hash, the prefix CRC re-pinned; digest.txt
  unchanged; segment_test's C13 term made 60% dense so it keeps bitset blocks. Tests:
  postings_test (every header width 0 to 32 round trip from a first block and from a base, a
  last off by one and a truncated block refused, a constant block, gaps wrapping by 2^32 onto the
  entry's last refused at width 31, an exact width-31 block decoded, a PFOR header refused even
  where its low part ends at the last); fuzz_postings now also feeds raw bytes to decode_gaps and
  aborts if it accepts a block outside (base, last] (3 min, 9,619 runs, no finding; 66 inputs
  added).
  - Revert checks: removing the 64-bit sum fails postings_test:399 (the wrap); removing the last
    check fails :378 and the walk (:319); removing the PFOR refusal fails :412; removing the
    constant's check crashes postings_test (SIGSEGV, an ordinal past the document count).

- M22 (F11's codec code 2), the commit after b9a30d4: Elias-Fano as a block ordinal codec
  (`format::ef`): the low l = log2(u / 128) bits of each ordinal's offset in the block's span u,
  bit-packed by bitpack at width l, then the high parts in unary; no header, its size a function
  of u. The writer takes the smallest of FOR at gap - 1, bitset, and Elias-Fano per block, ties
  to the earlier. The reader keeps an Elias-Fano block in place like a bitset block: advance()
  inside it is a successor probe (select0 of the target's bucket, then a scan), after a load
  check of 128 high bits and the last ordinal; next() and the bulk paths decode it whole,
  checking strict increase from the block's first ordinal to its last. Codec width 2 is now
  readable; code 3 is refused by the entry check. `format::pre_freeze` 0x80000004; fixtures, the
  T8 hash, the prefix CRC re-pinned; digest.txt unchanged. index_test's "postings can grow"
  merge rooms start at 150,000 (from 170,000): the merged block is Elias-Fano, smaller than its
  FOR, so the old range never refused (33 merged, 18 refused now). Tests: postings_test (a
  clustered term with Elias-Fano and FOR blocks through every read path; 400 synthetic blocks of
  four shapes round-tripped, every successor against the model, damage refused: a high bit
  cleared, a last moved, a short span, two equal ordinals (decode refuses, the probe's load check
  does not), an ordinal past the last in its bucket (the probe refuses); code 3 on a FOR block;
  codecs swapped); fuzz_postings feeds raw bytes to ef::decode and ef::successor (4 min, 13,956
  runs, no finding).
  - Revert checks: the entry's code check fails postings_test:300; decode's increase check fails
    :340 and :496; probe_ready's count fails :482; successor's bound fails :504.

- The merge of origin/main d058cd9 (phase4/dir-lock: files::pre_freeze 0x80000005), 7d91c9f:
  pre_freeze.txt keeps both families' rows (segments from this branch, files from main); ctest
  39/39.
- M20 (F13 at two pairs per k), the commit after 7d91c9f: a term with skip blocks and wdfs
  stores, after its skip flags, for each k of 10, 100, 1,000 up to its df, two dominance pairs
  (at least k postings with wdf >= w and length <= l): the one bm25 scores highest at the
  segment's statistics with l as a code, and the one bm25_xapian scores highest with l exact,
  [w][code u8][w][l] in LEB128, 2 to 4 bytes a pair; these are E4b's seeds. The writer finds
  them in one pass over the postings by wdf, highest first (a counting sort), keeping the k
  shortest lengths per k in a heap, and stops once no lower wdf can beat either best at the
  segment's shortest length. A scorer's `seed(k)` is the larger pair's score under its own
  weight for the smallest stored k at or above k, times (1 - 2^-40) so equal scores still
  enter; a union's is its clauses' largest. `search()` builds every segment's scorer first and
  seeds the collector, or top_k's floor for one segment, with the largest seed of the segments
  without deletes (a segment with deletes is wrapped by live_scorer, which forwards none);
  engine_query's segment-file path does the same. check() verifies each pair against its
  postings. `format::pre_freeze` 0x80000005; fixtures, the T8 hash, the prefix CRC re-pinned;
  digest.txt unchanged; segment_test's level-1 probe skips the seeds. Tests: postings_test (the
  stored pairs score exactly the brute-force best over every w for both presets, each dominates
  its k postings; check() refuses a pair no posting reaches; 240 searches over one segment,
  three, and three with deletes, terms and unions, k 1 to 1,000, both presets, equal an
  exhaustive reference to the bit; a segment whose seed comes from deleted postings still
  returns its 10 live hits); fuzz_postings compares seeded and unseeded top-k under both presets
  and runs check() on the damaged copy (4 min, 864 runs under a concurrent ctest, no finding).
  - Revert checks: taking the seed before the live wrapper fails postings_test:648; dropping the
    (1 - 2^-40) margin fails :621 (an equal k-th score dropped); summing a union's seeds fails
    :621; removing check()'s seed verification fails :574.

- M3t (format-free), the commit after 07bf37d: in a reordered segment (the collector's current
  segment not docid-ordered) a term with more than one skip group collects its tail, then its
  groups by descending bound, each from a fresh iterator with the per-block skip inside, until
  a group's bound cannot beat the threshold (perf/query-exp's E10, its union half dropped there).
  A docid-ordered segment keeps today's path. Tests: postings_test (a permuted 20,000-document
  segment with high-wdf documents gathered in two ranges, three terms, both presets, k 1 to
  1,000, with and without seeds: 48 top_k and 24 search() results equal an exhaustive
  reference, ties by docid).
  - Revert checks: dropping the tail fails postings_test:692; stopping at 1.5x the threshold
    fails :692 and :699; stopping at < instead of <= changes no result (it visits more).

- M20's writer fix, dd55eb6: seeds_of read min_length() per term, a scan of every length; a 1M
  docid merge took 115 s against 71 to 79 s. finish() takes it once.
- M22's decoder, 492b549 (after the per-item A/Bs put every loss on M22): a load reads the first
  ordinal from the first high bit and low part; the first advance into a block probes, a second
  decodes the block and searches it; the decode places a byte of high bits at a time from a
  table; probe_ready goes (the successor answers only inside the span, the decode checks the
  rest). Base against this build took intersection `TOP_10` from 1.614 to 1.131 and the decode
  alone took `COUNT` union 0.902 against the build before it. Test: postings_test refuses a first
  high bit past the span (revert: :513 fails).
- Measurements (`docs/perf/format13-query-items.md`, 2026-10-19, this Mac, light holds): bytes
  per item at 100k and 1M (1M reordered postings: M16 −0.72 MB, M23 −0.08, M22 −19.75, M20 +0.49;
  the kept set −10.5%); per-item run-level A/Bs; the kept set against main on 1M reordered and
  merged, both presets; dump identical to main's and verify 0 mismatches on both 1M indexes;
  indexing within a second at 100k. Phase times: 100k indexes for five binaries 55 s; 1M indexes
  about 5 min per binary (index 35 to 38 s, docid merge 71 to 115 s, reordering merge 184 to
  214 s); identity and verify 3 min; run-level sessions 99 to 122 s per phase at six runs, 31 to
  40 s at four runs of three commands; the 5 fuzz runs 3 to 5 min each.

## Next

1. The lead's gate VM confirmation: the 5M bytes, identity, verify, and the run-level sessions
   (commands in `docs/perf/format13-query-items.md`, "Gate VM confirmation"); parity against the
   Xapian oracle there.
2. M22's ruling (below), then either A4@128 (if M22 stays and the 5M bytes confirm), a cheaper
   Elias-Fano rule, or M22's removal with its bump.

## Blocked on

1. M22 (for the lead, the architect, and the owner): chosen per block by size, Elias-Fano is the
   smallest codec for about a tenth of the postings' bytes at 1M, which closes the postings row
   by projection (845.6 MB to about 757 MB at 5M, against keyed `-bp`'s 833.7), but every path
   that decodes whole blocks then decodes Elias-Fano, and the kept set loses 1.09 to 1.42 on
   intersections, phrases, negations, and counted unions against main even after 492b549. E6's
   evidence was a probe per block on 50 terms, not the decode or the share by size. Options,
   each a format decision: (a) keep it and spend effort on a faster decode (SIMD select, as
   PISA's partitioned Elias-Fano does), measured before the freeze; (b) choose Elias-Fano only
   when it saves a stated share of the FOR block's bytes (still a function of the block alone,
   F13-R8 item 6), trading bytes for time on a measured curve; (c) drop M22 (its decoder and
   code 2), which leaves the postings row about 10.5 MB over and A3@128 standing. Recommendation:
   (b) measured at two or three thresholds on the gate VM, since it keeps the row's bytes where
   blocks are sparse and FOR's speed where they are not; this branch builds nothing further for
   M22 until the ruling.
2. A4@128 against A3@128: paid for by projection only with M22 (the joint selection, F13-R8);
   the lead's call after the 5M bytes.

## Last verified

2026-10-19 (macOS x86_64, Apple clang, RelWithDebInfo, quiet exit 0 or 2 with nice): ctest 39/39
at 492b549 (38/38 at each item's commit before the merge, 39 with main's new test after it);
fuzz_postings' corpus (779 inputs) replays in about a second; libFuzzer (Homebrew clang 23,
ASan+UBSan) ran 3 to 5 minutes per item with no finding. CI: pushed at 492b549 and this commit;
no run was waited on (the lead watches CI).
