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
    Release); segments, manifests, and meta byte-identical to main's at 20k and 100k.
- Next:
  1. R23's rest: R27's two assertions; `valid_words` and `inverted_builder::add_text` internal
     (R28.2, R28.3).
  2. Duplication: `key_term` through `keys_plugin::key_of` (R29); `key_hash` through
     `wordwise::hash` (R32.1).
  3. C11: the tuple buffer's floor, measured before and after.
  4. Text: L10 and D16 with the code (R30, R31).
- Blocked on: nothing.
