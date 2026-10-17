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

## Queries after merging (gate VM, Linux x86_64)

The gate's "after merging" rows: strata's 5M-document index force-merged to one segment
(`engines/strata-merged`, `strata_engine merge`) against baselines whose builders also
force-merge. Carried (PLAN, "Carried rows"): recorded here, and Phase 4's gate does not
pass while a row misses.

Run with `bench/overnight.sh n1` under `tools/vm/quiet_trials.sh` on the night of
2026-10-15 (one clean attempt), five trials with the engine order rotated, the queries
and the stratified term set; strata at `2e774db`, a Release build with `-march=native`;
Lucene 10.4.0 with and without document reordering, Tantivy 0.26, Xapian 2.1.0. Scored by
`bench/phase2/gate.py --target 1.0 --strata strata-merged`; pipe floor 67 µs. Counts
equal every engine's on every query in every trial.

Trial 4's query set overlapped about five minutes of small host builds (03:33–03:38;
host load peaked at 4.07 against the guard's limit of 6, so the guard did not stop the
run), and trial 5's query set overlapped a ten-minute file scan (04:42–04:52). Each row's
interval is over five trials. Scoring trials 1–3 alone moves no global ratio by more than
0.02 (`TOP_10` 1.35×, `TOP_1000` 1.33×, `COUNT` 1.34×), so neither overlap changes a
verdict.

Every global class misses the 1.0× target: strata is 1.33× to 1.34× the faster of
Lucene and Tantivy, with every interval clear of 1.0. Four class rows are under 1.0×:
`TOP_10` negated (0.86×), `TOP_10` two-phase-critic (0.63×), `TOP_100_COUNT` term
(0.90×), and `COUNT` intersection_union (0.93×); `COUNT` term is at 0.99× with an
interval up to 1.27×. The widest gaps are intersection_union at `TOP_10` and `TOP_100`
(1.88× and 1.85×) and union at `TOP_10` (1.54×). Against Xapian 2.1.0, strata takes
0.13× of its time at `TOP_10` global.

Against the 16-segment diagnostic above (same baselines, two days apart), merging and
P6 bring strata's global excess from 1.58× to 1.33× at `TOP_10`, from 2.09× to
1.33× at `TOP_1000`, and from 1.65× to 1.33× at `COUNT`.

