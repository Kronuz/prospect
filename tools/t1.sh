#!/bin/sh
# T1 (build-rel configured; .scratch/t1/wiki.terms from wiki_terms.py; FK is the design's .scratch/fk):
# T1: terms + term_index (+ norms) of each corpus, flat and per-field, one segment and five shards.
cd "$(dirname "$0")/.."  # the repository
E=build-rel/prospect_engine
FK=~/code/Kronuz/strata-wt/fields-and-keys/.scratch/fk
W=.scratch/t1
out=$W/t1.txt
: > $out
run() { # corpus file shard
  for mode in flat fields; do
    d=$W/idx-$1-$3-$mode
    rm -rf $d
    if [ "$3" = all ]; then $E fields $d $2 $mode 2>/dev/null; else $E fields $d $2 $mode $3 2>/dev/null; fi | sed "s/^/$1 $3 $mode /" >> $out
    rm -rf $d
  done
}
for c in bank sparsekw sparsenum; do
  for s in all 0 1 2 3 4; do run $c $FK/$c.terms $s; done
done
run wiki $W/wiki.terms all
