# Code review: perf/phrases (d5e5792 against main d6f15d6)

Reviewer: code reviewer, fresh context. Scope: `include/prospect/reader.hh` and
`include/prospect/search.hh` (94 added lines; the rest of the branch is docs and `bench/quick`).
`docs/reviews/calibration.md` does not exist in prospect; nothing to start from there.

## Verdict: APPROVE WITH CONDITIONS

The four changes (E1 recycling, E2 `pair_match`, E3 `can_compete`, E5 prefix sum) are correct as
far as I could drive them: no stale state crosses queries, the per-thread stack is bounded and
freed at thread exit, `can_compete` never dropped a hit in 2M+ oracle checks including ties,
floors and 2 to 5 slot phrases, and the E5 test is equivalent to the old one on 20M adversarial
cases. One real defect (CR1: the stack hides use-after-free from ASan, exactly on the three
objects with the most pointer state) and one gap (CR2: no test pins `recycled`). Both are small.

## Findings

### CR1 (condition): `recycled` blinds AddressSanitizer on the three types it covers

`reader.hh:582-615`. A destroyed `postings_iterator`, `positions_reader` or `phrase_scorer` goes
onto the stack without being poisoned, so a later read through a dangling pointer to it is not
reported. Probe `.scratch/probes/recycle_probe.cc uaf` (ASan+UBSan build): the read of a deleted
recycled `postings_iterator` prints "went undetected"; the control (a plain 4.4 KB heap object,
same pattern) aborts with the ASan report. The suite's ASan runs, the libFuzzer targets and
`fuzz_*_replay` (which walk these types: `fuzz_postings.cc` builds iterators and readers per
input) all rely on ASan for the lifetime bugs these objects are most likely to have (they hold
raw pointers into a mapped segment and into each other).
Fix: under `__SANITIZE_ADDRESS__` / `__has_feature(address_sanitizer)` (and MSan) make `new`/`delete`
pass straight to `::operator new`/`delete`, or keep recycling and call `ASAN_POISON_MEMORY_REGION`
on stash and `ASAN_UNPOISON_MEMORY_REGION` on take. The bypass is two lines and keeps release
performance. Condition for landing.

### CR2 (condition): no test covers `recycled`

The branch adds no test. The existing `search_test`, `multisegment_test`, `engine_query_test` and
`concurrency_test` exercise it implicitly (all 39 ctest targets pass under ASan+UBSan), but
nothing pins the properties the ledger claims: the 8-block cap, release at thread exit, a block
taken after an exception thrown by a constructor, cross-thread free. Fix: add
`test/recycled_test.cc` from `.scratch/probes/recycle_probe.cc` (it measures with
`__sanitizer_get_current_allocated_bytes`; on a non-ASan build count with a replaced
`operator new`, sizes tagged). Condition for landing; ten minutes.

### CR3 (note, non-blocking): a delete after the thread's stack is destroyed leaks up to 8 blocks

`reader.hh:611`. `static thread_local stack` has a destructor; an object destroyed by a later
thread-local or static destructor on the same thread pushes into the dead stack
(count is 0 after the destructor, so `new` falls through; `delete` stores up to 8 blocks nobody
frees). Nothing in `include/` does this today (the only other `thread_local` is `reader.hh:177`,
a vector of string_views). Not reachable now; a comment on `cache()` or a `bool dead` flag in the
destructor closes it.

### CR4 (note, non-blocking): the 66 KB claim holds, the mechanism is per thread and per type

Probe `recycle_probe`: `sizeof` 4424 + 1192 + 2568, times 8 = 65,472 B per thread. Retained after
100 iterators are freed on one thread: 8 blocks (36,416 B measured, 8 x 4,424 plus allocator
overhead). 400 short-lived threads each building and dropping queries: allocated-bytes delta 0
after join (no leak at thread exit). 1,000 iterators created on one thread and freed on another:
all released (the freeing thread keeps 8, freed at its exit). Blocks hold no reader pointer (the
destructor has run), so a closed reader or the many-indexes shape (thousands of readers per
thread) does not extend any lifetime; the cost is 66 KB per thread and not per reader. With
thousands of OS threads that is tens of MB, which the ledger's "per thread" wording covers.

## Checked and clean

- Stale state (E1). Every member of the three types is initialized by its constructor or default
  member initializer (`= {}` arrays `counts_buf_`, `block_buf_`, `buf_ords_`, `bits_`, `word_sum_`,
  `buf_wdfs_`), buffers `out_` and `tail_values_` are members destroyed with the object. Mutant
  `.scratch/mut-poison` fills every block with 0xA5 both when stashed and when freshly allocated;
  `search_test`, `multisegment_test`, `pruning_test`, `cost_test` and `phrase_diff` pass against
  it (a bug in this class would be a failure). Alignment: `static_assert(alignof <= 16)` for the
  three types holds (probe). Sized `operator delete` is selected by `new`-expression, by the
  deleting destructor through `scorer*`, and when a constructor throws; no derived class of the
  three types exists.
