> Status: CURRENT (2026-11-02) workstream: one pool for the Kronuz libraries (developer), prospect's part.

# drop-threadpool (prospect)

- Task: threadpool deletes the `ThreadPool` family; strata repins it; prospect pins that strata.
- Branch: `phase7c/drop-threadpool`, from origin/main `prospect:465ee7d`.
- State: `CMakeLists.txt` pins strata `eadbd52` (strata branch `phase7c/drop-threadpool`: threadpool `db38594`, task `b406766`) in
  place of `501f943`, and task `b406766` in place of `8cad661` (task main; `lock(token)` replaces `lock_until` and `lock_for`,
  which prospect does not name). prospect uses `WorkPool` and `WorkGroup` only; no source changes.

## Done

- Pins moved (this branch's commit).

## Proof

See the commit message for the preflight and CI runs.

## Next

The lead lands strata, then prospect; Xapiand pins both branch heads (local master only).

## Blocked on

Nothing.
