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
| [P12](p12-term-dictionary.md) | Term dictionary: offsets from runs, packed values and entries | disk (dictionary) | 2.53× to 0.992× Lucene |
| [Reclamation](reclamation.md) | Whole-segment drop at commit, stepped file removal, merges at commit | disk under deletes, foreground stalls | FIFO without merges: 7.5× fewer bytes held; with merges, no change (write amplification 2.0); freeing costs 7–130 ms per GiB across platforms |
| [Commit barriers](commit-barriers.md) | A commit's data files made durable with one batched sync (`IO::sync_files`) | commit latency | commit phase median: macOS 187 → 106 ms, Linux ext4 24.8 → 16.2 ms; FreeBSD unchanged |
| [P15](p15-region-size.md) | Verified regions of 4 KiB instead of 64 KiB | serving memory | peak 677 → 574 MB (with lengths verified on first use; 1.025× Tantivy); queries within a few percent |
| [P6](p6-multisegment.md) | Search over many segments: per-segment costs and the ranked plan (analysis, not yet measured) | every `TOP_K` class, `COUNT` | 16 segments: `TOP_1000` 1.58× over drift, `TOP_100` and `COUNT` 1.18× |
