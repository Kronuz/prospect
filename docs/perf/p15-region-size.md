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

Open: per-section sizes (larger regions for postings read in long runs), once the
remaining 4% against Tantivy is attributed by section.
