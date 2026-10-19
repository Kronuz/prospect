# Code review: phase4/f13-query (format 13's query freeze items)

- Branch: `phase4/f13-query` at `bc00f08` (equal to `origin/phase4/f13-query`; the developer's
  worktree has no unpushed commits), reviewed as `git log origin/main..bc00f08` (17 commits, main at
  `d058cd9` merged in at `7d91c9f`).
- Read: `docs/perf/format13-query-items.md`, `docs/status/f13-query.md`, `docs/status/format13.md`
  (its diff), `docs/design/format-13.md` (F2, F10 to F13, "Choosing F10 to F13 together"),
  `docs/reviews/format-13-design.md` (F13-R8, F13-R45, C17), `docs/reviews/calibration.md`, and on
  `perf/query-exp` at `fac1e8d` `docs/perf/query-gap-format13.md` and `docs/perf/ab-run-level.md`.
- Review worktree: `/Users/kronuz/code/Kronuz/strata-wt/review-f13-query` (detached at `bc00f08`).
  Everything I built, ran, or wrote is in its `.scratch/` (listed under "Probes" below).
- Host: macOS 15.6, x86_64 (i9-9980HK), `quiet check` exit 0 before each build and run; builds at
  `nice -n 10`; the two 10-minute fuzz runs under `quiet run` light holds.

## Verdict: APPROVE WITH CONDITIONS

The code does what the ledger says, and I could not make it give a wrong answer. Results on the
tip match main's exactly at 100k: dumps are byte-identical with and without a count, verify shows
0 mismatches, and that holds on a 52-segment index with deletes, a docid-order merge, and a
reordered merge. In my sweep no seed was ever above the true k-th score: 8,496 of 8,496 seeded
probes were strictly below it. With M3t traversing groups, every one of 11,984 resealed bit flips
either fenced or answered correctly. Every new check I mutated fails a test, except the two the
developer already named and one defensive check (FQ6). Nothing blocks landing as code.

What remains is format and evidence. Bytes freeze with this branch, and several of them differ
from what the design text promises; F13-R8's "flags in a field of their own" is one. The architect
has to rule on those before the freeze (FQ3). The branch's result-identity evidence was produced
on an earlier commit, and it never exercised union seeds (FQ1, FQ2).

## Findings

**FQ1. The identity and verify evidence comes from an earlier binary, not the kept set at the tip
(medium; condition C1).** `docs/perf/format13-query-items.md` ("Results are identical to main's")
and the ledger cite dumps that are byte-identical over 1,162 queries and verify with 0 mismatches
on both 1M indexes. The raw run (`strata-wt/f13-query/.scratch/ab/identity.sh`) used `bin/m3t`
(32e4286) on the `m20-*` indexes at `0x80000005`. Four later commits change the read or write path
(`492b549`, since removed, then `03c79cc`, `a2b08d7`, and `6d2e642`). No index at the tip's
`0x80000007` exists in the developer's `.scratch` (I read the version word of every `seg-*.sx`
there). The claim held when I reran it at the tip at 100k (`.scratch/idx/`):
- Main's `strata_engine dump` (`d058cd9`, built in `.scratch/wt-main`) against the tip's: identical
  on `upd` (52 segments, one with deletes), `merged`, and `reordered`, each 6,972 lines.
- The no-count dump of FQ2: identical on all three, each 13,944 lines.
- `probe_search verify`: 1,162 queries and 0 mismatches on each, at k 1, 10, 11, 100, 1,000, and
  1,001, with and without a count.

The 1M and 5M rows are still owed, at the tip.

**FQ2. Neither dump nor verify ever applies a union's seed on the indexes they were run on (medium;
condition C1).** Both always pass a count (`tools/strata_engine.cc:441, 485`). On a one-segment
index, `search()` goes to `top_k(..., count, floor)`, which zeroes the floor whenever the count is
not exact (`include/strata/search.hh:1336`), and a union's count never is. The 1M identity runs
were all one-segment indexes, so they never ran a seeded union. Only `postings_test`'s 6,000-
document segments tested union seeds before this review. My `probe_search dump`/`verify` runs
without a count, and so does `probe_seeds` part 2: 8,064 searches over mixes of docid-ordered,
reordered, and deleted segments, unions with phrase, conjunction, and exclusion children, wqf 2,
and k from 1 to 5,000, all equal to the exhaustive scan. Recommended: give `strata_engine
verify` and `dump` a pass without a count, so the gate VM's run covers union seeds.

