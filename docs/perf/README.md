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
| [Presets](bm25-presets.md) | `bm25` against `bm25_xapian` as the default | every `TOP_K` class | `bm25` faster: `bm25_xapian` 1.14–1.16× its time |
| [P12](p12-term-dictionary.md) | Term dictionary: offsets from runs, packed values and entries | disk (dictionary) | 2.53× to 1.15× Lucene; open |
| [P15](p15-region-size.md) | Verified regions of 4 KiB instead of 64 KiB | serving memory | peak 677 → 583 MB (0.86×, 1.04× Tantivy); queries within a few percent |
