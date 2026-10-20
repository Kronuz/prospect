#!/usr/bin/env bash
# Source-based coverage as TESTING specifies: every test (T1 at PROSPECT_CRASH_POINTS, 2,000
# by default), parity against the oracle's ranks, and verify over a sample index, in a
# build configured with -fprofile-instr-generate -fcoverage-mapping. Prints each header's
# line and branch coverage.
#   COV_BUILD=dir COV_CORPUS=corpus.json COV_DOCS=10000 COV_RANKS=rank.jsonl \
#   COV_QUERIES=queries.txt tools/coverage.sh
# COV_SINGLE_BYTE=1 builds with single-byte coverage (a flag per region, no counter
# expressions) in its own directory. COV_CONFIRM=dir confirms this build's saturated counts
# with that single-byte build's run; COV_LIST=1 lists uncovered lines and outcomes.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sbc=""
[[ "${COV_SINGLE_BYTE:-}" == 1 ]] && sbc=" -mllvm -enable-single-byte-coverage=true"
B="${COV_BUILD:-$root/.scratch/build-cov${sbc:+-sbc}}"
LLVM="${LLVM_BIN:-/usr/local/opt/llvm/bin}"
: "${COV_CORPUS:?a corpus in the harness's JSON lines}" "${COV_RANKS:?the oracle's rank.jsonl}" "${COV_QUERIES:?queries, one per line}"
docs="${COV_DOCS:-10000}"
if [[ ! -f "$B/build.ninja" ]]; then
	cmake -S "$root" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
		-DCMAKE_CXX_FLAGS="-O1 -fprofile-instr-generate -fcoverage-mapping$sbc" -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
fi
cmake --build "$B"
rm -rf "$B/prof" "$B/cov-idx" && mkdir -p "$B/prof" "$B/cov-idx"
export LLVM_PROFILE_FILE="$B/prof/%p-%m.profraw"
(cd "$B" && PROSPECT_CRASH_POINTS="${PROSPECT_CRASH_POINTS:-2000}" ctest -j4 | grep -E "tests passed|Failed")
"$B/parity" "$COV_CORPUS" "$docs" "$COV_RANKS" | tail -1
head -n "$docs" "$COV_CORPUS" | "$B/prospect_engine" index "$B/cov-idx" | tail -1
"$B/prospect_engine" verify "$B/cov-idx" < "$COV_QUERIES" | tail -1
unset LLVM_PROFILE_FILE
"$LLVM/llvm-profdata" merge -sparse "$B"/prof/*.profraw -o "$B/all.profdata"
# Union over the binaries: llvm-cov report keeps one binary's counters for an inline
# function several instantiate, so a check one test reaches could read as unreached.
binaries() { for b in "$1"/*_test "$1"/fuzz_*_replay "$1/parity" "$1/prospect_engine"; do echo "$b"; done; }
args=("$LLVM" "$B/all.profdata")
while read -r b; do args+=("$b"); done < <(binaries "$B")
if [[ -n "${COV_CONFIRM:-}" ]]; then
	args+=(--confirm "$COV_CONFIRM/all.profdata")
	while read -r b; do args+=("$b"); done < <(binaries "$COV_CONFIRM")
fi
[[ "${COV_LIST:-}" == 1 ]] && args+=(--list)
python3 "$root/tools/coverage_union.py" "${args[@]}"