| Command | Class | n | Trials | strata µs | Lucene µs | Tantivy µs | Xapian µs | Raw | Floor-subtracted | Against Xapian |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `TOP_10` | global | 657 | 5 | 1,350 | 1,177 | 1,288 | 10,505 | 1.33× [1.32–1.36] | 1.36× [1.34–1.39]  | 0.133× [0.131–0.147] |
| `TOP_10` | intersection | 300 | 5 | 1,046 | 1,067 | 1,020 | 8,395 | 1.14× [1.12–1.16] | 1.16× [1.13–1.17]  | 0.159× [0.157–0.175] |
| `TOP_10` | intersection_union | 40 | 5 | 4,053 | 3,028 | 3,310 | 12,636 | 1.88× [1.87–1.89] | 1.93× [1.92–1.94]  | 0.253× [0.252–0.276] |
| `TOP_10` | negated | 19 | 5 | 509 | 884 | 719 | 13,539 | 0.86× [0.85–0.87] | 0.84× [0.83–0.85]  | 0.036× [0.036–0.042] |
| `TOP_10` | phrase | 300 | 5 | 2,355 | 2,080 | 1,803 | 8,422 | 1.43× [1.39–1.46] | 1.46× [1.42–1.49]  | 0.261× [0.260–0.282] |
| `TOP_10` | term | 201 | 5 | 292 | 286 | 255 | 16,783 | 1.14× [1.12–1.27] | 1.19× [1.16–1.35]  | 0.051× [0.050–0.068] |
| `TOP_10` | two-phase-critic | 1 | 5 | 16,816 | 49,227 | 26,549 | 58,264 | 0.63× [0.61–0.69] | 0.63× [0.61–0.69]  | 0.288× [0.282–0.305] |
| `TOP_10` | union | 301 | 5 | 1,547 | 1,236 | 1,728 | 13,816 | 1.54× [1.52–1.58] | 1.60× [1.57–1.65]  | 0.110× [0.109–0.123] |
| `TOP_100` | global | 657 | 5 | 1,888 | 1,597 | 1,639 | 15,151 | 1.34× [1.32–1.41] | 1.36× [1.34–1.43]  | 0.127× [0.126–0.138] |
| `TOP_100` | intersection | 300 | 5 | 1,329 | 1,275 | 1,033 | 8,461 | 1.31× [1.29–1.35] | 1.35× [1.33–1.38]  | 0.188× [0.187–0.208] |
| `TOP_100` | intersection_union | 40 | 5 | 4,422 | 3,099 | 3,330 | 18,651 | 1.85× [1.84–1.90] | 1.89× [1.88–1.94]  | 0.187× [0.182–0.192] |
| `TOP_100` | negated | 19 | 5 | 862 | 1,325 | 726 | 13,632 | 1.17× [1.16–1.22] | 1.20× [1.17–1.24]  | 0.059× [0.058–0.067] |
| `TOP_100` | phrase | 300 | 5 | 2,521 | 2,640 | 1,806 | 7,930 | 1.38× [1.34–1.38] | 1.41× [1.37–1.41]  | 0.325× [0.322–0.334] |
| `TOP_100` | term | 201 | 5 | 487 | 448 | 407 | 16,979 | 1.22× [1.18–1.28] | 1.26× [1.20–1.33]  | 0.079× [0.075–0.084] |
| `TOP_100` | two-phase-critic | 1 | 5 | 32,382 | 48,535 | 27,195 | 91,264 | 1.19× [1.18–1.21] | 1.19× [1.18–1.21]  | 0.353× [0.319–0.362] |
| `TOP_100` | union | 301 | 5 | 2,460 | 1,970 | 2,593 | 23,229 | 1.49× [1.46–1.56] | 1.52× [1.49–1.60]  | 0.086× [0.086–0.092] |
| `TOP_1000` | global | 657 | 5 | 2,743 | 2,540 | 2,543 | 21,388 | 1.33× [1.32–1.39] | 1.35× [1.33–1.41]  | 0.134× [0.130–0.150] |
| `TOP_1000` | intersection | 300 | 5 | 1,542 | 1,471 | 1,084 | 8,692 | 1.35× [1.35–1.38] | 1.39× [1.38–1.42]  | 0.203× [0.194–0.229] |
| `TOP_1000` | intersection_union | 40 | 5 | 4,790 | 3,621 | 3,533 | 28,066 | 1.59× [1.51–1.60] | 1.61× [1.53–1.62]  | 0.136× [0.131–0.150] |
| `TOP_1000` | negated | 19 | 5 | 1,144 | 1,514 | 875 | 14,045 | 1.32× [1.28–1.36] | 1.36× [1.31–1.39]  | 0.076× [0.075–0.092] |
| `TOP_1000` | phrase | 300 | 5 | 2,576 | 2,871 | 1,809 | 7,704 | 1.38× [1.35–1.40] | 1.42× [1.38–1.44]  | 0.346× [0.324–0.369] |
| `TOP_1000` | term | 201 | 5 | 891 | 798 | 670 | 17,230 | 1.15× [1.14–1.19] | 1.15× [1.15–1.21]  | 0.114× [0.113–0.121] |
| `TOP_1000` | two-phase-critic | 1 | 5 | 38,496 | 47,559 | 26,717 | 138,017 | 1.42× [1.29–1.46] | 1.42× [1.29–1.46]  | 0.274× [0.263–0.282] |
| `TOP_1000` | union | 301 | 5 | 4,188 | 3,880 | 4,604 | 35,630 | 1.37× [1.34–1.43] | 1.38× [1.35–1.44]  | 0.092× [0.090–0.101] |
| `TOP_100_COUNT` | global | 657 | 5 | 5,485 | 5,654 | 3,406 | 35,497 | 1.34× [1.24–1.37] | 1.37× [1.25–1.40]  | 0.121× [0.118–0.126] |
| `TOP_100_COUNT` | intersection | 300 | 5 | 1,652 | 1,324 | 1,032 | 8,618 | 1.45× [1.43–1.56] | 1.49× [1.46–1.62]  | 0.203× [0.198–0.212] |
| `TOP_100_COUNT` | intersection_union | 40 | 5 | 4,576 | 3,738 | 3,403 | 34,382 | 1.48× [1.45–1.58] | 1.50× [1.46–1.60]  | 0.105× [0.100–0.109] |
| `TOP_100_COUNT` | negated | 19 | 5 | 1,036 | 1,288 | 718 | 13,863 | 1.44× [1.23–1.46] | 1.50× [1.25–1.52]  | 0.069× [0.069–0.077] |
| `TOP_100_COUNT` | phrase | 300 | 5 | 2,565 | 3,019 | 1,800 | 8,291 | 1.39× [1.32–1.43] | 1.43× [1.34–1.47]  | 0.344× [0.339–0.364] |
| `TOP_100_COUNT` | term | 201 | 5 | 494 | 992 | 710 | 18,155 | 0.90× [0.83–0.92] | 0.89× [0.81–0.92]  | 0.078× [0.071–0.087] |
| `TOP_100_COUNT` | two-phase-critic | 1 | 5 | 38,119 | 43,074 | 26,683 | 136,051 | 1.43× [1.29–1.47] | 1.43× [1.29–1.47]  | 0.277× [0.259–0.297] |
| `TOP_100_COUNT` | union | 301 | 5 | 12,474 | 11,291 | 6,408 | 67,733 | 1.29× [1.20–1.32] | 1.30× [1.21–1.33]  | 0.076× [0.075–0.080] |
| `COUNT` | global | 657 | 5 | 1,083 | 904 | 1,346 | 35,255 | 1.33× [1.28–1.38] | 1.36× [1.30–1.42]  | 0.046× [0.045–0.048] |
| `COUNT` | intersection | 300 | 5 | 981 | 850 | 847 | 8,536 | 1.43× [1.37–1.48] | 1.48× [1.40–1.54]  | 0.154× [0.150–0.160] |
| `COUNT` | intersection_union | 40 | 5 | 261 | 282 | 354 | 34,116 | 0.93× [0.89–0.99] | 0.93× [0.86–0.98]  | 0.006× [0.006–0.006] |
| `COUNT` | negated | 19 | 5 | 665 | 796 | 514 | 13,675 | 1.32× [1.09–1.42] | 1.37× [1.11–1.50]  | 0.046× [0.044–0.049] |
| `COUNT` | phrase | 300 | 5 | 2,547 | 3,261 | 1,767 | 8,258 | 1.39× [1.19–1.44] | 1.43× [1.21–1.49]  | 0.333× [0.322–0.347] |
| `COUNT` | term | 201 | 5 | 199 | 209 | 200 | 18,077 | 0.99× [0.99–1.27] | 1.00× [0.99–1.41]  | 0.031× [0.031–0.041] |
| `COUNT` | two-phase-critic | 1 | 5 | 37,926 | 47,891 | 27,336 | 136,066 | 1.38× [1.28–1.53] | 1.38× [1.28–1.53]  | 0.279× [0.273–0.283] |
| `COUNT` | union | 301 | 5 | 1,403 | 1,098 | 2,280 | 67,312 | 1.32× [1.28–1.37] | 1.34× [1.30–1.40]  | 0.018× [0.018–0.019] |

