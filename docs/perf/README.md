# prospect performance records

One file per Track P item (strata's PLAN.md): what it changes, the class it targets, and its
before/after table on the gate VM. In-process numbers come from `strata_engine bench`
on the 5M Wikipedia index, interleaved A/B over three rounds with each query's best
time (`abcmp.sh` in the gate VM's scratch directory), and must keep `verify` clean.
Gate numbers come from the harness under `bench/phase2/g5.sh`. The core's records (reclamation,
commit barriers, P15's region size, the key-value rows against RocksDB) are strata's
[docs/perf](https://github.com/Kronuz/strata/tree/main/docs/perf).

| Item | Change | Class | Result |
|---|---|---|---|
| [P7](p7-membership-probes.md) | Counting windows probe dense terms for few candidates | `COUNT` intersection | 0.964× geometric mean, stopword intersections 0.54–0.60× |
| [P6a](p6a-conjunction-bulk-topk.md) | Bulk top-k for conjunctions of terms, no virtual call per candidate | `TOP_K` intersection | 0.88–0.90× geometric mean |
| [Presets](bm25-presets.md) | `bm25` against `bm25_xapian` as the default | every `TOP_K` class | `bm25` faster: `bm25_xapian` 1.14–1.16× its time |
| [P12](p12-term-dictionary.md) | Term dictionary: offsets from runs, packed values and entries | disk (dictionary) | 2.53× to 0.992× Lucene |
| [P6](p6-multisegment.md) | Search over many segments: per-segment costs and the ranked plan (analysis, not yet measured) | every `TOP_K` class, `COUNT` | 16 segments: `TOP_1000` 1.58× over drift, `TOP_100` and `COUNT` 1.18× |
| [Query gap](query-gap-merged.md) | Union counts through the windowed `count()`; per-query ranking of the merged-index gap | `COUNT` union | 0.932× geometric mean; worst unions 2.8–5.2× faster |
| [Format 13 landing](format13-landing.md) | C16's targeted A/Bs of format 13 on main against format 12 (developer's host, not the gate VM) | T4's guard, T5's lookups and indexing | queries 0.86–1.04×, the merged terms' `TOP_100` 1.021× [1.017, 1.024] on its rerun; key hits 0.71–0.80×, misses equal; indexing 0.95–0.98×, peak RSS 1.056× at 1M |
| [Count union bits](count-union-bits.md) | `fill` sends blocks inside a window straight into its bits: step-1 constant blocks as ranges, summed-mask scatter, entries walked per window (prototype, no format change) | `COUNT` union of more than three terms | 1M reordered: 0.528 (bm25), 0.564 (bm25_xapian); no row lost; union of three 0.884; bitset-biased writer measured a loser |