- `can_compete` (E3), Lucene mode: the phrase frequency is at most every term's position count
  (a repeated term needs one position per slot, so at most its count), `by_norm` is increasing in
  tf, `sum_upper(.,1)` adds rounding slack, skip test is `<= min_` as in `find`'s block skip; the
  document's `norm(d)` is the one `score()` uses. Xapian mode: the score is exact (same expression
  as `score()`), so skipping on `<= min_` is exact. A term without positions gives count 0 and
  the phrase cannot match anyway. Counted queries never set `min_` (`top_k` sets it only when
  `!count`), and the phrase has no `fresh()`, so `counts_apart()` is false and counts stay exact.
  Probe `.scratch/probes/phrase_diff.cc` (seeds 1, 2, 3, 7; 8 corpora of 400 and 6,000 documents;
  copies of earlier documents for exact score ties; identical lengths; documents of up to 530
  positions; wdf equal to and twice the position count; phrases of 2 to 5 slots with repeats, half
  taken from real documents; both schemes; k of 1, 2, 5, 10, 50; floors; counted): about 2,000,000
  checks, 0 failures against a brute-force oracle with ties by ordinal. The probe's strength:
  two invalid `can_compete` mutants (skip when the smallest count is 1; Xapian score from the first
  slot only) fail both `search_test` and `phrase_diff` (`.scratch/mut-m2-ge`, `.scratch/mut-m3-xapian-first-slot`).
  A third mutant (bound by the first term's count) survives, correctly: it is a valid, looser bound.
- `pair_match` (E2): same lead choice as the generic path (the second slot leads only when strictly
  smaller, as the stable insertion sort did), same forward-only cursor, same early `return 0`;
  a phrase of one term twice reads one cached span for both slots, so no read invalidates `pos`.
- E5: `add` holds at most 128 counts below 2^32 (below 2^39), so it cannot wrap; wrap of
  `sum_ + add` is detected exactly when the old per-step test would have fired (all addends are
  non-negative). Probe `.scratch/probes/e5_equiv.cc`: 20,000,000 random and boundary cases
  (starts near 2^64, counts of 0xffffffff, carries landing on 2^64), 832,765 raised, 0 mismatches
  with the old loop. State after a raise differs (`sum_pos_` stays put) but the iterator is
  not reusable after `corrupt` either way. `fuzz_postings_replay` (resealed byte flips through
  `positions_reader::read`) passes under ASan+UBSan.
- `tail_values_.reserve(min(block, end_ - tail_))`: `end_ - tail_` is validated at construction
  (`blocks_bytes_ <= end_ - p`); a corrupt tail longer than a block just grows the vector.
- Scope: two headers plus docs and bench scripts; no format or disk change, no environment
  switches left in the headers (`PHR_*` and `getenv` absent).

## Probes and mutants (all in `.scratch/` of this worktree)

- `.scratch/build-asan.sh`, `build-asan/`: Homebrew LLVM 22, ASan+UBSan, RelWithDebInfo; deps from
  the contributor's `build-rel/_deps` (read only). `ctest -j3`: 39 of 39 pass
  (`ctest-asan.log`, 643 s).
- `.scratch/mkprobe.sh SRC OUT [mutant include dir]`: compiles a probe with the build's flags.
- `.scratch/probes/phrase_diff.cc`, `recycle_probe.cc`, `e5_equiv.cc` (+ built binaries).
- `.scratch/mut-poison/`, `mut-m1-first-term-count/`, `mut-m2-ge/`, `mut-m3-xapian-first-slot/`
  (header copies plus `bin/`).

## For the architect

Nothing blocking. One design remark: the landing target (at most 1.0 of Tantivy) is not met
(`COUNT` 1.348, `TOP_10` 1.116 on the Intel Mac); that is a perf-gate matter, not a correctness
one. `recycled` is a platform-sensitive change (thread-local with a destructor; allocator
behavior), and the ledger says Linux, FreeBSD and arm64 are unmeasured; `__cxa_thread_atexit`
registration per thread on first use is the only per-thread cost I can see.

## Not run

TSan (the stacks are thread-local with no shared state; `concurrency_test` passed under ASan),
other platforms and arm64, any timing or benchmark, a libFuzzer run (replay corpus only), a
direct output diff against a main build (the oracle probe replaces it), the 1M and 100k index
`dump` identity (the contributor's, not re-run), multi-segment `live_scorer` deletions beyond
`multisegment_test`.
