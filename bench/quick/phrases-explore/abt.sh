#!/bin/bash
# abt.sh OUT PROSPECT_BIN ROUNDS : tantivy (a) vs prospect (b), and prospect vs prospect (A/A)
W=/Users/kronuz/code/Kronuz/prospect-wt/explore-phrases/.scratch
out=$1 bin=$2 rounds=${3:-8}; I=$HOME/.scratch/prospect-idx/1000000-reordered
mkdir -p $out/tv $out/aa
for r in $(seq 1 $rounds); do
 for c in TOP_10 TOP_100 TOP_1000 TOP_100_COUNT COUNT; do
  if [ $((r%2)) = 1 ]; then
   $W/tantivy/target/release/bench_query $W/tantivy/idx $c 3 < $W/phr.txt > $out/tv/$r-a-$c-queries.tsv
   $bin bench $I $c 3 < $W/phr.txt > $out/tv/$r-b-$c-queries.tsv 2>/dev/null
  else
   $bin bench $I $c 3 < $W/phr.txt > $out/tv/$r-b-$c-queries.tsv 2>/dev/null
   $W/tantivy/target/release/bench_query $W/tantivy/idx $c 3 < $W/phr.txt > $out/tv/$r-a-$c-queries.tsv
  fi
  $bin bench $I $c 3 < $W/phr.txt > $out/aa/$r-a-$c-queries.tsv 2>/dev/null
  $bin bench $I $c 3 < $W/phr.txt > $out/aa/$r-b-$c-queries.tsv 2>/dev/null
 done; echo "$(date +%T) round $r"
done
