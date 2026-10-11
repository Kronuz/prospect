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

## Reproduce

```sh
tools/vm/linux.sh create BASE_IMG && tools/vm/linux.sh start
# in the VM: clone search-benchmark-game, `make corpus`, then
bench/phase0/index_all.sh tantivy-0.26 lucene-10.4.0 xapian-2.1.0
make bench ENGINES="tantivy-0.26 lucene-10.4.0 xapian-2.1.0"
```

The Xapian engine adapter is GPL and lives outside this repository (`strata-oracle`).
