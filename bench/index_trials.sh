#!/bin/bash
# Single-thread indexing of the 1M corpus by strata, Tantivy, Lucene, and Xapian, three
# trials with the engine order rotated. On Linux the page cache is dropped before each
# unit; elsewhere (no root) the corpus is read once before each, so every unit starts
# warm.
# Units with a result are skipped, so tools/vm/quiet_trials.sh can stop and restart it.
# Results in ~/bench/results/index-NAME: wall time, peak RSS, bytes, segments.
#   index_trials.sh NAME
set -u
name=${1:-run}
cd ~/bench/search-benchmark-game
out=~/bench/results/index-$name
mkdir -p $out
export PATH=$HOME/.cargo/bin:$PATH
os=$(uname -s)
# GNU time -v on Linux; BSD time -l elsewhere (real seconds, peak RSS in bytes on macOS
# and in KB on FreeBSD).
if [ "$os" = Linux ]; then time_flag=-v; stat_size=-c%s; else time_flag=-l; stat_size=-f%z; fi
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
		if [ "$os" = Linux ]; then
			sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
		else
			cat "$CORPUS" > /dev/null
		fi
		case $e in
		strata) cmd="engines/strata/build/strata_engine index $dir" ;;
		xapian-2.1.0) cmd="engines/xapian-2.1.0/xapian_engine index $dir/idx" ;;
		*) dir=engines/$e/idx; rm -rf $dir; cmd="make -C engines/$e index" ;;
		esac
		# shellcheck disable=SC2086
		if /usr/bin/time $time_flag -o $out/$unit.time $cmd < $CORPUS > $out/$unit.log 2>&1; then
			rm -rf $dir/idx.build
			find $dir -type f -exec stat $stat_size {} + | awk '{n += $1} END {print n}' > $out/$unit.bytes
		else
			echo "UNIT_FAILED $unit"
		fi
	done
done
rm -rf ~/idx1m-strata ~/idx1m-xapian-2.1.0
for e in "${engines[@]}"; do
	printf '%-18s' $e
	if [ "$os" = Linux ]; then
		for t in 1 2 3; do printf ' %s' "$(grep Elapsed $out/i$t-$e.time 2>/dev/null | awk '{print $NF}')"; done
		echo "  rss $(grep Maximum $out/i1-$e.time 2>/dev/null | awk '{print $NF}') KB  bytes $(cat $out/i1-$e.bytes 2>/dev/null)"
	else
		for t in 1 2 3; do printf ' %ss' "$(awk '/real/ {print $1; exit}' $out/i$t-$e.time 2>/dev/null)"; done
		echo "  rss $(awk '/maximum resident/ {print $1; exit}' $out/i1-$e.time 2>/dev/null)  bytes $(cat $out/i1-$e.bytes 2>/dev/null)"
	fi
done
echo TRIALS_DONE
