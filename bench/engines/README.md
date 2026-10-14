# Baseline engines

The baselines strata measures against, as engines of
[search-benchmark-game](https://github.com/quickwit-oss/search-benchmark-game) (MIT):

- `tantivy-0.26-st`, `lucene-10.4.0-st`: the harness's Tantivy 0.26 and Lucene 10.4.0
  engines with one indexing thread, the gate's configuration (BENCHMARKS.md).
- `xapian-2.1.0`: Xapian 2.1.0 through its public API, built against a local
  `xapian-core` install (`XAPIAN_PREFIX`, default `~/opt/xapian-2.1.0`).

Each engine also has a `churn` mode that reads the comparative soak's op stream
(`bench/soak/soak.py`). strata's own engine is `bench/phase2/engine`.

`install.sh HARNESS` copies these and strata's engine into a harness checkout's `engines/`.
