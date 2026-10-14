# P12: the term dictionary

Target (PLAN, disk ratchet): the `terms` and `term_index` sections at ≤ 1.0× Lucene's
`.tim`, `.tip`, and `.tmd` (40.2 MB on the 5M Wikipedia corpus) by the Phase 3 gate.

| Step | Commit | Values B/term | Dictionary | Against Lucene |
|---|---|---:|---:|---:|
| Phase 2 gate (format 11) | | 19.16 | 101.6 MB | 2.53× |
| Offsets from restart runs; pairs only with skip blocks; cf elided when df; no maximum wdf | `4e9f8d9` | 7.56 | 55.1 MB | 1.37× |
| df packed with the flags; a single posting's df, cf, and wdf of 1 implied | `d2fe970` (with the region-size change) | 6.27 | 46.3 MB | 1.15× |
| termdict: lengths packed into one byte per entry | termdict `7ac4195` | 6.27 | 46.3 MB | 1.15× |
| Suffixes coded over the keys' alphabet (5 bits here); restarts every 32, blocks of 32–64 | termdict `4e3d37d`, strata `28c7175` | 6.27 | 39.9 MB | 0.992× |

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

## Alphabet-coded suffixes

The 5M dictionary's suffix bytes use 26 symbols (4.23 bits of entropy each). termdict now
codes each suffix byte as its rank in the dictionary's alphabet, `bit_width(size - 1)`
bits wide, so codes sort as the bytes do, and stores each restart run's suffixes as one
bit stream: 2.93 → 1.86 B/term at interval 16 (byte-aligned per entry would reach only
2.28). With codes, longer runs cost less to scan, so the interval and block size were
measured again (`dictsim` on the 5M terms):

| Restart interval | Block entries | Dictionary | Against Lucene | Lookup (word list, p50 of batches of 16) |
|---|---|---:|---:|---:|
| 16, uncoded (before) | 25–48 | 46.3 MB | 1.15× | ~375 ns |
| 16 | 25–48 | 42.3 MB | 1.053× | ~420 ns |
| 32 | 32–64 | 39.9 MB | 0.992× | ~470 ns |
| 32 | 48–96 | 39.6 MB | 0.986× | |
| 64 | 48–96 | 38.9 MB | 0.968× | ~550 ns |

32 / 32–64 is applied. Reindexed 5M: `terms` 39,185,071 + `term_index` 708,622 bytes =
39.9 MB. On the gate VM, against Lucene's `seekExact` on the same 3,964,752 terms (three
interleaved rounds): termdict 699–735 ns, Lucene 908–1,016 ns, 0.69–0.81× (0.54× before
coding). Queries against the previous index, in-process: `TOP_10` 0.998×, `COUNT` 0.979×,
counts equal; `verify` clean on all 1,162 gate queries; serving peak 572–573 MB, unchanged.

Open: 64 / 48–96 buys 2.4 more points at about 80 ns per lookup; Huffman codes would
take the suffixes from 1.86 to about 1.55 B/term.