**FQ3. F10 to F13 deviations that change bytes the design text promises, for the architect's
freeze-readiness review (design; not ruled here; condition C2).** The ledger lists ten. These are
the ones where the frozen bytes or their meaning differ from `docs/design/format-13.md` or from a
ruling:
1. **Flags are not a field of their own.** The design says "Flags, a field of their own
   (F13-R8)... one bit per block when only FOR and bitset remain, a byte per block otherwise" (F10
   item 2, and F13-R8's ruling). In the code, each block's codec code sits inside A3's bit-packed
   entries, at a width per term of 0, 1, or 2 bits taken from the skip flags (`format.hh:442-454`,
   `reader.hh:1515-1517`). This contradicts an architect ruling, not just the text (the ledger's
   deviations 2 and 5).
2. **Level 1 has no impact or positional offsets.** F10 item 3 says impacts are "located by a
   fixed-width offset per group in level 1", and level 1 "holds ... its blocks, advance, impact,
   and positional offsets". The code keeps format 12's level-1 fields. Impacts start at the advance
   entries' computed size. The positional deltas are found by walking every block's impact list in
   the group (`reader.hh:1542-1570`), so a phrase path now parses impact-list sizes, which F10
   reserves for "scoring paths" only. F12's reserved partitioned form (bit 3) would change that
   walk (deviation 1).
3. **A3's widths are derived, not "recorded in level 1"** (F10's A3 row), and the redundant last
   and first offset are dropped (deviations 3 and 4). Level 1 keeps format 12's size, which is
   consistent with that.
4. **The bitset boundary.** F10 says a bitset block "is chosen only when smaller", and F11 says
   per-block choices are made "by size alone". The writer instead compares the bitset against FOR
   over plain gaps, a block it never stores (`writer.hh:558, 690-700`). A bitset can therefore be
   chosen while larger than the stored FOR: 18 bytes against 17. The result is still a function of
   the block, and the 513-byte bound that F10's 16-bit offset argument relies on still holds,
   since a bitset is chosen only below a plain-gap FOR, which is at most 513 (deviation 10).
5. **M20's code pair has a different meaning.** F13 says "lengths round up": l is stored as the
   smallest code whose decoded length is at least the true length. The code stores
   `encode_length(l)` (rounded down) and means "every one of the k postings has code ≤ this". Under
   `bm25_xapian` it reads that as `max_length_of(code)` (`format.hh:542`, `search.hh:411-418`).
   It is sound, since both readings bound from below, and it is tighter under `bm25`: probe_seeds
   part 1 found no seed above the true k-th score. But it is a different frozen meaning. `no_wdf`
   terms store no seeds. There are two pairs per k where the text says "at most 3" (deviation 8).
   F13's sentence "Seeds do not apply ... where the threshold must admit ties" is also replaced by
   the (1 − 2⁻⁴⁰) margin, which is text drift.
6. **Exception (PFOR) headers in ordinal blocks are damage.** F10's M23 check names "an exception
   block's width plus its high bits" as a valid header. The reader refuses any exception block
   (`format.hh:499`), which freezes "ordinal FOR blocks never carry exceptions" (deviation 7).
7. **Code 2 is reserved through the skip flags' codec width** (`reader.hh:1160-1164`). This is
   F13-R45's place, consistent with the ruling (deviation 9).

I found no byte-level change that the ledger leaves out. I make no ruling on whether any of these
is acceptable.

