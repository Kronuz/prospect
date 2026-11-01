# prospect

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/prospect/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/prospect/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Embeddable search library for C++20: block-max pruning over compressed postings, BM25 scoring, and coroutine queries.
Its name stands for Pruned Ranking Over Scored Postings with Early-terminating
Cursor Traversal. prospect is the search layout of [strata](https://github.com/Kronuz/strata),
the segment core it builds on: strata keeps the index, its segments, manifests,
merges, and crash-safe commits, and prospect adds terms, postings, positions,
scoring, queries, and aggregations. Together they are built to replace Xapian
inside [Xapiand](https://github.com/Kronuz/Xapiand) and its successor, Elysian.

## ✨ Highlights

- **Block-max pruning** over compressed postings, scored with BM25 in Lucene's or
  Xapian's parameterization.
- **Coroutine queries** on the caller's reactor: no thread of its own, no blocking
  reads.
- **Reordering at merge.** Recursive graph bisection over the postings makes merged
  segments compress smaller and prune faster.
- **Structured data.** Doc values, aggregations, sort keys, and collapse run over
  the same segments.
- **On strata's core.** Immutable segments with crash-safe commits, verified
  against power loss, disk-full, and media faults; prospect plugs its sections
  into strata's layout and never re-implements the lifecycle.
- **Ratcheted against Lucene, Tantivy, and Xapian.** Each phase must match or beat
  them on speed, memory, and size before the next begins.

## 🚀 Quick start

```cpp
#include "completion/posix.h"
#include "prospect/index.hh"
#include "prospect/searcher.hh"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

int main() {
	const auto dir = std::filesystem::temp_directory_path() / "prospect-quick-start";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	kronuz::journal::PosixIO io(dir);
	auto ix = prospect::index::create(io, {}, {'q', 'u', 'i', 'c', 'k'});

	for (const char* text : {"the quick red fox", "a red fox and a red hen", "the lazy dog"}) {
		prospect::operation op;
		op.doc.field().text = text;
		op.doc.field().split_text = true;  // the words between spaces are the terms
		prospect::transaction tx;
		tx.fields = {{"text", strata::field_role::indexed, strata::field_postings::positions, false}};
		tx.ops.push_back(op);
		ix.apply(tx, {});
	}
	ix.commit();

	std::vector<prospect::segment_snapshot> segs;
	for (const auto& s : ix.segments()) segs.push_back({s.seg(), s.deleted, s.dead, &ix.fields()});
	prospect::term_table terms(segs);
	const auto weight = prospect::weight::bm25(terms.stats(), terms.df("text", "red"));
	auto red = [&](const prospect::segment& seg) -> prospect::scorer_ptr {
		const auto* info = terms.info(seg, "text", "red");
		if (!info) return nullptr;
		return std::make_unique<prospect::term_scorer>(seg.get<prospect::inverted_plugin>(), *info, weight);
	};
	for (const auto& hit : prospect::search(segs, red, 10))
		std::printf("doc %llu: %.3f\n", (unsigned long long)hit.docid, hit.score);  // doc 2: 0.258, doc 1: 0.227
	ix.close();
}
```

`test/index_test.cc` covers indexing, commit, and recovery;
`test/search_test.cc` covers the scorers; `test/multisegment_test.cc` covers
search across segments.

## 📦 Installation

Requires a C++20 compiler and CMake 3.18 or newer; it fetches strata (with the
libraries it builds on) and six sibling libraries of its own (task, docset,
bitpack, unicode, snowball, analysis). CI builds it on Linux (x86_64, arm64),
macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(prospect
  GIT_REPOSITORY https://github.com/Kronuz/prospect.git
  GIT_TAG        c77d6ebda1165fc72e50b631232a2f7494d77d46)  # or main for the tip
FetchContent_MakeAvailable(prospect)

target_link_libraries(my_app PRIVATE prospect::prospect)
```

```cpp
#include "prospect/index.hh"
#include "prospect/searcher.hh"
```

## 🎯 When to use it

Use it to follow or contribute to the engine, or to embed it experimentally.

Not for:

- **A stable API or format.** The API changes between phases;
  [docs/STATUS.md](docs/STATUS.md) says where the phases stand.
- **Production search.** Use Xapiand on its Xapian core, Lucene, or Tantivy until
  the query rows pass their gate.
- **A key-value store with no search.** Use strata's core alone (`strata::core`).

## 📚 Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): the search layout on strata's core, the
  search sections, and the read path.
- [docs/STATUS.md](docs/STATUS.md): where the phases stand.
- [docs/testing/parity.md](docs/testing/parity.md): the test-parity inventory against Lucene,
  Tantivy, and Xapian.
- [docs/perf/](docs/perf): one record per search optimization, with its measurements.
- strata's [ARCHITECTURE](https://github.com/Kronuz/strata/blob/main/docs/ARCHITECTURE.md),
  [PLAN](https://github.com/Kronuz/strata/blob/main/docs/PLAN.md),
  [BENCHMARKS](https://github.com/Kronuz/strata/blob/main/docs/BENCHMARKS.md), and
  [TESTING](https://github.com/Kronuz/strata/blob/main/docs/TESTING.md): what the two share.
- [AGENTS.md](AGENTS.md): repository map and working rules.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries): the catalog of the
  building blocks.

## 🛠️ Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## 🤝 Contributing

Read [AGENTS.md](AGENTS.md) first: prospect is a clean-room implementation, and every
change goes through its phase gates. CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE). The pruning of
`required_optional_scorer` is ported from Apache Lucene under the Apache License 2.0; see
[NOTICE](NOTICE).
