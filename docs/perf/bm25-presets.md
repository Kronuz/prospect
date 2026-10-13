# The default weighting preset

strata has two presets of one weighting (ARCHITECTURE, "One implementation per
mechanism"): `bm25`, Lucene's and Tantivy's parameters over one-byte quantized lengths,
and `bm25_xapian`, Xapian's BM25Weight over exact lengths. The default is the faster on
the gate corpus.

Gate VM, the 962 gate queries on the 5M Wikipedia index at format 12, in-process,
interleaved over three rounds, best per query; geometric mean of `bm25_xapian`'s time
over `bm25`'s:

| Command | `bm25` total | `bm25_xapian` total | Geometric mean |
|---|---:|---:|---:|
| `TOP_10` | 1,285.6 ms | 1,439.0 ms | 1.163 |
| `TOP_100` | 1,634.7 ms | 1,835.4 ms | 1.163 |
| `TOP_1000` | 2,104.3 ms | 2,401.5 ms | 1.135 |

By class, `bm25_xapian` is slower everywhere (1.03–1.24×; `term` 1.50–2.07× on its one
query, `the`) except the single `two-phase-critic` query (0.89–1.03×). `bm25` stays the
default. A synthetic 200,000-document segment had shown `bm25_xapian` 5× faster on
single-term `TOP_10`; its uniform lengths made Xapian's bounds unusually tight, which is
why the decision rests on the gate corpus.
