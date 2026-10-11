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

Running. Results land in `query.json` with the harness's commands (`TOP_10`,
`TOP_100`, `TOP_1000`, `TOP_100_COUNT`, `COUNT`) over its 962 queries.

## Reproduce

```sh
tools/vm/linux.sh create BASE_IMG && tools/vm/linux.sh start
# in the VM: clone search-benchmark-game, `make corpus`, then
bench/phase0/index_all.sh tantivy-0.26 lucene-10.4.0 xapian-2.1.0
make bench ENGINES="tantivy-0.26 lucene-10.4.0 xapian-2.1.0"
```

The Xapian engine adapter is GPL and lives outside this repository (`strata-oracle`).
