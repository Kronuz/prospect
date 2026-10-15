# Phase 3 ratchet rows

Measured rows for Phase 3's gate, with the platform they were run on. Rows that are still
unmeasured are listed at the end.

## Indexing, 1 thread (gate VM, Linux x86_64)

**Superseded (2026-10-15):** these rows ran strata's benchmark engine on Phase 2's
single-segment writer, without keys, versions, flushes, merges, or commits, so they do
not measure T4. The engine now indexes through `strata::index` (`ad16376`, `5dfe415`)
and every engine runs keyed; the rows are being rerun at equal memory
(BENCHMARKS.md, "Indexing speed").

Run on the gate VM with `bench/index_trials.sh p3` and `tools/vm/quiet_trials.sh`, using
the VM's synced source tree as built at 06:48 on 2026-10-14 (Phase 3 lifecycle code; the copy
carries no commit id). The corpus is the 1M-document file from the search-benchmark-game. Each
engine ran three trials, with the engine order rotated and the page cache dropped before
each unit.

| Engine | Wall time (three trials) | Median | Peak RSS | Index bytes |
|---|---|---|---|---|
| strata | 1:32.11, 1:21.97, 1:20.66 | 82.0 s | 1.47 GiB | 609.0 MB |
| Tantivy 0.26 | 1:53.09, 1:38.86, 1:43.81 | 103.8 s | 1.66 GiB | 673.6 MB |
| Lucene 10.4.0 | 2:58.19, 2:45.69, 3:05.11 | 178.2 s | 2.42 GiB | 619.8 MB |
| Xapian 2.1.0 | 16:16.97, 17:51.37, 18:46.31 | 1071.4 s | 0.49 GiB | 3341.1 MB |

Relative to the fastest other engine:

1. **Time:** 0.79× Tantivy's median, so 1.27× faster. Phase 2's gate measured 0.84×
   Tantivy's speed, behind it; this row now passes the 1.0× ratchet.
2. **Peak RSS:** 0.88× Tantivy's and 0.61× Lucene's.
3. **Index bytes:** 0.90× Tantivy's and 0.98× Lucene's, as each indexer commits them.
   The size row after a full merge and per section is still owed (below).

## Queries, 16-segment strata against force-merged baselines (gate VM, Linux x86_64)

**Diagnostic, not a gate row** (P6 review R4): strata's 5M-document index is its
unmerged one (16 segments, 256 MB buffer), while the baselines' builders force-merge to
one segment and Lucene's buffer is 2,047 MB. The gate's "before merging" rows rebuild the
baselines unmerged at equal memory; its "after merging" rows merge strata too.

Run with `bench/q3.sh` under `tools/vm/quiet_trials.sh` on 2026-10-15 (one clean
attempt), five trials with the engine order rotated, the queries and the stratified term
set; strata at `5dfe415` as synced to the VM, Lucene 10.4.0 with and without document
reordering, Tantivy 0.26, Xapian 2.1.0. Scored by `bench/phase2/gate.py --target 1.0`
(the reference: the fastest of Lucene, Lucene with reordering, and Tantivy); pipe floor
74 µs. Counts equal every engine's on every query in every trial.

39 of 40 command-class rows miss 1.0×; strata wins every class against Xapian. Against
Phase 2's single-segment gate, normalized by Tantivy's drift between sessions, strata's
own excess is 1.03× (`TOP_10`), 1.18× (`TOP_100`, `COUNT`), and 1.58× (`TOP_1000`): the
per-segment costs P6 addresses (`docs/perf/p6-multisegment.md`). Whether Phase 3's code
changed the single-segment path is settled by a same-session A/B (P6 review R3).

