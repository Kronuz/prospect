# prospect

[![CI](https://github.com/Kronuz/prospect/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/prospect/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Status: in development](https://img.shields.io/badge/status-in%20development-orange.svg)

An embeddable, MIT-licensed search library in C++20: Pruned Ranking Over Scored Postings with
Early-terminating Cursor Traversal. prospect is the search layout of
[strata](https://github.com/Kronuz/strata), the segment core it builds on: strata keeps the
index, its segments, manifests, merges, and crash-safe commits; prospect adds terms, postings,
positions, scoring, queries, and aggregations. Together they replace Xapian inside
[Xapiand](https://github.com/Kronuz/Xapiand) on the way to Elysian, its successor.

## ✨ Highlights

- ⚡ **Block-max pruning** over compressed postings, scored with BM25 in Lucene's or
  Xapian's parameterization.
- 🧵 **Coroutine queries** on the caller's reactor: no thread of its own, no blocking
  reads.
- 🔀 **Reordering at merge**: recursive graph bisection over the postings, so merged
  segments compress smaller and prune faster.
- 📊 **Structured data**: doc values, aggregations, sort keys, and collapse over the same
  segments.
- 🧱 **On strata's core**: immutable segments with crash-safe commits, verified against power
  loss, disk-full, and media faults; prospect plugs its sections into strata's layout and
  never re-implements the lifecycle.
- 📏 **Ratcheted against Lucene, Tantivy, and Xapian**: each phase must match or beat
  them on speed, memory, and size before the next begins.

## 🚧 Status

prospect is under active development and its API changes between phases; it split out of
strata on 2026-10-20 with its history. Phases 0 through 2 have passed their gates, and Phase 3's
query rows are carried while Phase 4 proceeds. Queries don't yet meet the ≤ 1.0× target. strata's
[docs/PLAN.md](https://github.com/Kronuz/strata/blob/main/docs/PLAN.md) has the phases, and
[docs/gates/](docs/gates) has the search rows.

## 📦 Installation

Requires a C++20 compiler and CMake 3.18 or newer; strata and the libraries it builds on are
fetched at configure time. CI builds it on Linux (x86_64, arm64), macOS (arm64, x86_64), and
FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(prospect
  GIT_REPOSITORY https://github.com/Kronuz/prospect.git
  GIT_TAG        main)
FetchContent_MakeAvailable(prospect)

target_link_libraries(my_app PRIVATE prospect::prospect)
```

## 🚀 Quick start

```sh
git clone https://github.com/Kronuz/prospect.git && cd prospect
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build
```

The tests are the current usage examples: `test/index_test.cc` covers indexing, commit,
and recovery; `test/search_test.cc` covers the scorers; `test/multisegment_test.cc`
covers search across segments.

## 🎯 When to use it

- ✅ Following or contributing to the engine, or embedding it experimentally.
- ❌ Production search today: use Xapiand on its Xapian core, Lucene, or Tantivy until
  the query rows pass their gate.
- ❌ A key-value store with no search: use strata's core alone (`strata::core`).

## 📚 Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): the search layout on strata's core, the
  search sections, and the read path.
- [AGENTS.md](AGENTS.md): repository map and working rules.
- [docs/testing/parity.md](docs/testing/parity.md): the test-parity inventory against Lucene,
  Tantivy, and Xapian.
- [docs/perf/](docs/perf): one record per search optimization, with its before and after.
- strata's [ARCHITECTURE](https://github.com/Kronuz/strata/blob/main/docs/ARCHITECTURE.md),
  [PLAN](https://github.com/Kronuz/strata/blob/main/docs/PLAN.md),
  [BENCHMARKS](https://github.com/Kronuz/strata/blob/main/docs/BENCHMARKS.md), and
  [TESTING](https://github.com/Kronuz/strata/blob/main/docs/TESTING.md): what the two share.

## 🤝 Contributing

Read [AGENTS.md](AGENTS.md) first: prospect is a clean-room implementation, and every
change goes through its phase gates. CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE). The pruning of
`required_optional_scorer` is ported from Apache Lucene under the Apache License 2.0; see
[NOTICE](NOTICE).
