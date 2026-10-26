#!/bin/bash
# abm.sh OUT ROUNDS : base twice (A/A), tab1, tab8, tantivy; interleaved, order rotated per round
W=/Users/kronuz/code/Kronuz/prospect-wt/explore-phrases/.scratch; I=$HOME/.scratch/prospect-idx/1000000-reordered
out=$1 rounds=$2; mkdir -p $out/f
run() { # name cmd
 case $1 in
  base1|base2) bin=$W/build-main/prospect_engine;; tab1) bin=$W/build-tab1/prospect_engine;; tab8) bin=$W/build-tab8/prospect_engine;;
  tv) $W/tantivy/target/release/bench_query $W/tantivy/idx $2 3 < $W/phr.txt > $out/f/$r-$1-$2.tsv; return;;
 esac; $bin bench $I $2 3 < $W/phr.txt > $out/f/$r-$1-$2.tsv 2>/dev/null; }
names=(base1 tab1 tv tab8 base2)
for r in $(seq 1 $rounds); do for c in TOP_10 TOP_1000 COUNT; do
 k=$((r%5)); for i in 0 1 2 3 4; do run ${names[$(((i+k)%5))]} $c; done
done; echo "$(date +%T) round $r"; done
# pair up for ab.py
mk() { d=$out/$1; mkdir -p $d; for f in $out/f/*-$2-*.tsv; do b=$(basename $f); r=${b%%-*}; c=${b#*-$2-}; cp $f $d/$r-a-$c; done; for f in $out/f/*-$3-*.tsv; do b=$(basename $f); r=${b%%-*}; c=${b#*-$3-}; cp $f $d/$r-b-${c%.tsv}-queries.tsv; done; for f in $d/*-a-*; do case $f in *-queries.tsv) ;; *) mv $f ${f%.tsv}-queries.tsv;; esac; done; }
mk aa base1 base2; mk t1 base1 tab1; mk t8 base1 tab8; mk tv1 tv tab1; mk tv8 tv tab8; mk tv0 tv base1