## Disk after a full merge

The 5M index force-merged, keys configured alike (PLAN: "keys configured alike"): strata
keys every document by its id (`Q` prefix, one key term each, and the `pk_filter`
section); Lucene 10.4.0 indexed with `KEYED=1` through the single-thread builder
(`StringField` ids, `updateDocument`), force-merged to one segment. Section sizes from
`strata_engine sections` (`1baaa6c`); Lucene's by file extension.

| Section | strata | Lucene, keyed | Ratio | Lucene `-bp`, unkeyed | Tantivy, unkeyed |
|---|---:|---:|---:|---:|---:|
| Whole index | 2,770.0 MB | 2,819.7 MB | 0.98× | 2,508.1 MB | 3,031.0 MB |
| Postings (`.doc`) | 1,007.9 MB | 985.6 MB | 1.02× | 833.2 MB | 1,054.6 MB |
| Positions (`.pos`) | 1,631.2 MB | 1,709.1 MB | 0.95× | 1,556.9 MB | 1,850.5 MB |
| Terms and term index (`.tim`, `.tip`, `.tmd`) | 92.0 MB | 67.9 MB | 1.36× | 39.6 MB | 47.1 MB |
| Stored (`.fdt`, `.fdx`, `.fdm`) | 15.5 MB | 31.9 MB | 0.49× | 53.1 MB | 53.6 MB |
| Norms (`.nvd`) | 5.0 MB | 5.0 MB | 1.00× | 5.0 MB | 5.0 MB |
| Key filter (`pk_filter`) | 5.7 MB | — | | — | — |
| Lengths (exact, Xapian's semantics) | 10.1 MB | — | | — | — |
| Doc values (the harness's `sort_field`) | — | 20.1 MB | | 20.1 MB | 20.1 MB |

**Against keyed Lucene with reordering** (night of 2026-10-16, the one-thread builder with
`BP=1`, `results/keyed-bp-n2.sizes`): 2,542.5 MB in all; `.doc` 833.7 MB, `.pos` 1,556.1 MB,
terms 75.8 MB, stored 51.7 MB. That build is PLAN's whole-index reference.

**strata with reordering** (P1 at `4594cce`, the 16-segment 5M index force-merged with
reordering on the gate VM, 2026-10-17; `strata_engine sections`):

| Section | strata, docid order | strata, reordered | Lucene `-bp`, keyed | Ratio |
|---|---:|---:|---:|---:|
| Whole index | 2,770.0 MB | 2,539.3 MB | 2,542.5 MB | 0.999× |
| Postings (`.doc`) | 1,007.9 MB | 845.6 MB | 833.7 MB | 1.014× |
| Positions (`.pos`) | 1,631.2 MB | 1,525.0 MB | 1,556.1 MB | 0.980× |
| Terms and term index | 92.0 MB | 91.2 MB | 75.8 MB | 1.20× |
| Stored | 15.5 MB | 24.9 MB | 51.7 MB | 0.48× |
| Docids (the permuted map) | — | 29.2 MB | — | |

The whole index passes against keyed `-bp`. Postings (1.014×) and the term dictionary
(1.20×) miss and are carried to format 13. The scale check passes: the docid-ordered and
the reordered indexes give byte-identical `strata_engine dump` output (`aec8d11`, 5,772
lines: the 962 gate queries at k = 10, 100, and 1,000 under both schemes, docids, `%a`
scores, and counts).

**The merge's cost** (T3): the forced merge with reordering takes 3,910 s (peak RSS 5.60 GB,
4.7 GB written) against 404 s in docid order (5.42 GB, 4.4 GB). Keyed Lucene `-bp`'s force
merge, reordering included, takes 595 s (its log's "Merge" at 06:48:03 to the build's end
at 06:57:58). T3 misses at 6.6×. Bisection recounted every term's degree on each
iteration, three passes over the rows. Updating the degrees of swapped documents only
(`1698c3f`, with the stored-record spill of `956ec8b`) takes the merge to 2,678 s, 1.46×
faster, with byte-identical output (peak RSS 5.88 GB, 4.9 GB written; informational: the
host compiled at low priority meanwhile). T3 is then 4.5×. With `reorder_threads` at 8
(`7a4afca`, at `c80996b`) the merge takes 845.9 s, at 355% CPU and 6.03 GB peak RSS, with
output byte-identical to the one-thread run's. Lucene's reorderer runs on more than one
thread: sampled every 5 s from `/proc` during a second keyed `-bp` build's force merge (652
s that run), its CPU averages 177%, with a fifth of the samples near 300%. T3 is 1.30× to
1.42× (against 652 s and 595 s), measured under a strict hold.

