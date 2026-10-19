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

M16 and M23 committed; M22 next.

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

## Next

1. M22: Elias-Fano blocks chosen per block by size.

## Blocked on

Nothing.

## Last verified

2026-10-19: ctest 36/36 at 026ee10 (macOS x86_64, Apple clang, RelWithDebInfo, quiet exit 0).
