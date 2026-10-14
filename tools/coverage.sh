#!/usr/bin/env bash
# Source-based coverage as TESTING specifies: every test (T1 at STRATA_CRASH_POINTS, 2,000
# by default), parity against the oracle's ranks, and verify over a sample index, in a
# build configured with -fprofile-instr-generate -fcoverage-mapping. Prints each header's
# line and branch coverage.
#   COV_BUILD=dir COV_CORPUS=corpus.json COV_DOCS=10000 COV_RANKS=rank.jsonl \
#   COV_QUERIES=queries.txt tools/coverage.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
B="${COV_BUILD:-$root/.scratch/build-cov}"
LLVM="${LLVM_BIN:-/usr/local/opt/llvm/bin}"
: "${COV_CORPUS:?a corpus in the harness's JSON lines}" "${COV_RANKS:?the oracle's rank.jsonl}" "${COV_QUERIES:?queries, one per line}"
docs="${COV_DOCS:-10000}"
if [[ ! -f "$B/build.ninja" ]]; then
	cmake -S "$root" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
		-DCMAKE_CXX_FLAGS="-O1 -fprofile-instr-generate -fcoverage-mapping" -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
fi
cmake --build "$B"
rm -rf "$B/prof" "$B/cov-idx" && mkdir -p "$B/prof" "$B/cov-idx"
export LLVM_PROFILE_FILE="$B/prof/%p-%m.profraw"
(cd "$B" && STRATA_CRASH_POINTS="${STRATA_CRASH_POINTS:-2000}" ctest -j4 | grep -E "tests passed|Failed")
"$B/parity" "$COV_CORPUS" "$docs" "$COV_RANKS" | tail -1
head -n "$docs" "$COV_CORPUS" | "$B/strata_engine" index "$B/cov-idx" | tail -1
"$B/strata_engine" verify "$B/cov-idx" < "$COV_QUERIES" | tail -1
unset LLVM_PROFILE_FILE
"$LLVM/llvm-profdata" merge -sparse "$B"/prof/*.profraw -o "$B/all.profdata"
objs=()
for b in "$B"/*_test "$B"/fuzz_*_replay "$B/parity" "$B/strata_engine"; do objs+=(${objs:+-object} "$b"); done
"$LLVM/llvm-cov" report "${objs[@]}" -instr-profile="$B/all.profdata" 2>/dev/null | grep -E "include/strata|TOTAL" |
	awk '{n = split($1, a, "/"); printf "%-18s lines %7s  branches %7s\n", a[n], $10, $13}'
