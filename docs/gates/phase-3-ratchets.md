# Phase 3 ratchet rows

Measured rows for Phase 3's gate, with the platform they were run on. Rows that are still
unmeasured are listed at the end.

## Indexing, 1 thread (gate VM, Linux x86_64)

**Superseded (2026-10-15):** these rows ran strata's benchmark engine on Phase 2's
single-segment writer, without keys, versions, flushes, merges, or commits, so they do
not measure T4. The engine now indexes through `strata::index` (`ad16376`, `5dfe415`)
and every engine runs keyed; the rows are being rerun at equal memory
(BENCHMARKS.md, "Indexing speed").

Run on the gate VM with `bench/index_trials.sh p3` and `tools/vm/quiet_trials.sh`, using
the VM's synced source tree as built at 06:48 on 2026-10-14 (Phase 3 lifecycle code; the copy
carries no commit id). The corpus is the 1M-document file from the search-benchmark-game. Each
engine ran three trials, with the engine order rotated and the page cache dropped before
each unit.

| Engine | Wall time (three trials) | Median | Peak RSS | Index bytes |
|---|---|---|---|---|
| strata | 1:32.11, 1:21.97, 1:20.66 | 82.0 s | 1.47 GiB | 609.0 MB |
| Tantivy 0.26 | 1:53.09, 1:38.86, 1:43.81 | 103.8 s | 1.66 GiB | 673.6 MB |
| Lucene 10.4.0 | 2:58.19, 2:45.69, 3:05.11 | 178.2 s | 2.42 GiB | 619.8 MB |
| Xapian 2.1.0 | 16:16.97, 17:51.37, 18:46.31 | 1071.4 s | 0.49 GiB | 3341.1 MB |

Relative to the fastest other engine:

1. **Time:** 0.79× Tantivy's median, so 1.27× faster. Phase 2's gate measured 0.84×
   Tantivy's speed, behind it; this row now passes the 1.0× ratchet.
2. **Peak RSS:** 0.88× Tantivy's and 0.61× Lucene's.
3. **Index bytes:** 0.90× Tantivy's and 0.98× Lucene's, as each indexer commits them.
   The size row after a full merge and per section is still owed (below).

## Still unmeasured

- The same rows on macOS arm64 bare metal and on FreeBSD. They are required before Phase
  3R. The M1 is retired (2026-10-14: its device management keeps turning ssh off), so
  another arm64 machine is owed. One unrotated first round ran there before it was
  retired, after `148eebb` fixed the harness's stdin reading (macOS libc++ reads
  `std::cin` a character at a time, which had made strata's row 236.7 s): strata 26.8 s,
  Tantivy 0.26 34.8 s, Lucene 10.4.0 63.4 s; Xapian did not finish. It is not a gate row.
- Multi-thread and replace-heavy indexing, write amplification, refresh latency.
- Index size after a full merge, per section.
- The query rows on multi-segment indexes, before and after merging, including
  IResearch (which needs Docker on the VM) and PISA as a reference.
