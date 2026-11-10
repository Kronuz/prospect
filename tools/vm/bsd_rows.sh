#!/usr/bin/env bash
# Phase 3's prospect rows on strata's FreeBSD VM (A8, a deferred check; D51). Ships the engine's
# source ($JOB_REF_MAIN when the job names a main ref, else this checkout), the first DOCS
# documents of the gate VM's corpus, its gate queries, and the oracle's ranks; builds
# prospect_engine and parity, then runs one keyed one-thread index (time -l: wall, CPU, peak
# RSS; bytes, segments), a reordered merge, verify before and after it, and parity at 4
# segments and with deletes. Files land in $JOB_OUT/bsd. The VM has no rust or JDK, so these
# are prospect's rows alone; its 4.5 GB of free disk bounds DOCS (default 500,000).
#   [DOCS=N] [PROSPECT_BUDGET_MB=N] tools/vm/bsd_rows.sh
set -euo pipefail
tree=$(cd "$(dirname "$0")/../.." && pwd)
bsd=${BSD_VM:-$HOME/code/Kronuz/strata/tools/vm/freebsd.sh}
linux=${GATE_VM:-$HOME/code/Kronuz/strata/tools/vm/linux.sh}
src=${JOB_REF_MAIN:-$tree}
commit=$(git -C "$src" rev-parse HEAD)
docs=${DOCS:-500000}
budget=${PROSPECT_BUDGET_MB:-1039}
rank=${ORACLE_RANK:-$HOME/code/Kronuz/strata-oracle/vectors/out/rank.jsonl}
out=${JOB_OUT:-$tree/.scratch}/bsd
w=p3r-bsd
mkdir -p "$out"
[ "$("$bsd" status)" = running ] || "$bsd" start >&2
[ "$("$linux" status)" = running ] || "$linux" start >&2
"$bsd" ssh "mkdir -p ~/$w"
if ! "$bsd" ssh "test -f ~/$w/src/COMMIT && test \"\$(cat ~/$w/src/COMMIT)\" = $commit"; then
	tar -C "$src" --exclude ./.git --exclude ./.scratch --exclude ./.campaign -cf - . \
		| "$bsd" ssh "rm -rf ~/$w/src && mkdir -p ~/$w/src && tar -xf - -C ~/$w/src && echo $commit > ~/$w/src/COMMIT"
fi
"$linux" ssh "head -n $docs ~/bench/search-benchmark-game/corpus.json" | "$bsd" ssh "cat > ~/$w/corpus.json"
"$linux" ssh 'cat ~/bench/search-benchmark-game/queries.txt' | "$bsd" ssh "cat > ~/$w/queries.txt"
"$bsd" ssh "cat > ~/$w/rank.jsonl" < "$rank"
rc=0
"$bsd" ssh "cd ~/$w && DOCS=$docs BUDGET=$budget sh -s" <<'SH' || rc=$?
set -eu
die() { echo "ASSERT: $*"; exit 1; }
b=build-$(cut -c1-12 src/COMMIT)
rm -rf out && mkdir out
echo "setup: prospect $(cat src/COMMIT), $DOCS documents, budget $BUDGET MiB"
echo "host: $(uname -srm), $(sysctl -n hw.ncpu) CPUs, $(sysctl -n hw.model), $(($(sysctl -n hw.physmem) / 1048576)) MiB"
echo "toolchain: $(clang++ --version | head -1); $(cmake --version | head -1)"
echo "disk: $(df -h . | awk 'NR == 2 {print $4 " free of " $2}')"
[ "$(grep -c . corpus.json)" = "$DOCS" ] || die "corpus.json has $(grep -c . corpus.json) documents, not $DOCS"
[ -f "$b/CMakeCache.txt" ] || cmake -S src -B "$b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > out/cmake.log 2>&1 \
	|| { tail -30 out/cmake.log; die "configure failed"; }
cmake --build "$b" -j4 --target prospect_engine parity > out/build.log 2>&1 || { tail -40 out/build.log; die "build failed"; }
file "$b/prospect_engine" | tee out/engine.file
rm -rf idx merged && mkdir idx
PROSPECT_BUDGET_MB=$BUDGET /usr/bin/time -l -o out/index.time "$b/prospect_engine" index idx < corpus.json > out/index.log 2>&1 \
	|| { tail -20 out/index.log; die "index failed"; }
n=$(grep -o 'index: [0-9]* documents' out/index.log | awk '{print $2}')
[ "$n" = "$DOCS" ] || die "indexed $n documents of $DOCS"
bytes() { find "$1" -type f -exec stat -f %z {} + | awk '{n += $1} END {print n}'; }
echo "index: $(head -1 out/index.time | xargs), max RSS $(awk '/maximum resident/ {print $1}' out/index.time) KB, $(grep -o '[0-9]* segments' out/index.log | tail -1), $(bytes idx) bytes" | tee out/index.txt
"$b/prospect_engine" verify idx < queries.txt > out/verify-idx.txt 2>&1 || { tail -5 out/verify-idx.txt; die "verify failed"; }
tail -1 out/verify-idx.txt
grep -q ' 0 mismatches' out/verify-idx.txt || die "verify before merging has mismatches"
cp -R idx merged
PROSPECT_REORDER_THREADS=4 /usr/bin/time -l -o out/merge.time "$b/prospect_engine" merge merged > out/merge.log 2>&1 || { cat out/merge.log; die "merge failed"; }
grep -q 'merged: 1 segments, reordered' out/merge.log || die "the merge did not leave one reordered segment"
echo "merge: $(head -1 out/merge.time | xargs), max RSS $(awk '/maximum resident/ {print $1}' out/merge.time) KB" | tee out/merge.txt
"$b/prospect_engine" verify merged < queries.txt > out/verify-merged.txt 2>&1 || { tail -5 out/verify-merged.txt; die "verify failed"; }
tail -1 out/verify-merged.txt
grep -q ' 0 mismatches' out/verify-merged.txt || die "verify after merging has mismatches"
"$b/parity" corpus.json 10000 rank.jsonl 4 | tee out/parity-4.txt
PARITY_DELETES=7 "$b/parity" corpus.json 10000 rank.jsonl 16 | tee out/parity-deletes-16.txt
rm -rf idx merged corpus.json
echo "bsd rows passed"
SH
"$bsd" ssh "tar -C ~/$w/out -cf - ." | tar -xf - -C "$out"
echo "bsd_rows: prospect $commit, exit $rc, files in $out" >&2
exit "$rc"
