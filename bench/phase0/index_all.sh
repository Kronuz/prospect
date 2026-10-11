#!/bin/bash
# Indexes the corpus with each engine in turn; wall time, CPU, and peak RSS from GNU time.
set -u
cd ~/bench/search-benchmark-game
export CORPUS=$PWD/corpus.json PATH=$HOME/.cargo/bin:$PATH
for e in "$@"; do
  (cd engines/$e && make clean >/dev/null 2>&1; true)
  (cd engines/$e && make compile >/dev/null 2>&1)
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  /usr/bin/time -v -o ~/bench/results/index-$e.time make -C engines/$e index > ~/bench/results/index-$e.log 2>&1
  du -sb engines/$e/idx | cut -f1 > ~/bench/results/index-$e.bytes
  echo "$e done"
done
echo INDEX_ALL_DONE
