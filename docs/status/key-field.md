> Status: CURRENT (2026-10-28) F3 of fields-and-keys, prospect side: callers off `key_prefix`. Branch `phase8/key-field` from `phase8/fields` (1c62009), worktree `prospect-wt/f3`; strata's side is `strata-wt/f3`, ledger docs/status/key-field.md there.

# key-field (prospect)

- Design: strata docs/design/fields-and-keys.md (D6, D7, D8, T5). Builds under `quiet light f3p`.

## State

- State: in progress.
- prospect already selected the key leaf by the field's role (F2: `find_term`, `reader.hh` stage 1); F3 here removes the plugins' `key_prefix` parameter (`inverted_plugin::check`, `values_plugin::check`, the inverter's writer), migrates every `key_prefix` caller in tests and tools to `index_options::keyed`, and pins the strata F3 commit.

## Log

- 2026-10-28: migrated callers; `test/format13_test.cc` gains the any-first-byte test (every byte 0..255 as a key that is also a text term; found by `_id` and `text`, through flush, merge, reopen, replace, erase).