**FQ4. The M20 writer's cost at scale rests on single, non-interleaved runs (low; condition C3).**
dd55eb6 fixed a per-term scan of every length (115 s against main's 70.6 s, docid merge at 1M,
`strata-wt/f13-query/.scratch/idx-1m.log:72`). The only 1M merges after the fix are the
threshold builds (`idx-ef.log`: 70.2, 60.6, and 73.1 s against main's 70.6 s), one run each on a
shared host. Neither the kept set's own merge time at `6d2e642` nor anything at 5M is recorded.
By reading `seeds_of` (`writer.hh:755-823`), the cost is linear: a counting sort, O(df + max wdf)
for wdfs up to 2¹⁶ and a stable sort above that; then at most 3 heap operations per posting until
the stopping rule fires, each O(log k) with k ≤ 1,000. Nothing in it is superlinear, so I expect
it to scale, but that is a reading, not a measurement. Condition: the gate VM's 5M build records
both merge times beside main's.

**FQ5. The fuzzers never build a reordered segment, so M3t is never fuzzed, intact or damaged
(low).** `fuzz/fuzz_postings.cc:142` never sets `w.permuted`, and its seed check covers single
terms only. `probe_corrupt` fills the gap for this review. It flips every bit of two terms' skip
headers, level 1, and advance entries, in a permuted segment and a docid-ordered one, and reseals
each copy (11,984 flips). Each copy then runs seeded and unseeded `top_k` at k 10, 100, and 1,000
and a two-segment `search()`. 11,018 fenced, 966 answered, and none produced an ordinal past the
count, a repeated ordinal, or a repeated docid. That holds under ASan+UBSan too. Recommended: a
shape bit in `fuzz_postings` for a permuted segment, plus a seeded union.

**FQ6. One defensive check has no test, and without it the reader shifts by 64 (low).**
`load_group` refuses a group with fewer block bytes than blocks (`reader.hh:1489`, `gr.blocks_end
- gr.blocks_begin < gr.n`). Without that check, a group with zero bytes derives `wo =
bit_width(2⁶⁴ − 1) = 64` (`reader.hh:1493`), and `get_bits` evaluates `1 << 64` (`format.hh:469`).
The mutant that removes the check survives `postings_test`, `probe_seeds`, and `probe_corrupt`,
including under ASan+UBSan, because no single bit flip makes two groups' block offsets equal.
Recommended: a damaged case with group g+1's blocks offset equal to group g's. The two other
surviving mutants are known: `m3t-stop-lt` (the ledger states it changes no result) and the
equivalent ones (memo off, memo across groups, M3t off, seeds off).

**FQ7. `search()` now keeps every segment's scorer alive for the whole query (low; for the
many-indexes workload).** Before, it built one segment's scorer at a time. It now builds all of
them first, to find the floor (`searcher.hh:205-215`). Per term a scorer is 5,856 bytes on the
branch against 6,648 on main (`probe_sizeof`). So a 4-term query over 32 segments holds about
750 KB at once where it held about 23 KB. That is not a correctness issue. If it matters under the
many-indexes rows, the floor can be found in a first pass that keeps only `seed(k)`.

**FQ8. The ledger has no numbered done criteria (process, low).** `docs/status/f13-query.md` has
items and "Done" bullets with evidence, but no K-numbered criteria to tick, as the pipeline asks.
The freeze criteria live in format13's ledger (F1 items 2 to 8). It is for the lead to say which of
those this branch closes.

**FQ9. Nits.** `test/index_test.cc:974` lowered the merge rooms to 150,000 for M22's smaller
Elias-Fano block, and they stayed lowered after M22 left. The ledger's reason no longer holds;
the test still passes and covers more rooms. The ledger's "T8 hash" is `reorder_test.cc:311`'s
pin, which that test calls T7.

## What I checked, item by item

1. **Decoders.**
   - `decode_gaps` (`format.hh:498-525`), by hand: at header width ≤ 25 the total of (gap + 1)
     over 128 gaps is at most 2³², so matching the last value modulo 2³² forces a 64-bit match
     given last > base. The constant form is checked as (v+1)·128 = span in 64 bits. The wide form
     checks the 64-bit sum. Exception headers and widths above 32 are refused.
   - `block_at` (`reader.hh:1521-1535`) checks last ≤ the group's last, last > base, and
     0 ≤ start < end ≤ the group's bytes. `load_group` checks the group's fields and the bounds of
     its advance entries, and `need()`s them. The seeds sit in the 128 verified header bytes; a
     valid header is at most 104 bytes.
   - Codec code 2 is refused by the width check: `codec-width-2` fails `postings_test`.
   - Fuzzing, 10 minutes each under ASan+UBSan with libFuzzer, from the checked-in corpora:
     `fuzz_postings` 2,743 runs and `fuzz_segment` 1,189 runs (at about 1 exec/s it mostly
     replayed its corpus), no crash. Corpus replays pass in both builds.
2. **Correctness.**
   - Identity and verify at 100k on the tip (FQ1, FQ2).
   - Seeds: `probe_seeds` part 1 swept k = 1 to 1,200 over six terms, four segments, both
     presets, and wqf 1 to 3, with wdfs past 2¹⁶ (the stable_sort path), lengths 0 to 2³² − 1
     across every code, and mixed zero wdfs. Every seed was strictly below the true k-th score,
     and 0 past the largest stored k.
   - Seeds are off with deletes: the `seed-with-deletes` mutant fails `postings_test` and
     `probe_seeds`.
   - M3t runs only when the collector's current segment is reordered (`search.hh:344`, through
     `top_collector::reordered()`). `live_scorer` does not forward `collect_top_k`, so a deleted
     segment takes the generic loop.
   - The last-checked-block cache: equivalent to recomputing for valid data, since the
     predecessor's end is the entry's offset and its last the entry's previous last. It needs no
     invalidation, because the iterator's bytes are immutable and nothing re-targets an iterator.
     `memo-stale`, which lets the cache serve any later block, fails `postings_test` (SIGABRT) and
     `probe_seeds`. `memo-off` and `memo-across-groups` are equivalent.
