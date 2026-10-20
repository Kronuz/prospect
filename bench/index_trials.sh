#!/bin/bash
# Indexing of the 1M corpus by prospect, Tantivy, Lucene, and Xapian, five trials (PLAN's
# minimum from Phase 3) with the engine order rotated. On Linux the page cache is dropped before each
# unit; elsewhere (no root) the corpus is read once before each, so every unit starts
# warm.
# Units with a result are skipped, so tools/vm/quiet_trials.sh can stop and restart it.
# Results in ~/bench/results/index-NAME: wall time, peak RSS, bytes, segments.
# ENGINES overrides the engine list, DOCS the corpus size (a count, or "all" for the full
# corpus), and KEYED=1 makes every engine upsert by id (T4), passed to each indexer.
# prospect runs on PROSPECT_BUDGET_MB, the node budget calibrate_rss.sh matched to the
# reference's peak RSS, and PROSPECT_THREADS for the multi-thread rows.
#   PROSPECT_BUDGET_MB=N [PROSPECT_THREADS=8] [ENGINES="prospect tantivy-0.26-st"] [DOCS=all] [KEYED=1] index_trials.sh NAME
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
docs=${DOCS:-1000000}
if [ "$docs" = all ]; then export CORPUS=$PWD/corpus.json; else
	[ -f corpus-$docs.json ] || head -$docs corpus.json > corpus-$docs.json
	export CORPUS=$PWD/corpus-$docs.json
fi
export KEYED=${KEYED:-0}
[ -n "${PROSPECT_BUDGET_MB:-}" ] || { echo "PROSPECT_BUDGET_MB is required: run calibrate_rss.sh"; exit 1; }
export PROSPECT_BUDGET_MB
read -r -a engines <<< "${ENGINES:-prospect tantivy-0.26-st lucene-10.4.0-st xapian-2.1.0}"
count=${#engines[@]}
for e in "${engines[@]}"; do make -s -C engines/$e compile > /dev/null 2>&1; done
for t in 1 2 3 4 5; do
	for i in $(seq 0 $((count - 1))); do
		e=${engines[$(((i + t - 1) % count))]}
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
		prospect) cmd="engines/prospect/build/prospect_engine index $dir" ;;
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
rm -rf ~/idx1m-prospect ~/idx1m-xapian-2.1.0
for e in "${engines[@]}"; do
	printf '%-18s' $e
	if [ "$os" = Linux ]; then
		for t in 1 2 3 4 5; do printf ' %s' "$(grep Elapsed $out/i$t-$e.time 2>/dev/null | awk '{print $NF}')"; done
		echo "  rss $(grep Maximum $out/i1-$e.time 2>/dev/null | awk '{print $NF}') KB  bytes $(cat $out/i1-$e.bytes 2>/dev/null)"
	else
		for t in 1 2 3 4 5; do printf ' %ss' "$(awk '/real/ {print $1; exit}' $out/i$t-$e.time 2>/dev/null)"; done
		echo "  rss $(awk '/maximum resident/ {print $1; exit}' $out/i1-$e.time 2>/dev/null)  bytes $(cat $out/i1-$e.bytes 2>/dev/null)"
	fi
done
echo TRIALS_DONE
