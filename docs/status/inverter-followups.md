# inverter-followups

- Design: the architect's "Re-check on main (inverter rebase)" (`docs/reviews/layers-design.md`,
  R26 to R33, C11); the code reviewer's re-check of CR11 to CR13 (`docs/reviews/inverter-main.md`,
  on `review-main-inverter`) folds in when it lands.
- Branch / worktree: `phase4/inverter-followups` at
  `/Users/kronuz/code/Kronuz/strata-wt/inverter-followups`, from `origin/main` at `2b598dd`.
- State: implementing; one commit per fix, each built and tested before it is committed.
- Known bisect gap on main (R26): `2ec8c36` and `1e2d89b` do not build (`index.hh:1832`,
  "use of undeclared identifier 'split_words'"; at `2ec8c36` also `tuple_buffer_test.cc:76`).
  Main's history stays; a bisect over main marks both `git bisect skip`. `888d03c` builds.
  Every commit on this branch builds and passes ctest before it is committed.
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - R28.1, CR9's wrap: `index_options::max_run_bytes` is gone; the run cap is the index's
    private `max_run_bytes_` (`tuple_buffer::run_cap`, 2^32 - 1, by default), which only
    `index_test_access` (defined by `index_test.cc`) lowers, and `drain` asserts the cap is
    at most `run_cap`. No public field can pass it. ctest 35 of 35 (macOS, Apple clang,
    Release); segments, manifests, and meta byte-identical to main's at 20k and 100k
    (`37ee946`).
  - R27 and R28.2-3, R23's rest: `tuple_buffer::add_with`'s debug block asserts all six
    stated preconditions (terms unique, by a sorted copy; each terms entry's positions
    strictly increasing, beside the token clauses). The architect's `probe_preconditions`
    aborts in all three cases with asserts on (case 1 at the uniqueness assert, case 2 at
    the positions assert). `inverted_plugin::valid_words` is private; `inverted_builder::
    add_text` is private, reached only by `segment_writer` (itself through `writer_access`).
    ctest 35 of 35 (Release); `tuple_buffer_test`, `segment_test`, `index_test`, and
    `multisegment_test` pass with asserts on (-O1); bytes identical to main's at 20k, 100k.
- Next:
  1. Duplication: `key_term` through `keys_plugin::key_of` (R29); `key_hash` through
     `wordwise::hash` (R32.1).
  2. C11: the tuple buffer's floor, measured before and after.
  3. Text: L10 and D16 with the code (R30, R31).
- Blocked on: nothing.