3. **The writer.**
   - The bitset boundary equals main's rule exactly: `gaps_bytes` is `encode_for`'s size over
     plain gaps, `prev` follows the same sequence, and `encode_for` never emits exceptions.
     `boundary-gaps-minus-1` fails `postings_test`.
   - Every byte-changing commit bumped `pre_freeze` with its fixtures (`88f23ee` 02, `b9a30d4`
     03, `c21ae56` 04, `07bf37d` 05, `03c79cc` 06, `6d2e642` 07). Commits that change no bytes
     did not bump it.
   - Each byte change also re-pinned `pre_freeze.txt`, `format13_test`'s prefix CRC
     (`0x8427a464`), and `reorder_test`'s hash. `test/fixtures/digest.txt` is unchanged against
     main.
   - The M20 writer's cost (FQ4).
4. **The F10 deviations:** FQ3.
5. **Revert table** (`.scratch/mutants/results.txt`; each mutant is a copy of `include/` with one
   exact edit, with `postings_test` and the probes built against it). All of the developer's
   claimed reverts fail as stated: entry against the group's last, flags bits, codec width 2,
   64-bit sum, last check (`postings_test:330`), PFOR refusal, constant check (SIGSEGV), seed
   before the live wrapper, margin, union sum, `check()`'s seed verification, the old boundary,
   M3t's tail and its 1.5× stop. Mutants I added also fail: `seed-k-mapping`, `writer-code-low`,
   `writer-stop-early`, `m3t-block-skip-1.5x`, and `memo-stale`. The survivors are in FQ6.

Suite results:
- RelWithDebInfo (Apple clang): `ctest` 39/39 in 123 s.
- ASan+UBSan (Homebrew clang 23, `-O1`, Debug): `ctest` 39/39 in 1,319 s.
- `probe_seeds` and `probe_corrupt` pass under ASan+UBSan.
- CI: green on every platform at `6d2e642` (run 39338537068), the last commit with code;
  `bc00f08` is docs only, which `paths-ignore` skips.

## Probes (all in `.scratch/`)

- `probes/cc.sh`: compiles a probe against a tree and its build (`asan` for ASan+UBSan).
- `probes/probe_search.cc`: `dump`/`verify` without a count over an index. It is built for the
  tip (`probe_search_rel`) and for main (`probe_search_main`) to compare byte for byte.
- `probes/probe_seeds.cc`: seeds against true k-th scores, and the mixed-snapshot search sweep.
- `probes/probe_corrupt.cc`: every-bit damage of the skip header, level 1, and advance entries,
  with M3t, seeded top_k, and two-segment search.
- `probes/probe_sizeof.cc`: scorer sizes on the branch and on main.
- `idx/build.sh`, `idx/{main,tip}-{upd,merged,reordered}`: the 100k indexes. The dumps are
  `idx/dump-*.txt` and `idx/nc-*.txt`, and the verify outputs `idx/verify-tip-*.out`.
- `corpus/`: the 100k corpus, the update variant, and the 1,162 queries.
- `mutants/mutate.py`, `mutants/results.txt`, `mutants/<name>/`: 25 mutants with their logs.
- `fuzz/{postings,segment}.log` and the corpora they grew. `fuzz/artifacts/` is empty.
- `wt-main/`: main at `d058cd9`, release `strata_engine`.
- `ctest.log`, `ctest-asan.log`, and the build and configure logs.

## Blocking items

None in the code.

## Conditions

- **C1** (FQ1, FQ2): before landing, or at the gate VM run the lead already plans, rerun result
  identity against main and `verify` at the tip, on the 1M or 5M merged and reordered indexes,
  with a no-count pass (this review's `probe_search`, or the same mode added to `strata_engine`).
- **C2** (FQ3): the architect rules on FQ3 items 1, 2, 4, 5, and 6 before the freeze commit. These
  bytes freeze with format 13, and item 1 departs from F13-R8's ruling. Item 3's wording in the
  design is the architect's.
- **C3** (FQ4): the gate VM's 5M build records the docid and reordering merge times for main and
  the branch, interleaved, so M20's writer cost has a figure.
- Follow-ups for the next hardening pass, not blocking: FQ5's permuted fuzz shape, FQ6's damaged
  case, FQ7's floor pass if the many-indexes rows show it, FQ8 and FQ9.

## Not run

- Any 1M or 5M run, and any timing, A/B, or benchmark.
- Parity against the Xapian oracle (its rank vectors are on the gate VM).
- Linux, FreeBSD, and arm64 builds (CI covers them; it is green at `6d2e642`).
- TSan.
- Fuzzing past 10 minutes per target.
- Re-measurement of the bytes tables in `format13-query-items.md`.
- I did not open the developer's indexes; I read only their version words and logs.