The whole index passes (0.98×). Two sections miss: postings at 1.02× and the term
dictionary at 1.36×. P12 brought the dictionary to 0.992× of Lucene's on an unkeyed
index; the 5,032,102 key terms add 52 MB to strata's and 27.7 MB to Lucene's. Against
Lucene with document reordering the postings are 1.21× its `.doc` (unkeyed; a keyed `-bp`
build is owed): reordering at merge (Track P1) is not implemented.

**Where the key terms' bytes go** (`strata_engine dictstats` and `dictdump` on the merged
segment). The keys are the corpus's URLs, 44.6 bytes on average (219.5 MB raw); every key
term has df 1 and wdf 0. Per key: the single ordinal 3.58 bytes (18.0 MB), the wdf 0
stored although the term's `no_wdf` flag implies it (5.0 MB), df and flags about one
byte, and the key text front-coded over the dictionary's alphabet. Delta-coding single
ordinals within the dictionary's runs of 32 would take them to 9.8 MB, and eliding the
implied wdf saves 5.0 MB: 78.8 MB, still 1.16×. D12 (`docs/design/phase-4.md`, format 13)
moves keys into their own section; front-coded blocks of 128 keys compressed take the key
text to 10.6 MB (zlib level 6 as the stand-in; the codec is D12's to choose against lookup
cost), so keys about 21 MB plus the unkeyed dictionary's 39.9 MB is about 61 MB, 0.90× of
Lucene's 67.9 MB. The dictionary row is carried to format 13 on that projection.