| Command | Class | n | Trials | strata µs | Lucene µs | Tantivy µs | Xapian µs | Raw | Floor-subtracted | Against Xapian |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `TOP_10` | global | 657 | 5 | 1,629 | 1,212 | 1,304 | 10,796 | 1.58× [1.55–1.79] | 1.65× [1.62–1.89]  | 0.170× [0.161–0.179] |
| `TOP_10` | intersection | 300 | 5 | 1,342 | 1,098 | 1,038 | 8,666 | 1.41× [1.40–1.60] | 1.49× [1.46–1.69]  | 0.216× [0.203–0.228] |
| `TOP_10` | intersection_union | 40 | 5 | 4,545 | 3,099 | 3,328 | 13,008 | 2.06× [2.04–2.22] | 2.12× [2.10–2.30]  | 0.293× [0.280–0.297] |
| `TOP_10` | negated | 19 | 5 | 836 | 931 | 730 | 13,715 | 1.29× [1.21–1.40] | 1.32× [1.25–1.47]  | 0.059× [0.055–0.065] |
| `TOP_10` | phrase | 300 | 5 | 2,797 | 2,145 | 1,862 | 8,659 | 1.75× [1.74–1.97] | 1.83× [1.82–2.10]  | 0.340× [0.329–0.362] |
| `TOP_10` | term | 201 | 5 | 357 | 294 | 267 | 16,842 | 1.38× [1.34–1.46] | 1.53× [1.48–1.64]  | 0.066× [0.064–0.070] |
| `TOP_10` | two-phase-critic | 1 | 5 | 19,454 | 49,355 | 29,381 | 59,187 | 0.65× [0.61–0.67] | 0.65× [0.61–0.67]  | 0.320× [0.314–0.339] |
| `TOP_10` | union | 301 | 5 | 1,761 | 1,271 | 1,751 | 14,150 | 1.73× [1.69–1.96] | 1.83× [1.78–2.09]  | 0.133× [0.125–0.141] |
| `TOP_100` | global | 657 | 5 | 2,107 | 1,639 | 1,642 | 15,211 | 1.65× [1.62–1.68] | 1.72× [1.69–1.75]  | 0.163× [0.161–0.164] |
| `TOP_100` | intersection | 300 | 5 | 1,604 | 1,307 | 1,040 | 8,512 | 1.76× [1.64–1.77] | 1.88× [1.73–1.90]  | 0.260× [0.257–0.265] |
| `TOP_100` | intersection_union | 40 | 5 | 4,922 | 3,150 | 3,338 | 18,739 | 2.22× [2.17–2.26] | 2.28× [2.23–2.33]  | 0.221× [0.220–0.226] |
| `TOP_100` | negated | 19 | 5 | 1,338 | 1,349 | 737 | 13,675 | 1.88× [1.71–1.97] | 2.01× [1.79–2.11]  | 0.098× [0.094–0.101] |
| `TOP_100` | phrase | 300 | 5 | 2,829 | 2,681 | 1,826 | 7,950 | 1.74× [1.62–1.76] | 1.85× [1.70–1.87]  | 0.424× [0.423–0.427] |
| `TOP_100` | term | 201 | 5 | 642 | 431 | 337 | 16,810 | 1.77× [1.75–2.06] | 2.01× [2.01–2.38]  | 0.112× [0.111–0.137] |
| `TOP_100` | two-phase-critic | 1 | 5 | 34,014 | 48,735 | 28,759 | 91,810 | 1.16× [1.11–1.25] | 1.16× [1.11–1.25]  | 0.365× [0.355–0.371] |
| `TOP_100` | union | 301 | 5 | 2,515 | 2,015 | 2,598 | 23,272 | 1.70× [1.64–1.73] | 1.76× [1.70–1.81]  | 0.100× [0.100–0.102] |
| `TOP_1000` | global | 657 | 5 | 3,790 | 2,609 | 2,525 | 21,222 | 2.09× [2.00–2.17] | 2.19× [2.08–2.27]  | 0.219× [0.215–0.225] |
| `TOP_1000` | intersection | 300 | 5 | 2,239 | 1,480 | 1,086 | 8,693 | 2.21× [2.15–2.28] | 2.39× [2.32–2.47]  | 0.338× [0.332–0.350] |
| `TOP_1000` | intersection_union | 40 | 5 | 7,638 | 3,711 | 3,479 | 27,770 | 2.79× [2.62–2.83] | 2.85× [2.68–2.90]  | 0.243× [0.237–0.247] |
| `TOP_1000` | negated | 19 | 5 | 2,482 | 1,548 | 838 | 14,116 | 3.01× [3.00–3.32] | 3.26× [3.24–3.60]  | 0.180× [0.179–0.191] |
| `TOP_1000` | phrase | 300 | 5 | 2,988 | 2,933 | 1,827 | 7,714 | 1.84× [1.82–1.90] | 1.96× [1.94–2.03]  | 0.468× [0.465–0.482] |
| `TOP_1000` | term | 201 | 5 | 1,775 | 819 | 672 | 17,226 | 2.15× [2.09–2.25] | 2.33× [2.25–2.46]  | 0.231× [0.221–0.241] |
| `TOP_1000` | two-phase-critic | 1 | 5 | 39,284 | 48,412 | 28,126 | 136,150 | 1.40× [1.37–1.44] | 1.40× [1.37–1.44]  | 0.289× [0.284–0.294] |
| `TOP_1000` | union | 301 | 5 | 5,242 | 3,948 | 4,567 | 35,276 | 2.05× [1.95–2.12] | 2.11× [2.00–2.18]  | 0.141× [0.139–0.144] |
| `TOP_100_COUNT` | global | 657 | 5 | 4,285 | 5,793 | 3,559 | 36,321 | 1.56× [1.47–1.58] | 1.63× [1.53–1.66]  | 0.141× [0.139–0.147] |
| `TOP_100_COUNT` | intersection | 300 | 5 | 2,032 | 1,365 | 1,071 | 8,909 | 1.96× [1.92–2.03] | 2.10× [2.07–2.17]  | 0.278× [0.276–0.291] |
| `TOP_100_COUNT` | intersection_union | 40 | 5 | 5,137 | 3,794 | 3,602 | 35,227 | 1.76× [1.52–1.77] | 1.79× [1.54–1.81]  | 0.122× [0.121–0.126] |
| `TOP_100_COUNT` | negated | 19 | 5 | 1,591 | 1,414 | 756 | 14,297 | 2.20× [2.04–2.28] | 2.36× [2.18–2.47]  | 0.112× [0.107–0.118] |
| `TOP_100_COUNT` | phrase | 300 | 5 | 2,929 | 3,166 | 1,859 | 8,548 | 1.78× [1.70–1.87] | 1.89× [1.79–2.00]  | 0.457× [0.447–0.461] |
| `TOP_100_COUNT` | term | 201 | 5 | 638 | 983 | 696 | 18,129 | 1.20× [1.17–1.22] | 1.28× [1.23–1.32]  | 0.107× [0.105–0.110] |
| `TOP_100_COUNT` | two-phase-critic | 1 | 5 | 39,911 | 45,548 | 28,650 | 141,472 | 1.38× [1.33–1.42] | 1.38× [1.33–1.42]  | 0.286× [0.279–0.289] |
| `TOP_100_COUNT` | union | 301 | 5 | 7,242 | 11,521 | 6,692 | 69,144 | 1.25× [1.18–1.26] | 1.27× [1.19–1.27]  | 0.074× [0.073–0.077] |
| `COUNT` | global | 657 | 5 | 1,242 | 892 | 1,360 | 35,236 | 1.65× [1.61–1.83] | 1.76× [1.70–1.99]  | 0.059× [0.059–0.060] |
| `COUNT` | intersection | 300 | 5 | 1,129 | 850 | 863 | 8,634 | 1.75× [1.70–1.93] | 1.88× [1.81–2.11]  | 0.194× [0.193–0.196] |
| `COUNT` | intersection_union | 40 | 5 | 414 | 282 | 360 | 34,152 | 1.48× [1.46–1.70] | 1.66× [1.63–2.00]  | 0.010× [0.010–0.010] |
| `COUNT` | negated | 19 | 5 | 833 | 777 | 505 | 13,814 | 1.73× [1.68–1.79] | 1.89× [1.82–1.96]  | 0.061× [0.060–0.061] |
| `COUNT` | phrase | 300 | 5 | 2,708 | 3,077 | 1,796 | 8,333 | 1.70× [1.63–1.75] | 1.80× [1.71–1.86]  | 0.416× [0.411–0.418] |
| `COUNT` | term | 201 | 5 | 251 | 206 | 200 | 18,044 | 1.26× [1.24–1.29] | 1.41× [1.38–1.48]  | 0.042× [0.042–0.043] |
| `COUNT` | two-phase-critic | 1 | 5 | 38,848 | 46,443 | 28,860 | 137,079 | 1.34× [1.30–1.44] | 1.34× [1.30–1.44]  | 0.283× [0.278–0.293] |
| `COUNT` | union | 301 | 5 | 1,583 | 1,083 | 2,302 | 67,110 | 1.60× [1.55–1.77] | 1.69× [1.63–1.90]  | 0.023× [0.023–0.023] |

## Still unmeasured

- The same rows on macOS arm64 bare metal and on FreeBSD. They are required before Phase
  3R. The M1 is retired (2026-10-14: its device management keeps turning ssh off), so
  another arm64 machine is owed. One unrotated first round ran there before it was
  retired, after `148eebb` fixed the harness's stdin reading (macOS libc++ reads
  `std::cin` a character at a time, which had made strata's row 236.7 s): strata 26.8 s,
  Tantivy 0.26 34.8 s, Lucene 10.4.0 63.4 s; Xapian did not finish. It is not a gate row.
- Multi-thread and replace-heavy indexing, write amplification, refresh latency.
- Index size after a full merge, per section.
- The query rows on multi-segment indexes, before and after merging, including
  IResearch (which needs Docker on the VM) and PISA as a reference.
