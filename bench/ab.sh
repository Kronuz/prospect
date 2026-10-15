#!/bin/bash
# A/B query trials of two strata variants in the gate VM: engines strata-a and strata-b
# serve the given indexes with the given binaries, alternating order over five trials of
# both query sets. Units with a result are skipped, so tools/vm/quiet_trials.sh can stop
# and restart it. Results land in ~/bench/results/ab-NAME; compare with gate.py.
#   ab.sh NAME A_IDX B_IDX [A_BINARY [B_BINARY]]
set -u
name=$1 a_idx=$2 b_idx=$3
a_bin=${4:-$HOME/bench/search-benchmark-game/engines/strata/build/strata_engine}
b_bin=${5:-$a_bin}
cd ~/bench/search-benchmark-game
out=~/bench/results/ab-$name
mkdir -p $out
export COMMANDS="TOP_10 TOP_100 TOP_1000 TOP_100_COUNT COUNT"
for v in a b; do
	d=engines/strata-$v
	mkdir -p $d
	eval "bin=\$${v}_bin idx=\$${v}_idx"
	printf 'compile:\n\nserve:\n\t@%s serve %s\n' "$bin" "$idx" > $d/Makefile
done
for t in 1 2 3 4 5; do
	if [ $((t % 2)) = 1 ]; then order="strata-a strata-b"; else order="strata-b strata-a"; fi
	for set in queries terms-stratified; do
		unit=q$t-$set
		[ -f $out/$unit.json ] && continue
		# A server of the previous unit may still hold the index's owner lock.
		while pgrep -f "[s]trata_engine serve" >/dev/null; do sleep 1; done
		echo "$(date +%T) $unit: $order"
		# shellcheck disable=SC2086
		python3 src/client.py $set.txt $order > $out/$unit.log 2>&1 && cp results.json $out/$unit.json || echo "UNIT_FAILED $unit"
	done
done
echo TRIALS_DONE
