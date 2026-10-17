# P15: verified-region size

Every section is checksummed per region of 2^shift bytes (shift 12–20, recorded in its
table of contents entry), and a reader verifies a region the first time a read touches
it. A larger region checks more bytes than the read needs and keeps them resident; a
smaller one stores more checksums (4 bytes per region). The default moves from 64 KiB
to 4 KiB.

The 5M-document Wikipedia index written both ways, the same binary serving the gate's
query stream (2,886 queries, every command) after dropping the page cache; peak values
from `/proc/PID/status`, sampled every 100 queries. Identical over two runs:

| | Disk | RssAnon | RssFile | Peak (VmHWM) |
|---|---:|---:|---:|---:|
| strata, 64 KiB regions | 2,716.1 MB | 2 MB | 674 MB | 677 MB |
| strata, 4 KiB regions | 2,718.6 MB | 7 MB | 576 MB | 583 MB (0.86×) |
| Tantivy 0.26 | | 1 MB | 558 MB | 559 MB |
| Lucene 10.4.0 | | 621 MB | 635 MB | 1,256 MB |

Against Tantivy, the faster engine's footprint here, serving memory goes from 1.21× to
1.04×. The checksums cost 2.5 MB of disk (0.09%) and 5 MB of anonymous memory.

Query time, two measurements that disagree in sign:

| | 4 KiB over 64 KiB |
|---|---|
| Harness, five alternating trials (`bench/ab.sh`), medians per class | 1.00–1.03×, two-phase-critic 1.02–1.05× |
| In-process (`strata_engine bench`, best of 3 × 5 runs), 962 gate queries | `TOP_10` 0.955×, `COUNT` 1.004× |

Counts match in both. The effect on queries is a few percent either way; the memory
saving is consistent, so 4 KiB is the default.

Tried and not kept: two bits per region in `residency` instead of a byte (one load per
64 verified regions) measured 1.004× (`TOP_10`) and 1.001× (`COUNT`), so the per-region
bookkeeping is not where time goes. Its model-based test stays in `residency`.

## Attribution by section

`strata_engine resident DIR` reports each section's bytes in the page cache after the
same run (Tantivy's files measured the same way, by `mincore`):

| | strata | Tantivy |
|---|---:|---:|
| Positions | 470.5 MB | 454.9 MB (`.pos`) |
| Postings | 261.3 MB | 270.4 MB (`.idx`) |
| Dictionary | 35.7 MB | 44.5 MB (`.term`) |
| Lengths and norms | 15.1 MB | 5.0 MB (`.fieldnorm`) |
| Stored | 0.2 MB | 0.2 MB |

The lengths section was checked whole at open, though the default `bm25` reads only
norms. It now verifies whole on its first use: lengths 10.1 → 0.3 MB resident, serving
peak 583 → 573–574 MB (two runs), 1.025× Tantivy. Positions hold the rest of the gap.

Tried and not kept: verifying a positions block's exact size from its header instead of
the largest possible encoding (1,155 bytes) moved positions 470.5 → 469.9 MB and the peak
not at all, so read-ahead verification is not what keeps positions resident. The gap
goes to P13: fewer position bytes per phrase query.

## Anonymous memory at open (2026-10-16)

Serving the merged, keyed 5M index (`bench/serve_mem.py`'s queries, gate VM), strata's
anonymous memory was 15.7 MB against Phase 2's 3 MB: 4 KiB regions made the per-region
CRC tables 16 times larger, and they were held twice (the table of contents and
residency's spans), and the 5.7 MB key filter was copied out of the mapping by
`fusefilter::filter8::deserialize`. Now the CRCs move into the spans (and sections not
verified per region drop theirs), and the key filter is read in place through
`fusefilter::filter8_view` (fusefilter `28f4241`).

| | At open | After 2,886 queries | Mapped index pages |
|---|---:|---:|---:|
| Before | 14.8 MB | 15.7 MB | 572 MB |
| After | 6.9 MB | 7.5 MB | 572 MB |

The remaining 7.5 MB is residency's CRC table (2.7 MB), its per-region state (0.7 MB),
the pinned term index (1.4 MB), and the query structures. Segments are byte-identical
(`fixtures_test`).
