#!/bin/bash
# The single-index query path with no statistics supplied, before (a, main) and after (b, the
# branch), over the synthetic index of stats_ab_build.sh: bench/quick/ab.sh rounds, sides
# alternating, scored by bench/quick/ab.py; AA=1 runs a against itself for the A/A band.
#   [OUT=~/.scratch/supplied-stats] [ROUNDS=8] [RUNS=5] [AA=1] [COMMANDS="TOP_10 ..."] stats_ab.sh
set -eu
out=${OUT:-$HOME/.scratch/supplied-stats}
here=$(pwd)
b=$out/engine-b
[ -n "${AA:-}" ] && b=$out/engine-a
export QUERIES=$out/queries.txt TERMS=$out/terms.txt
res=$out/results-${AA:+aa-}$(date +%H%M%S)
for s in a b; do file "$out/engine-$s"; done
SCHEME=${SCHEME:-} bash "$here/bench/quick/ab.sh" "$res" "$out/engine-a" "$out/index" "$b" "$out/index" "${ROUNDS:-8}" "${RUNS:-5}" "${COMMANDS:-TOP_10 TOP_100 COUNT}"
python3 "$here/bench/quick/ab.py" "$res"
