#!/bin/bash
# Builds prospect_engine at this checkout (b) and at $JOB_REF_BASE (a) into $OUT, and a synthetic
# 100,000-document index with a's binary: the inputs of stats_ab.sh.
#   [OUT=~/.scratch/supplied-stats] [DOCS=100000] stats_ab_build.sh
set -eu
out=${OUT:-$HOME/.scratch/supplied-stats}
docs=${DOCS:-100000}
here=$(pwd)
mkdir -p "$out"
for side in a b; do
	src=$here
	[ $side = a ] && src=$JOB_REF_BASE
	cmake -S "$src" -B "$src/build-ab" -DCMAKE_BUILD_TYPE=Release > "$out/build-$side.log" 2>&1
	cmake --build "$src/build-ab" -j4 --target prospect_engine >> "$out/build-$side.log" 2>&1
	cp "$src/build-ab/prospect_engine" "$out/engine-$side"
	file "$out/engine-$side"
done
python3 "$here/bench/quick/synth_corpus.py" "$out" "$docs"
rm -rf "$out/index"
PROSPECT_IDENTITY_SEED=1 "$out/engine-a" index "$out/index" < "$out/corpus.json"
echo "built $(date +%T)"
