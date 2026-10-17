#!/bin/bash
# The query ratchet's trials on the full corpus. "build" builds each engine's index once
# per NAME (no timing, so it needs no quiet host); the trials then run five times with the
# engine order rotated, over queries.txt and terms-stratified.txt. Lucene runs with and
# without document reordering (-bp); the faster sets the reference (BENCHMARKS.md). Units
# with a result are skipped, so tools/vm/quiet_trials.sh can stop and restart the run.
# Results in ~/bench/results/query-NAME.
#   [ENGINES="strata tantivy-0.26"] [SETS="queries terms-stratified"] [TRIALS=5] query_trials.sh NAME [build]
# SETS names query files in the harness directory (bench/sample_queries.py writes the quick
# tier's samples there); TRIALS below 5 is the quick tier (BENCHMARKS.md, "Tiers").
set -u
name=${1:-run}
mode=${2:-trials}
cd ~/bench/search-benchmark-game
out=~/bench/results/query-$name
mkdir -p "$out"
export COMMANDS="TOP_10 TOP_100 TOP_1000 TOP_100_COUNT COUNT" PATH=$HOME/.cargo/bin:$PATH CORPUS=$PWD/corpus.json
# ENGINES overrides the list (an engine without an index cannot run trials).
read -r -a engines <<< "${ENGINES:-strata tantivy-0.26 lucene-10.4.0 lucene-10.4.0-bp xapian-2.1.0 iresearch-26.03.1 pisa-0.8.2}"
rotated() {  # rotation k of the engine list
	local k=$1 n=${#engines[@]}
	for i in $(seq 0 $((n - 1))); do echo -n "${engines[$(((i + k) % n))]} "; done
}
if [ "$mode" = build ]; then
	for e in "${engines[@]}"; do
		make -s -C engines/"$e" compile > "$out/compile-$e.log" 2>&1
		[ -f "$out/index-$e.done" ] && continue
		echo "$(date +%T) index $e"
		rm -rf engines/"$e"/idx engines/"$e"/idx.build
		make -s -C engines/"$e" index > "$out/index-$e.log" 2>&1 && touch "$out/index-$e.done" || echo "INDEX_FAILED $e"
	done
	echo BUILD_DONE
	exit 0
fi
for e in "${engines[@]}"; do
	[ -f "$out/index-$e.done" ] || { echo "no index for $e: run build first"; exit 1; }
done
read -r -a sets <<< "${SETS:-queries terms-stratified}"
for t in $(seq 1 "${TRIALS:-5}"); do
	order=$(rotated $((t - 1)))
	for set in "${sets[@]}"; do
		unit=q$t-$set
		[ -f "$out/$unit.json" ] && continue
		echo "$(date +%T) $unit: $order"
		# shellcheck disable=SC2086
		python3 src/client.py $set.txt $order > "$out/$unit.log" 2>&1 && cp results.json "$out/$unit.json" || echo "UNIT_FAILED $unit"
	done
done
echo TRIALS_DONE
