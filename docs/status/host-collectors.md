> Status: CURRENT (2026-10-27) in progress on `phase4/host-collectors`.

# host-collectors: P4, collectors and one snapshot over several indexes

- Design: strata `docs/design/xapiand-on-strata.md`, step P4 and rows G5, G7, G11; census: Xapiand
  `docs/dev/engine-census.md`; `docs/mechanisms/aggregation-scope.md` (aggregations check every match).
- Branch / worktree: `phase4/host-collectors` at `/Users/kronuz/code/Kronuz/prospect-wt/host-collectors`, from main
  `9de01a1`. Oracle side: `strata-oracle` branch `shard-vectors` at
  `/Users/kronuz/code/Kronuz/strata-oracle-wt/shards` (not merged).
- How it works: `docs/mechanisms/multi-index-search.md`.
- State: implementing.
- Done criteria (set by the lead; contributors tick, never edit):
  - [ ] H1 Identity: a search over several indexes equals a search over one merged index holding the same
    documents at the interleaved docids: same documents, same order, same scores (global statistics), same counts.
  - [ ] H2 Docid interleave matches Xapian's: oracle vectors from `strata-oracle` (`shards`).
  - [ ] H3 Property test over random shard splits (including empty shards and docid gaps).
  - [ ] H4 The exhaustive visitor sees every match exactly once.
  - [ ] H5 A mutant per claim, each caught by a test.
  - [ ] H6 ASan+UBSan and TSan, each in its own build directory with its own dependencies.
  - [ ] H7 CI green on the head.
  - [ ] H8 `docs/mechanisms/multi-index-search.md`.
  - [ ] H9 Smoke A/B of single-index search unchanged (screen only).
- Done: nothing yet.
- Doing: oracle vectors (`shards` command), then `include/prospect/shards.hh`.
- Next: key-then-relevance collector and visitor, doc-length bounds, tests, mutants, sanitizers, note, push.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: not yet.

## Log

- 2026-10-27: branch and ledger created.
