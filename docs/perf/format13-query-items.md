# Format 13's query freeze items: bytes, time, and the joint selection

Evidence for the lead (SYS-6: branch measurements, not gate rows) for the items perf/query-exp's
freeze table kept (`docs/perf/query-gap-format13.md` at fac1e8d): M16 at A3@128, M23, M22, M20
at two pairs per k, and M3t, built on phase4/f13-query (`docs/status/f13-query.md`).

- Binaries: `base` is main 026ee10 (format 13 at pre-freeze 0x80000001); `m16`, `m23`, `m22`,
  `m20` are this branch at 88f23ee, b9a30d4, c21ae56, 07bf37d (the last also carries the merge of
  main d058cd9, the directory lock, which changes no segment byte); `final` is dd55eb6 (M3t and
  the writer's seed fix on top; bytes as `m20`); `final4` is 492b549 (M22's faster decode and
  one probe per block; bytes as `m20`).
- Host: macOS 15.6, Intel i9-9980HK (x86_64, 8 cores), Apple clang, `-DCMAKE_BUILD_TYPE=Release`,
  shared with other agents' holds; every timed run here took a hold of its own (`quiet run`,
  `QUIET_LEVEL=light` for one-thread runs).
- Corpus: `strata-wt/index-size/.scratch/corpus-{100000,1000000}.json`, the gate corpus's
  first 100,000 and 1,000,000 JSON lines. Queries: the 962 gate queries
  (`strata/.scratch/phase2/queries.txt`) and the 200 stratified terms
  (`bench/phase2/terms-stratified.txt`).
- Indexes: `strata_engine index` flushing at `STRATA_BUFFER_MB=40` without merges, then
  `strata_engine merge` to one segment in docid order (`STRATA_REORDER=0`, "merged") and
  reordered (`STRATA_REORDER_THREADS=4`, "reordered"); sections by `strata_engine sections`.
- Raw files: `.scratch/idx/`, `.scratch/ab/` in the developer's worktree (untracked).

## Bytes

Postings section in bytes, each item over the one before it (format 12's figures were not
re-measured; `base` is main's format 13, whose postings encoding is format 12's):

| Index | base | M16 (A3@128) | M23 | M22 | M20 | final over base |
|---|---:|---:|---:|---:|---:|---:|
| 100k merged | 15,971,722 | −79,544 | −6,179 | −845,530 | +79,276 | −851,977 (−5.3%) |
| 100k reordered | 14,799,435 | −74,256 | −4,164 | −1,331,872 | +79,276 | −1,331,016 (−9.0%) |
| 1M merged | 218,774,896 | −779,637 | −105,131 | −15,439,257 | +485,647 | −15,838,378 (−7.2%) |
| 1M reordered | 190,781,607 | −722,160 | −80,972 | −19,750,326 | +485,647 | −20,067,811 (−10.5%) |

The whole file moves by the same bytes (the terms section by under 400 bytes: smaller postings
offsets in the term infos). At 1M reordered: 576,587,668 against 556,499,920 (−3.5%).

Against E6's projections at 5M: A3@128 takes −0.38% of the postings here, twice its projected
share (−1.5 MB of 845.6 MB, −0.18%; not attributed further: this layout also drops each
group's first positional deltas, and the corpus is a fifth the size); M23's −0.04% is its projected share (−0.3 MB at 5M); M22 saves far more than E6 measured on the 50 most frequent terms (−7% of their
ordinal bytes): over every term with skip blocks, Elias-Fano is the smallest codec in enough
blocks to take 9 to 10% off the reordered postings; M20 costs 0.25% (E6/E4b projected 1.2 to
2.5 MB at 5M, 0.14 to 0.30%).

## The joint selection (F13-R8)

D15's constraint is the postings section at 5M at most keyed `-bp`'s `.doc`, 833.7 MB, where
format 12's reordered postings are 845.6 MB (11.9 MB over), with the whole index's 3.2 MB of
headroom read with it. These are 1M figures; the 5M section sizes are exact on a branch build
and are the lead's to confirm on the gate VM (commands below). Projected by the 1M reordered
ratio (0.895), the kept set takes format 12's 845.6 MB to about 757 MB, 77 MB under keyed
`-bp`'s `.doc`, so:

1. **The postings row closes** by this projection, with M22 the item that closes it. Without
   M22 the other items net −0.32 MB at 1M reordered (M16 −0.72, M23 −0.08, M20 +0.49), about
   −0.17%, or −1.4 MB of 845.6 at 5M by the same ratio: the row stays about 10.5 MB over.
2. **A4@128's +4.4 MB is paid for** by this projection: A4@128 in place of A3@128 adds about 5.9
   MB at 5M (E6: +4.4 against −1.5), leaving the postings about 71 MB under the constraint and
   the whole index far inside its headroom. E6 found A4@128 the fastest layout on every trace.
   The freeze table's rule ("A4@128 if the joint selection pays") then selects A4@128; this
   branch builds A3@128, the layout the table decided for the case the budget did not pay, and
   switching is the lead's call once the 5M bytes are confirmed (recorded under "Blocked on" in
   the ledger). Without M22 (see "Time") it is not paid for, and A3@128 stands.

