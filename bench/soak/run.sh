#!/usr/bin/env bash
# The comparative soak for every engine in turn, from a harness checkout whose engines
# came from bench/engines/install.sh and were compiled. Results in
# ~/bench/results/soak-NAME/ENGINE.jsonl, one line per cycle.
#   run.sh HARNESS NAME [LIVE] [CYCLES]
set -euo pipefail
set -f  # the Lucene classpath's * stays literal
here="$(cd "$(dirname "$0")" && pwd)"
harness="${1:?usage: run.sh HARNESS NAME [LIVE] [CYCLES]}"
name="${2:?usage: run.sh HARNESS NAME [LIVE] [CYCLES]}"
live="${3:-100000}"
cycles="${4:-100}"
out=${SOAK_OUT:-~/bench/results}/soak-$name
mkdir -p "$out"
cd "$harness"
lucene=engines/lucene-10.4.0-st
command_of() {
	case $1 in
	prospect) echo "engines/prospect/build/prospect_engine churn {dir}" ;;
	tantivy-0.26-st) echo "engines/tantivy-0.26-st/target/release/churn {dir}" ;;
	lucene-10.4.0-st) echo "java -XX:+UseParallelGC --add-modules jdk.incubator.vector -cp $lucene/build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:$lucene/build/dependencies/* Churn {dir}" ;;
	xapian-2.1.0) echo "engines/xapian-2.1.0/xapian_engine churn {dir}" ;;
	esac
}
for e in prospect tantivy-0.26-st lucene-10.4.0-st xapian-2.1.0; do
	[ -s "$out/$e.jsonl" ] && [ "$(wc -l < "$out/$e.jsonl")" -gt "$cycles" ] && continue
	echo "$(date +%T) soak $e"
	# shellcheck disable=SC2086
	python3 "$here/soak.py" --corpus corpus.json --queries queries.txt --dir "${SOAK_DIR:-$HOME/soak-idx}" --live "$live" --cycles "$cycles" \
		-- $(command_of $e) > "$out/$e.jsonl" 2> "$out/$e.err" || echo "SOAK_FAILED $e"
	rm -rf "${SOAK_DIR:-$HOME/soak-idx}"
done
echo TRIALS_DONE