`strata_engine merge` commits and exits without reclaiming, so the merged directory still
holds the 16 sources (5.6 GB in all); only the manifest's files count above.

## Serving memory

`bench/serve_mem.py` on the gate VM after the night's other runs: each engine's `serve`
through the 962 gate queries for `TOP_10`, `COUNT`, and `TOP_1000` (2,886 queries), the page
cache dropped first, peaks sampled every 50 queries over the engine's processes; strata
force-merged, the baselines force-merged by their builders.

| Engine | Anonymous | Mapped index pages | Peak | strata against it |
|---|---:|---:|---:|---:|
| strata (merged) | 16 MB | 573 MB | 589 MB | |
| Tantivy 0.26 | 1 MB | 562 MB | 563 MB | 1.05× |
| Lucene 10.4.0 | 626 MB (JVM) | 639 MB | 1,265 MB | 0.47× |
| Lucene 10.4.0 `-bp` | 620 MB (JVM) | 567 MB | 1,187 MB | 0.50× |

A miss against Tantivy, at 1.05×. Mapped pages are 1.02× Tantivy's; strata's anonymous
memory is 16 MB where Phase 2 measured 3 MB, the part to explain first.

## Verify and parity

`strata_engine verify` (C14's form: pruned top-k equal to an exhaustive scan at k = 10, 100,
and 1,000, ordinals and scores, Lucene and Xapian BM25) on the gate VM at `98faf05`:

| Index | Queries | Mismatches | Wall | Peak RSS |
|---|---:|---:|---:|---:|
| 5M, 16 segments (before merging) | 962 gate queries | 0 | 3:15 | 1,137 MB |
| 5M, 16 segments | 200 stratified terms | 0 | 0:10 | 241 MB |
| 5M, force-merged to one segment | 962 gate queries | 0 | 2:38 | 709 MB |
| 5M, force-merged | 200 stratified terms | 0 | 0:07 | 130 MB |

`tools/parity.cc` against the oracle's 1,257-line rank vectors (419 queries over the first
10,000 documents), counted and pruned: 419 of 419 equal in counts and top-10 at 1, 4, and
16 segments weighted with the snapshot's statistics, worst relative error 3.87e-16. The
variant with deletes (drift against Xapian vectors from the same update stream, exact
again after a merge drops every deleted document) is not in the tool yet.

## Quick tier with PISA (night of 2026-10-16)

The quick tier (BENCHMARKS.md, "Tiers": 40 sampled queries per class and 40 stratified
terms, three trials, a 20 s warm-up per engine and command) on the merged 5M indexes,
strata at `db0a891`, PISA 0.8.2 with its own reordering, block-max WAND, and SIMD-BP128
(`results/query-n2-quick`; `gate.py --md --target 1.0 --strata strata-merged`). The
reference is the fastest of Lucene, Lucene `-bp`, and Tantivy, so `-bp` now sets most
rows; PISA answers no phrases. Global ratios:

