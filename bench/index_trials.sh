#!/bin/bash
# Single-thread indexing of the 1M corpus by strata, Tantivy, Lucene, and Xapian in the
# gate VM, three trials with the engine order rotated, the page cache dropped before each.
# Units with a result are skipped, so tools/vm/quiet_trials.sh can stop and restart it.
# Results in ~/bench/results/index-NAME: wall time, peak RSS, bytes, segments.
#   index_trials.sh NAME
set -u
name=${1:-run}
cd ~/bench/search-benchmark-game
out=~/bench/results/index-$name
mkdir -p $out
export PATH=$HOME/.cargo/bin:$PATH
[ -f corpus-1m.json ] || head -1000000 corpus.json > corpus-1m.json
export CORPUS=$PWD/corpus-1m.json
engines=(strata tantivy-0.26-st lucene-10.4.0-st xapian-2.1.0)
for e in "${engines[@]}"; do make -s -C engines/$e compile > /dev/null 2>&1; done
for t in 1 2 3; do
	for i in 0 1 2 3; do
		e=${engines[$(((i + t - 1) % 4))]}
		unit=i$t-$e
		[ -f $out/$unit.bytes ] && continue
		echo "$(date +%T) $unit"
		dir=~/idx1m-$e
		rm -rf $dir && mkdir -p $dir
		sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
		case $e in
		strata) cmd="engines/strata/build/strata_engine index $dir" ;;
		xapian-2.1.0) cmd="engines/xapian-2.1.0/xapian_engine index $dir/idx" ;;
		*) dir=engines/$e/idx; rm -rf $dir; cmd="make -C engines/$e index" ;;
		esac
		# shellcheck disable=SC2086
		if /usr/bin/time -v -o $out/$unit.time $cmd < $CORPUS > $out/$unit.log 2>&1; then
			rm -rf $dir/idx.build
			du -sb $dir | cut -f1 > $out/$unit.bytes
		else
			echo "UNIT_FAILED $unit"
		fi
	done
done
rm -rf ~/idx1m-strata ~/idx1m-xapian-2.1.0
for e in "${engines[@]}"; do
	printf '%-18s' $e
	for t in 1 2 3; do printf ' %s' "$(grep Elapsed $out/i$t-$e.time 2>/dev/null | awk '{print $NF}')"; done
	echo "  rss $(grep Maximum $out/i1-$e.time 2>/dev/null | awk '{print $NF}') KB  bytes $(cat $out/i1-$e.bytes 2>/dev/null)"
done
echo TRIALS_DONE
