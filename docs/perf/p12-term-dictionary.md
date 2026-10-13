# P12: the term dictionary

Target (PLAN, disk ratchet): the `terms` and `term_index` sections at ≤ 1.0× Lucene's
`.tim`, `.tip`, and `.tmd` (40.2 MB on the 5M Wikipedia corpus) by the Phase 3 gate.

| Step | Commit | Values B/term | Dictionary | Against Lucene |
|---|---|---:|---:|---:|
| Phase 2 gate (format 11) | | 19.16 | 101.6 MB | 2.53× |
| Offsets from restart runs; pairs only with skip blocks; cf elided when df; no maximum wdf | `4e9f8d9` | 7.56 | 55.1 MB | 1.37× |
| df packed with the flags; a single posting's df, cf, and wdf of 1 implied | `d2fe970` (with the region-size change) | 6.27 | 46.3 MB | 1.15× |
| termdict: lengths packed into one byte per entry | termdict `7ac4195` | 6.27 | 46.3 MB | 1.15× |

The last two rows were measured together on one reindex. Queries after the first step,
interleaved against the step before: gate queries 0.97× `TOP_10` and 0.98× `COUNT`,
stratified terms 1.01× and 1.04× (geometric means); `verify` clean.

## Restart interval and block size

`strata_engine dictdump` and `dictsim` rebuild the dictionary from a segment's terms with
other termdict parameters. 5M corpus:

| Restart interval | Block entries | Dictionary | Against Lucene |
|---|---|---:|---:|
| 16 | 25–48 (current) | 46.3 MB | 1.15× |
| 32 | 25–48 | 44.8 MB | 1.11× |
| 32 | 32–64 | 43.8 MB | 1.09× |
| 16 | 32–64 | 46.1 MB | 1.15× |
| 32 | 48–96 | 43.6 MB | 1.08× |
| 64 | 48–96 | 42.8 MB | 1.065× |

Lookups (termdict bench, the system word list, medians of batches of 1,024): 16/25–48
383–422 ns, 32/32–64 438–452 ns, 10–15% slower. Not applied yet: the interval is chosen
together with suffix compression, whose estimate on these keys is 0.73 B/term (deflate
over each block's suffixes; prefix sharing leaves 2.93 B/term of suffix bytes). No
combination measured so far reaches 1.0×; the remaining gap is the per-entry and key
bytes.
