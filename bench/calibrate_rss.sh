#!/bin/bash
# Equal memory (BENCHMARKS.md): indexes the corpus once with REFERENCE and records its
# peak RSS, then searches strata's node budget (STRATA_BUDGET_MB, D17) for the one whose
# peak RSS comes within 3% of it, bisecting between 16 MiB and 16 GiB. Every run is
# recorded; the last line is the budget to use. Linux only (GNU time, drop_caches).
# Results in ~/bench/results/calibrate-NAME.
#   [DOCS=1000000] [KEYED=1] [STRATA_THREADS=1] calibrate_rss.sh NAME REFERENCE
set -u
name=$1 reference=$2
cd ~/bench/search-benchmark-game
out=~/bench/results/calibrate-$name
mkdir -p "$out"
export PATH=$HOME/.cargo/bin:$PATH KEYED=${KEYED:-1}
docs=${DOCS:-1000000}
[ -f corpus-$docs.json ] || head -"$docs" corpus.json > corpus-$docs.json
export CORPUS=$PWD/corpus-$docs.json
make -s -C engines/"$reference" compile > /dev/null 2>&1
make -s -C engines/strata compile > /dev/null 2>&1
# Runs one engine into a fresh directory; prints its peak RSS in KB and segment count.
run() {
	local e=$1 unit=$2 dir=~/calibrate-$1 cmd
	rm -rf "$dir" && mkdir -p "$dir"
	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
	case $e in
	strata) cmd="engines/strata/build/strata_engine index $dir" ;;
	xapian-2.1.0) cmd="engines/xapian-2.1.0/xapian_engine index $dir/idx" ;;
	*) dir=engines/$e/idx; rm -rf "$dir"; cmd="make -C engines/$e index" ;;
	esac
	# shellcheck disable=SC2086
	/usr/bin/time -v -o "$out/$unit.time" $cmd < "$CORPUS" > "$out/$unit.log" 2>&1 || { echo "UNIT_FAILED $unit" >&2; return 1; }
	local rss segments
	rss=$(awk '/Maximum resident/ {print $NF}' "$out/$unit.time")
	case $e in
	strata) segments=$(grep -o '[0-9]* segments' "$out/$unit.log" | tail -1 | awk '{print $1}') ;;
	lucene*) segments=$(find "$dir" -name '*.si' | wc -l) ;;
	tantivy*) segments=$(find "$dir" -name '*.idx' | wc -l) ;;
	*) segments=1 ;;
	esac
	echo "$rss ${segments:-?}"
	rm -rf ~/calibrate-strata ~/calibrate-xapian-2.1.0
}
if [ -f "$out/reference" ]; then read -r target ref_segments < "$out/reference"; else
	read -r target ref_segments < <(run "$reference" reference) || exit 1
	echo "$target $ref_segments" > "$out/reference"
fi
echo "$reference rss ${target} KB segments $ref_segments" | tee "$out/summary"
lo=16 hi=16384 best=
while [ $((hi - lo)) -gt 8 ]; do
	mid=$(((lo + hi) / 2))
	read -r rss segments < <(STRATA_BUDGET_MB=$mid run strata budget-$mid) || exit 1
	echo "strata budget $mid MB rss $rss KB segments $segments" | tee -a "$out/summary"
	off=$(awk -v r="$rss" -v t="$target" 'BEGIN { d = (r - t) / t; print (d < 0 ? -d : d) <= 0.03 }')
	if [ "$off" = 1 ]; then best=$mid; break; fi
	if [ "$rss" -lt "$target" ]; then lo=$mid; else hi=$mid; fi
	best=$mid
done
echo "BUDGET $best" | tee -a "$out/summary"
echo CALIBRATE_DONE