## Time: run-level A/Bs (A/A first)

`ab-run-level.md`'s method (perf/query-exp's `bench/quick/ab.py --aa`), one thread under a light
hold, the 1M indexes, the 962 gate queries and 200 stratified terms. A row is a **loss** when its
lower bound clears both 1.03 and the A/A band's upper bound, a **win** when its upper bound is
below both 1.0 and the band's lower bound with the ratio at most 0.97. Raw: `.scratch/ab/`.

**Per item, each against the one before** (reordered, `bm25`, four runs of `TOP_10`, `TOP_1000`,
`COUNT`; 31 to 40 s per phase):

| Item | Wins | Losses | Notable rows (B/A) |
|---|---|---|---|
| M16 (A3@128) | none | none | term:frequent `TOP_10` 1.038 [1.023–1.053], phrase `TOP_1000` 1.028, `COUNT` union 1.038 (all neither) |
| M23 | intersection_union `TOP_10` 0.961 | none | `TOP_1000` intersection 0.984, phrase 0.977; `COUNT` 0.97 to 0.99 |
| M22, as committed at c21ae56 | none | every class but terms | `TOP_10` intersection 1.614, negated 1.845, phrase 1.473, union 1.267; `COUNT` intersection 1.654, union 1.647 |
| M20 | term `TOP_10` 0.862, term:frequent 0.639 and 0.705, union `TOP_10` 0.873 and `TOP_1000` 0.885, `COUNT` union 0.965 | none | intersections and phrases 0.98 to 1.00 |
| M3t | term:frequent `TOP_10` 0.901 | none | everything else 0.97 to 1.01 |

So the kept set's losses are M22's. Its decoder then changed (492b549): the first advance into an
Elias-Fano block probes, a second decodes it whole (each in-block advance had probed from
scratch), the load reads the first ordinal directly, and the decode places a byte of high bits at
a time. The decode alone (`final` against a build of it, reordered, four runs): `COUNT` union 0.902,
intersection 0.924; base against `final3` (one probe per block) took intersection `TOP_10` from
1.614 to 1.131.

**The kept set against main** (`base` against `final4`, six runs, all commands; phases 99 to 122 s):

| Row (B/A [95% over runs]; verdict) | Reordered `bm25` | Merged `bm25` | Reordered `bm25_xapian` |
|---|---|---|---|
| term `TOP_10` | 0.800 [0.624–1.025] | 0.933 [0.857–1.017] | 0.887 [0.798–0.986] |
| term:frequent `TOP_10` | 0.506 [0.414–0.618] win | 0.683 [0.632–0.739] win | 0.529 [0.482–0.581] win |
| term:frequent `TOP_1000` | 0.681 [0.637–0.729] win | 0.827 [0.746–0.916] win | 0.732 [0.700–0.765] win |
| union `TOP_10` | 0.989 [0.857–1.141] | 0.971 [0.883–1.069] | 0.961 [0.910–1.014] |
| union `TOP_1000` | 0.964 [0.902–1.029] | 0.942 [0.894–0.994] | 0.921 [0.912–0.931] win |
| intersection `TOP_10` | 1.140 [1.000–1.299] | 1.078 [0.989–1.175] | 1.106 [1.039–1.178] |
| intersection `TOP_1000` | 1.153 [1.061–1.253] loss | 1.103 [1.075–1.131] | 1.089 [1.078–1.100] |
| phrase `TOP_1000` | 1.143 [1.051–1.244] loss | 1.091 [1.055–1.128] | 1.095 [1.084–1.105] |
| `TOP_100_COUNT` intersection | 1.104 [1.060–1.151] loss | 1.146 [1.127–1.165] loss | 1.103 [1.084–1.124] loss |
| `COUNT` intersection | 1.166 [1.114–1.220] loss | 1.106 [1.015–1.206] | 1.156 [1.121–1.192] loss |
| `COUNT` union | 1.228 [1.181–1.276] loss | 1.167 [1.062–1.284] loss | 1.208 [1.164–1.254] loss |
| `COUNT` negated | 1.196 [1.160–1.233] loss | 1.423 [1.155–1.753] loss | 1.202 [1.181–1.224] loss |

