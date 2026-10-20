#!/bin/bash
# The Phase 2 gate's quiet-host run (review G5), in the gate VM: five query trials of
# every engine with the engine order rotated, then three 1M-document single-thread
# indexing trials per engine, rotated. Each unit writes its result only when it
# completes, and a unit with a result is skipped, so tools/vm/quiet_trials.sh can stop
# the run on host load and start it again without losing finished units.
#   copy to ~/bench/trials.sh in the VM
set -u
cd ~/bench/search-benchmark-game
out=~/bench/results/g5
mkdir -p $out
export COMMANDS="TOP_10 TOP_100 TOP_1000 TOP_100_COUNT COUNT" PATH=$HOME/.cargo/bin:$PATH
engines=(prospect tantivy-0.26 lucene-10.4.0 xapian-2.1.0)
rotated() {  # rotation k of the engine list
	local k=$1 n=${#engines[@]}
	for i in $(seq 0 $((n - 1))); do echo -n "${engines[$(((i + k) % n))]} "; done
}

for e in "${engines[@]}" tantivy-0.26-st lucene-10.4.0-st; do make -s -C engines/$e compile > /dev/null 2>&1; done

for t in 1 2 3 4 5; do
	order=$(rotated $((t - 1)))
	for set in queries terms-stratified; do
		name=q$t-$set
		[ -f $out/$name.json ] && continue
		echo "$(date +%T) $name: $order"
		# shellcheck disable=SC2086
		python3 src/client.py $set.txt $order > $out/$name.log 2>&1 && cp results.json $out/$name.json || echo "UNIT_FAILED $name"
	done
done

[ -f corpus-1m.json ] || head -1000000 corpus.json > corpus-1m.json
export CORPUS=$PWD/corpus-1m.json
index_engines=(prospect tantivy-0.26-st lucene-10.4.0-st xapian-2.1.0)
for t in 1 2 3; do
	for i in 0 1 2 3; do
		e=${index_engines[$(((i + t - 1) % 4))]}
		name=i$t-$e
		[ -f $out/$name.bytes ] && continue
		echo "$(date +%T) $name"
		dir=~/idx1m-$e
		rm -rf $dir && mkdir -p $dir
		sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
		case $e in
		prospect) cmd="engines/prospect/build/prospect_engine index $dir" ;;
		xapian-2.1.0) cmd="engines/xapian-2.1.0/xapian_engine index $dir/idx" ;;
		*) dir=engines/$e/idx; rm -rf $dir; cmd="make -C engines/$e index" ;;  # idx is a make target
		esac
		# shellcheck disable=SC2086
		if /usr/bin/time -v -o $out/$name.time $cmd < $CORPUS > $out/$name.log 2>&1; then
			case $e in
			tantivy*) python3 -c "import json; m = json.load(open('$dir/meta.json')); print(len(m['segments']), [s['max_doc'] for s in m['segments']])" > $out/$name.segments ;;
			lucene*) ls $dir | grep '\.si$' > $out/$name.segments ;;
			*) grep -c '^segment:' $out/$name.log > $out/$name.segments || true ;;
			esac
			rm -rf $dir/idx.build  # Xapian's pre-compaction database
			du -sb $dir | cut -f1 > $out/$name.bytes
		else
			echo "UNIT_FAILED $name"
		fi
	done
done
rm -rf ~/idx1m-prospect ~/idx1m-xapian-2.1.0
echo TRIALS_DONE
