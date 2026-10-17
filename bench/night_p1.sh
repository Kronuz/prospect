#!/bin/bash
# The night of 2026-10-16 (P1's groundwork and the stretch reference), run in the gate VM under
# tools/vm/quiet_trials.sh (TRIALS='~/bench/night_p1.sh NAME'). Timed steps run first, untimed
# ones after; every step skips what it finished, so the guard can stop and restart it.
#   1. The quick tier (BENCHMARKS.md, "Tiers") against every engine, PISA included: the sampled
#      gate queries and stratified terms, three trials, a 20 s warm-up per engine and command.
#   2. The equal-bound skip's cost (reordering review, P1-R9): the merged 5M index with and
#      without the skip, in-process, both weighting schemes, four rounds of three runs.
#   3. Untimed: keyed Lucene with reordering at 5M (BP=1, P1-R10's reference), its sizes.
#   4. Untimed: T4's equal-memory calibration at 5M, keyed, against one-thread Lucene.
# DRY=1 shrinks every step to a smoke test (two queries per class, one trial, one round,
# 20,000 documents) and writes under NAME-dry.
#   night_p1.sh NAME
set -u
name=${1:?usage: night_p1.sh NAME}
dry=${DRY:-0}
[ "$dry" = 1 ] && name=$name-dry
log() { echo "$(date +%T) $*"; }
res=~/bench/results
G=~/bench/search-benchmark-game
merged=$G/engines/strata-merged/idx
for s in query_trials.sh calibrate_rss.sh sample_queries.py quick/ab.sh quick/ab.py; do
	[ -e ~/bench/$s ] || { log "missing ~/bench/$s"; exit 1; }
done
# The skip A/B's binaries, both built from aad1770: -a as is, -b with STRATA_EXPERIMENT_NO_EQUAL_SKIP.
[ -x ~/se-skip-a ] && [ -x ~/se-skip-b ] || { log "missing the skip experiment binaries"; exit 1; }

# 1. The quick tier.
q=$res/query-$name-quick
mkdir -p "$q"
# Xapian is left out tonight: its 15 GB index made room for PISA's build (strata is 0.13x it).
engines="strata-merged tantivy-0.26 lucene-10.4.0 lucene-10.4.0-bp"
for e in $engines; do [ -f "$res/query-n1-merged/index-$e.done" ] && touch "$q/index-$e.done"; done
if grep -q "PISA_INDEX_EXIT 0" ~/pisa-idx5m.log 2>/dev/null; then
	engines="$engines pisa-0.8.2"
	touch "$q/index-pisa-0.8.2.done"
else
	log "PISA's 5M index is not done: the quick tier runs without it"
fi
if [ "$dry" = 1 ]; then
	python3 ~/bench/sample_queries.py "$G/queries.txt" "$G/queries-dry.txt" 2 > /dev/null
	python3 ~/bench/sample_queries.py "$G/terms-stratified.txt" "$G/terms-dry.txt" 2 > /dev/null
	sets="queries-dry terms-dry" trials=1 warmup=1 iters=1
else
	[ -f "$G/queries-sample.txt" ] || python3 ~/bench/sample_queries.py "$G/queries.txt" "$G/queries-sample.txt" 40
	[ -f "$G/terms-sample.txt" ] || python3 ~/bench/sample_queries.py "$G/terms-stratified.txt" "$G/terms-sample.txt" 40
	sets="queries-sample terms-sample" trials=3 warmup=20 iters=10
fi
log "quick tier build: $engines"
ENGINES="$engines" ~/bench/query_trials.sh "$name-quick" build > "$q/build.log" 2>&1 || exit 1
grep -q INDEX_FAILED "$q/build.log" && { log "an index failed: $q/build.log"; exit 1; }
log "quick tier trials"
WARMUP_TIME=$warmup NUM_ITER=$iters SETS="$sets" TRIALS=$trials ENGINES="$engines" ~/bench/query_trials.sh "$name-quick" > "$q/trials.log" 2>&1 || exit 1
grep -q UNIT_FAILED "$q/trials.log" && { log "a unit failed: $q/trials.log"; exit 1; }

# 2. The equal-bound skip, both schemes.
if [ "$dry" = 1 ]; then rounds=1 runs=1 qs=$G/queries-dry.txt ts=$G/terms-dry.txt; else rounds=4 runs=3 qs=$G/queries.txt ts=$G/terms-stratified.txt; fi
for scheme in bm25 bm25_xapian; do
	d=$res/skip-$name-$scheme
	[ -f "$d.done" ] && continue
	log "equal-bound skip A/B, $scheme"
	arg=""
	[ "$scheme" = bm25 ] || arg=$scheme
	rm -rf "$d"
	SCHEME=$arg QUERIES=$qs TERMS=$ts ~/bench/quick/ab.sh "$d" ~/se-skip-a "$merged" ~/se-skip-b "$merged" $rounds $runs "TOP_10 TOP_100 TOP_1000" > "$d.log" 2>&1 &&
		python3 ~/bench/quick/ab.py "$d" > "$d.md" && touch "$d.done" || exit 1
done

# 3. Keyed Lucene with reordering, untimed.
bp=$res/keyed-bp-$name
if [ ! -f "$bp.done" ]; then
	log "keyed Lucene -bp build"
	corpus=$G/corpus.json
	[ "$dry" = 1 ] && { head -20000 "$corpus" > ~/c20k.json; corpus=~/c20k.json; }
	rm -rf ~/keyed-bp && mkdir -p ~/keyed-bp
	(cd "$G/engines/lucene-10.4.0-st" && KEYED=1 BP=1 /usr/bin/time -v -o "$bp.time" java -server --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED \
		-cp "build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:build/dependencies/*" BuildIndex ~/keyed-bp/idx < "$corpus" > "$bp.log" 2>&1) || exit 1
	for f in ~/keyed-bp/idx/*; do printf '%s\t%s\n' "${f##*.}" "$(stat -c %s "$f")"; done | awk -F'\t' '{s[$1] += $2; t += $2} END {for (k in s) print k "\t" s[k]; print "total\t" t}' | sort > "$bp.sizes"
	touch "$bp.done"
fi

# 4. T4's calibration at 5M, untimed.
cal=$res/calibrate-$name-5m.out
if ! grep -q "^BUDGET [0-9][0-9]*$" "$cal" 2>/dev/null; then
	log "5M calibration"
	docs=all
	[ "$dry" = 1 ] && docs=20000
	# At 1M a 8,200 MB budget peaked at 13.1 GB: the cap keeps every run inside the VM's 15 GB.
	DOCS=$docs KEYED=1 CAL_HI=4096 ~/bench/calibrate_rss.sh "$name-5m" lucene-10.4.0-st > "$cal.tmp" 2>&1 && mv "$cal.tmp" "$cal" || exit 1
fi
log NIGHT_DONE
echo TRIALS_DONE
