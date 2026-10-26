#!/bin/bash
# abx.sh OUT ROUNDS "CMDS" name=bin ...: interleaved rounds of every variant over the phrase queries
# (best of 3 per query), order rotated each round. A variant named tv is Tantivy's bench_query.
# Pair the names with score.py.
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)/.scratch}
I=${IDX:-$HOME/.scratch/prospect-idx/1000000-reordered}
out=$1 rounds=$2 cmds=$3; shift 3
names=(); bins=()
for v in "$@"; do names+=("${v%%=*}"); bins+=("${v#*=}"); done
n=${#names[@]}; mkdir -p "$out"
for r in $(seq 1 "$rounds"); do
 for c in $cmds; do
  for i in $(seq 0 $((n-1))); do
   k=$(((i+r)%n))
   if [ "${names[$k]}" = tv ]; then
    "$W/tantivy/target/release/bench_query" "$W/tantivy/idx" "$c" 3 < "$W/phr.txt" > "$out/$r-${names[$k]}-$c.tsv"
   else
    "${bins[$k]}" bench "$I" "$c" 3 < "$W/phr.txt" > "$out/$r-${names[$k]}-$c.tsv" 2>/dev/null
   fi
  done
 done
 echo "$(date +%T) round $r"
done
