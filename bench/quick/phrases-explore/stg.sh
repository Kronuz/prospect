#!/bin/bash
I=$HOME/.scratch/prospect-idx/1000000-reordered; B=build-stage/prospect_engine
for r in 1 2 3; do for s in 2 3 9 2 3 9; do
 t=$(PHR_STAGE=$s $B bench $I COUNT 3 < phr.txt 2>/dev/null | awk -F'\t' '{s+=$1} END{print s}'); echo "round $r stage $s sum_us $t"
done; done
