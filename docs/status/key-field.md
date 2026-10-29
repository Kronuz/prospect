> Status: CURRENT (2026-10-28) F3 of fields-and-keys, prospect side: callers off `key_prefix`. Branch `phase8/key-field` from `phase8/fields` (1c62009), worktree `prospect-wt/f3`; strata's side is `strata-wt/f3`, ledger docs/status/key-field.md there.

# key-field (prospect)

- Design: strata docs/design/fields-and-keys.md (D6, D7, D8, T5). Builds under `quiet light f3p`.

## State

- State: in review (decoder-safety review follows on strata's manifest change); CI green on both branches.
- Done (commits prospect:8d72d2d callers, tests and tools; prospect:d00f682 the pin of strata:d4e43a9; rebased onto prospect main 3aeabbb):
  - prospect already routed the key leaf by the key field's role (F2); here `inverted_plugin::check` and `values_plugin::check` lose the prefix argument, the inverter no longer sets the writer's prefix, and every test and tool uses `keyed`. `git grep key_prefix -- include test tools fuzz bench` is empty.
  - `test/format13_test.cc`: every first byte 0..255 is a key that is also a text term; found by `_id` (the keys) and `text` (the dictionary), through flush, merge, reopen, replace, erase; check().
  - Mutants V and W (`tools/mutants_fields.py`) pick the key leaf by the byte `Q` or `0xff` instead of the role: `format13_test` fails on both.
  - The F20 manifest tests changed with the rule: a manifest whose configured bits lack `keys` is a valid keyless index (a keyed request on it is refused, an install of norms-without-inverted is still refused).
  - `test/fixtures/pre_freeze.txt` takes files 0x80000007; the digest hash `7dfb8e5a57a89fee` and the `.sx` fixtures are unchanged, so only the format bytes moved.
- Proof: Release, ASan+UBSan (LLVM 22, own tree) and TSan (own tree) ctest 49 of 49 each (TSan skips `recycled`, as before). CI: `prospect:d00f682` 8 of 8 jobs green; `strata:d4e43a9` 8 of 8.
- Next: the lead's decoder-safety review, then landing both branches together (prospect's pin names the strata branch head; re-pin to strata main after it lands).
- Blocked on: nothing.

## Log

- 2026-10-28: migrated callers; any-first-byte test; mutants V and W; sanitizers; pushed.
