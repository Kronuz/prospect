# Phase 0 baselines

Provisional: these runs shared the host with other builds. Every row is re-run on a
quiet host before it gates anything, as [BENCHMARKS.md](../../docs/BENCHMARKS.md)
requires.

## Environment

| Item | Value |
|---|---|
| Host | Intel i9-9980HK, 16 threads, 32 GB, macOS 15 |
| VM | QEMU 9 with HVF, `-cpu host`, 8 vCPUs, 16 GB, virtio disk (`cache=none`) on APFS |
| Guest | Ubuntu 24.04, kernel 6.8, GCC 13.3, Clang 18.1, OpenJDK 21.0.12, Rust 1.99 |
| In-VM synchronous write | 4.1 ms per 4 KiB `O_DSYNC` write (2,000 writes) |
| Corpus | search-benchmark-game English Wikipedia: 5,032,104 documents, 8.33 GB JSON |
| Harness | quickwit-oss/search-benchmark-game `a7c7547` |

## Indexing

Each engine indexes the corpus into a single merged index from a dropped page cache.
Analysis is whitespace tokenization of pre-normalized text with positions; Xapian
drops 2 tokens longer than its 245-byte term limit, as the other engines drop
over-long tokens.

| Engine | Writer threads | Wall time | Docs/s | CPU used | Peak RSS | Index size |
|---|---:|---:|---:|---:|---:|---:|
| Tantivy 0.26 | 4 | 5:58.6 | 14,033 | 2.49 cores | 4.86 GiB | 2.82 GiB |
| Lucene 10.4.0 | harness default | 5:56.0 | 14,135 | 5.35 cores | 6.83 GiB | 2.61 GiB |
| Xapian 2.1.0 (glass) | 1 | 1:35:09 | 881 | 0.90 cores | 0.49 GiB | 14.38 GiB |

Wall time includes the final merge (Tantivy, Lucene) or compaction (Xapian). Xapian's
index stores per-document term lists, which the other two do not; that accounts for
part of its size.

## Querying

The harness's 962 queries, one at a time over a pipe, after a 60 s warm-up, 10 runs
each; a query's time is its fastest run. Raw results are in `query.json`. Times are
microseconds and include the pipe round trip, which is the same for every engine.

| Command | Tantivy 0.26 mean / p99 | Lucene 10.4.0 mean / p99 | Xapian 2.1.0 mean / p99 |
|---|---:|---:|---:|
| `TOP_10` | 1,694 / 14,877 | 1,670 / 10,626 | 11,505 / 61,085 |
| `TOP_100` | 2,046 / 17,262 | 2,191 / 14,005 | 14,810 / 75,168 |
| `TOP_1000` | 3,278 / 23,361 | 3,092 / 22,444 | 18,352 / 93,143 |
| `TOP_100_COUNT` | 3,171 / 34,307 | 5,348 / 63,590 | 29,925 / 260,872 |
| `COUNT` | 1,580 / 17,934 | 1,463 / 13,354 | 27,770 / 244,836 |

Lucene's counts equal Tantivy's on every query. Xapian's differ on 19 queries with
excluded terms (`-word`), which this run's adapter searched as literal terms; the
adapter now maps them to `AND_NOT`, and the Xapian rows are re-run with the quiet-host
pass. The other 943 queries agree.

On this corpus, Xapian is 6–19× slower than Lucene and Tantivy on every command, so
the gates against Lucene and Tantivy are the binding ones for reads as well as writes.

## Xapiand

Xapiand `master` (`361a2ebf6`, RelWithDebInfo, Clang 18) in the gate VM, one node
(`--solo`), index `wiki` built from the first 100,000 corpus documents
(`xapiand_phase0.py convert`), 307 MB on disk.

### RESTORE profile

RESTORE of the 100,000 documents runs at 964 docs/s unprofiled and 954 docs/s under
`perf record -F 99 --call-graph dwarf` (25,572 samples). Inclusive shares of all
samples:

| Where | Share |
|---|---:|
| Xapian `replace_document` (glass inverter included) | 28.2% |
| Per-document shard check-in (`DatabasePool::checkin`, `pthread_cond_signal`, kernel futex) | 25.4% |
| Per-document `DatabaseWALWriter::delete_wal` enqueue | 14.6% |
| Document preparation (`DocPreparer`: schema, serialization) | 10.2% |
| Per-document shard checkout (`lock_shard`) | 7.7% |

Xapian accounts for 28% of RESTORE, so an infinitely fast engine bounds Xapiand's
RESTORE speedup at 1.39×. The largest costs are host per-document synchronization:
each document checks a shard out and back in, signaling a condition variable, and
enqueues a WAL deletion. The ingestion gap with Elasticsearch is mostly Xapiand's
own pipeline; strata's write path is justified by the engine-level indexing rows,
and the host costs are fixed in Xapiand.

The mechanism (`DatabaseHandler::replace_document_term`, `src/database/handler.cc`):
on a multi-shard index, every document first looks its id term up in every shard
(`get_docid_term`: lock the shard, open the term's posting list, release), then locks
its target shard again to write. Concurrent `DocIndexer` threads convoy on those shard
locks, and each release wakes a waiter through a futex. Two host fixes follow, both
Xapiand decisions rather than engine work:

1. Look a non-numeric id up only in the shard its hash selects, which is the only shard
   `replace_document_term` ever writes it to while the shard count is unchanged.
2. Skip the per-document `delete_wal` enqueue on the RESTORE path, which runs with the
   WAL disabled.

### Offload hop

`SearchService::should_offload` sends every search to the worker pool. A VM-only build
flag (`XAPIAND_INLINE`, not committed) runs searches on the reactor instead. Sequential
full-text searches over 900 benchmark queries, two interleaved rounds each:

| Mode | Mean | p50 | p99 |
|---|---:|---:|---:|
| Offloaded (current) | 2,375 µs | 2,192 µs | 5,211 µs |
| Inline | 2,028 µs | 1,899 µs | 4,919 µs |

The hop costs about 350 µs per search, 15% of its latency.

### Light queries under heavy load

Eight clients run broad 4-term disjunctions with exact counts for 30 s while one client
fetches documents by id:

| Mode | Light unloaded p50 / p99 | Light under load p50 / p99 | Heavy queries done |
|---|---:|---:|---:|
| Offloaded (current) | 609 / 1,540 µs | 850 / 7,219 µs | 9,050 |
| Inline | 432 / 1,149 µs | 703 / 3,020 µs | 6,239 |

The Phase 6 baseline is the offloaded row: light-query p99 of 7.2 ms under heavy load.
Inline serves light queries faster but completes 31% fewer heavy queries, so the two
rows do not compare at equal throughput.

## Reproduce

```sh
tools/vm/linux.sh create BASE_IMG && tools/vm/linux.sh start
# in the VM: clone search-benchmark-game, `make corpus`, then
bench/phase0/index_all.sh tantivy-0.26 lucene-10.4.0 xapian-2.1.0
make bench ENGINES="tantivy-0.26 lucene-10.4.0 xapian-2.1.0"
```

The Xapian engine adapter is GPL and lives outside this repository (`strata-oracle`).
