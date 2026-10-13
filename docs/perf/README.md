# strata performance records

One file per Track P item (PLAN.md): what it changes, the class it targets, and its
before/after table on the gate VM. In-process numbers come from `strata_engine bench`
on the 5M Wikipedia index, interleaved A/B over three rounds with each query's best
time (`abcmp.sh` in the gate VM's scratch directory), and must keep `verify` clean.
Gate numbers come from the harness under `bench/phase2/g5.sh`.

| Item | Change | Class | Result |
|---|---|---|---|
| [P7](p7-membership-probes.md) | Counting windows probe dense terms for few candidates | `COUNT` intersection | 0.964× geometric mean, stopword intersections 0.54–0.60× |
| [P6a](p6a-conjunction-bulk-topk.md) | Bulk top-k for conjunctions of terms, no virtual call per candidate | `TOP_K` intersection | 0.88–0.90× geometric mean |
