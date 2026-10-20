#!/bin/bash
# Quick in-process A/B of two prospect builds: `prospect_engine bench` over the gate queries
# and the stratified terms, sides alternating each round, best of RUNS per query and
# round. Minutes, not hours, and no quiet host needed: noise hits both sides alike and
# the per-query best over rounds discards it. Score with quick/ab.py.
#   [SCHEME=bm25_xapian] ab.sh OUT A_BIN A_DIR B_BIN B_DIR [ROUNDS] [RUNS] [COMMANDS]
# SCHEME passes a weighting preset to `prospect_engine bench` (default: its own, bm25).
set -eu
out=$1 a_bin=$2 a_dir=$3 b_bin=$4 b_dir=$5
rounds=${6:-6} runs=${7:-3}
commands=${8:-"TOP_10 TOP_100 TOP_1000 TOP_100_COUNT COUNT"}
queries=${QUERIES:-$HOME/bench/search-benchmark-game/queries.txt}
terms=${TERMS:-$HOME/bench/search-benchmark-game/terms-stratified.txt}
mkdir -p "$out"
for r in $(seq 1 "$rounds"); do
	if [ $((r % 2)) = 1 ]; then order="a b"; else order="b a"; fi
	for c in $commands; do
		for s in $order; do
			eval "bin=\$${s}_bin dir=\$${s}_dir"
			# shellcheck disable=SC2086
			"$bin" bench "$dir" "$c" "$runs" ${SCHEME:-} < "$queries" > "$out/$r-$s-$c-queries.tsv"
			# shellcheck disable=SC2086
			"$bin" bench "$dir" "$c" "$runs" ${SCHEME:-} < "$terms" > "$out/$r-$s-$c-terms.tsv"
		done
	done
	echo "$(date +%T) round $r"
done