| Command | Against the fastest baseline | Against PISA |
|---|---:|---:|
| `TOP_10` | 1.43× | 2.30× |
| `TOP_100` | 1.44× | 2.64× |
| `TOP_1000` | 1.39× | 3.10× |
| `TOP_100_COUNT` | 1.42× | 2.55× |
| `COUNT` | 1.16× | 0.48× |

Over the 35 class and command rows the geometric mean is 1.34× the fastest baseline and,
over PISA's 25, 1.59× PISA. PISA's lead is in pruned top-k (intersection_union 4.0–4.8×,
union 2.1–3.9×); strata leads it on single terms (0.39–0.87×) and on counts. The stretch
goal (0.5× the faster of Lucene and Tantivy) is about 2.7× away; the full table is the
command's output.

**What PISA's harness runs** (`engines/pisa-0.8.2/tools`), which bounds what these ratios
mean:
1. Recursive graph bisection at build time, SIMD-BP128 blocks of 128, and block-max data
   over fixed blocks of 40 postings.
2. Quantized BM25 (the `quantized` scorer): integer scores, so its top-k is not BM25's.
3. Top-k unions by block-max WAND, seeded with each term's 10th-best score precomputed at
   build time. The seed is safe at k = 10 and too high at k = 100 and 1,000, so those
   unions may drop true hits: PISA's `TOP_100` and `TOP_1000` union, intersection_union,
   and global rows are not exact.
4. Single terms and intersections by ranked AND; `TOP_100_COUNT` counts and ranks nothing;
   phrases are unsupported.
The harness checks counts only, and PISA's top-k commands answer a count of 1, so no check
saw (2) or (3). Its `TOP_10` rows are the fair reference; P21 seeds at k = 10, 100, and
1,000 so that every k stays exact.

## Gate status (2026-10-16)

PLAN's Phase 3 gate, row by row. The query rows are carried (PLAN, "Carried rows"): the
gate records their ratios and gaps, and Phase 4's gate does not pass while they miss.

| Row | State | Where |
|---|---|---|
| Power loss at ≥ 10,000 crash points; crashes during recovery | Pass | `phase-3-lifecycle.md` (T1), background merges at 10,000 points at `e8accae` |
| Digest equal across two replays | Pass | `phase-3-lifecycle.md` (T2) |
| Disk full: fail closed, no corruption, recovery | Pass | `phase-3-lifecycle.md` (T5) |
| Queries after merging (strata force-merged, baselines force-merged), five trials | Miss, carried: global 1.33–1.34× | above |
| Queries before merging (baselines unmerged at equal memory) | Owed: calibration on the full corpus, then a scheduled night | — |
| Indexing, 1 and 8 threads, keyed (T4), five trials at equal memory | Owed: calibrated at 5M (a 2,056 MB budget peaks at 4,608 MB against one-thread Lucene's 4,593 MB, 0.3% over; `results/calibrate-n2-5m.out`); the trials wait for P1, since C2 records the rows at a commit that includes it | — |
| Write amplification against Lucene (T3), append-only and replace-heavy | Owed: rescheduled with T4 | — |
| Memory (serving and indexing peaks), disk after a full merge, per section | Disk with P1: whole index 0.999× keyed `-bp` (pass); postings 1.014× and terms 1.20× carried to format 13. Serving memory 1.05× Tantivy (miss; anonymous 16 MB against Phase 2's 3 MB); indexing peaks with T4 | "Disk after a full merge" and "Serving memory" below |
| `verify` and `parity` on multi-segment indexes | Pass before and after merging; `parity` with deletes owed | "Verify and parity" below |
| Coverage targets and the test-parity inventory's Phase 3 rows | Owed: `cost()` checked directly (`cost_test`); scores against Lucene's own, document limits, and nested WAND or C7's cost model remain; reads while a writer commits need a shared-ownership reader (Phase 3R) | `docs/testing/parity.md` |
| The rows on FreeBSD and arm64 (before Phase 3R) | Owed: FreeBSD VM; an arm64 host | — |

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
  IResearch (which needs Docker on the VM); PISA ran in the quick tier only.