Rows without a verdict are "neither" at this session's noise; these sessions' A/A bands ran wider
than the per-item ones (to 0.75–1.30 on some term rows), so the term `TOP_10` rows that won per item
read "neither" here. Results are identical to main's: `dump` (top 10, 100, 1,000 with counts, both
presets, 1,162 queries) is byte-identical between `base` and `final` on both 1M indexes, and
`verify` (pruned top-k against an exhaustive scan) reads 0 mismatches over 1,162 queries on each.

**Reading.** M16, M23, M20, and M3t pass on this host: no loss, and M20 and M3t win their target
rows (frequent terms 0.51 to 0.83 against main with everything on). M22 fails D14's guard: its
blocks are the smallest codec in about a tenth of the postings' bytes, so intersections, phrases,
negations, and counted unions now decode Elias-Fano where they decoded FOR or OR'd bitsets, and
even with the faster decoder they lose 1.09 to 1.42. E6 measured Elias-Fano's probe (one target per
block, 2.6x FOR's decode and search), not its whole-block decode, nor how many blocks it takes
when chosen by size over every term. That decision is recorded under "Blocked on" in the ledger.

**Indexing** (100k, three interleaved builds per side, seconds): index 2, 2, 3 against 3, 3, 3;
docid merge 2, 2, 3 against 2, 2, 3; reordering merge 5, 5, 5 against 5, 5, 6: within this host's
one-second resolution. Before dd55eb6 the writer scanned every length per term for the seeds, and a
1M docid merge took 115 s against 71 to 79 s; the 1M timings after the fix are the gate VM's.

## Gate VM confirmation (the lead's)

The 5M bytes and the run-level guard on the gate VM, as perf/query-exp ran its sessions (one
thread pinned, light holds, S′ in `~/bench/results/sprime/`). `$CORPUS` is the 5M gate corpus the
engines are indexed from.

```sh
# Builds: main (A) and this branch (B), Release, strata_engine only.
git -C ~/libs/strata fetch origin && git -C ~/libs/strata worktree add ~/libs/f13q-main origin/main
git -C ~/libs/strata worktree add ~/libs/f13q phase4/f13-query   # dd55eb6 or later
for w in f13q-main f13q; do cmake -S ~/libs/$w -B ~/libs/$w/build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build ~/libs/$w/build-rel -j4 --target strata_engine; done

# 5M indexes per side: flushed, then one reordered merge (the carried postings row) and one
# docid-order merge; section sizes are exact, so this is the bytes row (F1 item 3).
for s in f13q-main f13q; do
  B=~/libs/$s/build-rel/strata_engine D=~/bench/results/f13q/$s
  STRATA_AUTO_MERGE=0 $B index $D-flushed < $CORPUS
  cp -R $D-flushed $D-merged && mv $D-flushed $D-reordered
  STRATA_REORDER=0 $B merge $D-merged && STRATA_REORDER_THREADS=4 $B merge $D-reordered
  for k in merged reordered; do mkdir -p $D-sec-$k && ln -sf $(ls -S $D-$k/*.sx | head -1) $D-sec-$k/segment.sx && $B sections $D-sec-$k; done
done

# Result identity and the pruned-against-exhaustive check on B (both presets, k 10 to 1,000).
cat ~/bench/results/sprime/queries.txt ~/bench/results/sprime/terms.txt > ~/bench/results/f13q/sprime.txt
for k in merged reordered; do
  ~/libs/f13q-main/build-rel/strata_engine dump ~/bench/results/f13q/f13q-main-$k < ~/bench/results/f13q/sprime.txt > ~/bench/results/f13q/a-$k.dump
  ~/libs/f13q/build-rel/strata_engine dump ~/bench/results/f13q/f13q-$k < ~/bench/results/f13q/sprime.txt > ~/bench/results/f13q/b-$k.dump
  cmp ~/bench/results/f13q/a-$k.dump ~/bench/results/f13q/b-$k.dump
  ~/libs/f13q/build-rel/strata_engine verify ~/bench/results/f13q/f13q-$k < ~/bench/results/f13q/sprime.txt
done

# Run-level sessions, A/A first (ab-run-level.md), each index and preset.
for k in reordered merged; do for sc in bm25 bm25_xapian; do
  QUIET_LEVEL=light quiet run f13q-$k-$sc 40 "f13-query run-level $k $sc" -- \
    ~/bench/results/session.sh ~/bench/results/f13q/s-$k-$sc $sc \
      ~/libs/f13q-main/build-rel/strata_engine ~/bench/results/f13q/f13q-main-$k \
      ~/libs/f13q/build-rel/strata_engine ~/bench/results/f13q/f13q-$k 8
done; done
```

Parity against the Xapian oracle (`tools/parity`) needs the oracle's rank vectors, which live on
the gate VM only: `~/libs/f13q/build/parity $CORPUS N RANK_JSONL` and with `PARITY_REORDER`, as
phase 2's harness runs it.
