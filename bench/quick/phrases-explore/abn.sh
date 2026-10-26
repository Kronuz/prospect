#!/bin/bash
# abn.sh OUT BASE NEW ROUNDS [CMDS]: a=BASE b=NEW (ab), and BASE vs BASE (aa), interleaved
W=/Users/kronuz/code/Kronuz/prospect-wt/explore-phrases/.scratch
out=$1 A=$2 B=$3 rounds=${4:-8} cmds=${5:-"TOP_10 TOP_1000 COUNT"}; I=$HOME/.scratch/prospect-idx/1000000-reordered
mkdir -p $out/ab $out/aa
for r in $(seq 1 $rounds); do
 for c in $cmds; do
  if [ $((r%2)) = 1 ]; then o="a b"; else o="b a"; fi
  for s in $o; do eval bin=\$$(echo $s | tr a-z A-Z); $bin bench $I $c 3 < $W/phr.txt > $out/ab/$r-$s-$c-queries.tsv 2>/dev/null; done
  $A bench $I $c 3 < $W/phr.txt > $out/aa/$r-a-$c-queries.tsv 2>/dev/null
  $A bench $I $c 3 < $W/phr.txt > $out/aa/$r-b-$c-queries.tsv 2>/dev/null
 done; echo "$(date +%T) round $r"
done
