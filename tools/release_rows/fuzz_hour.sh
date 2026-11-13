#!/usr/bin/env bash
# CPU-hour fuzzing of the decoders the release reads (docs/perf/fuzz-targets.md). Binaries go to
# $HOME/.scratch/fuzz-hour/NAME, the corpora that persist across runs to $HOME/.scratch/fuzz-corpus.
#   fuzz_hour.sh build NAME [SRC]      the four libFuzzer targets (ASan, UBSan) from SRC (default: this
#                                      checkout) at its own pins; NAME "head" is this checkout's commit
#   fuzz_hour.sh cost NAME             each target's CPU per input over its checked-in corpus
#   fuzz_hour.sh run TARGET NAME [CPU_SECONDS]
#                                      TARGET until it has used CPU_SECONDS (default 3600) of CPU time,
#                                      user + system, in as many libFuzzer rounds as it takes; its
#                                      corpus is kept; exits 1 on a finding
set -euo pipefail
tree=$(cd "$(dirname "$0")/../.." && pwd)
step=${1:?usage: fuzz_hour.sh build|cost|run ...}
shift
root=$HOME/.scratch/fuzz-hour
corpora=${FUZZ_CORPORA:-$HOME/.scratch/fuzz-corpus}
clang=${FUZZ_CLANG:-/usr/local/opt/llvm/bin}
targets=(segment postings values fielddict)
# NAME "head" is this checkout's commit, so a binary is never reused across commits.
named() { [ "$1" = head ] && git -C "$tree" rev-parse --short=12 HEAD || echo "$1"; }
log() { echo "$(date +%T) $*"; }
die() { echo "ASSERT: $*" >&2; exit 1; }

# libFuzzer's length limit, from the first input on (-len_control=0): segment inputs are byte-change
# lists of five bytes each, so a limit near the seeds' (26 bytes at ten minutes) allows four changes.
max_len() {
	case $1 in
	segment) echo "${FUZZ_SEGMENT_MAX_LEN:-256}" ;;
	*) echo 0 ;;
	esac
}
len_flags() {
	local n
	n=$(max_len "$1")
	[ "$n" = 0 ] && return 0
	echo "-max_len=$n -len_control=0"
}

# user + system seconds of the /usr/bin/time -p report at the end of a log.
cpu_of() { awk '/^(user|sys) [0-9.]+$/{s+=$2} END{printf "%.2f", s}' "$1"; }

build() {
	local name src=${2:-$tree} b dir
	name=$(named "${1:?build NAME}")
	dir=$root/$name
	b=$dir/build
	mkdir -p "$dir"
	cmake -S "$src" -B "$b" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPROSPECT_FUZZ=ON \
		-DCMAKE_C_COMPILER="$clang/clang" -DCMAKE_CXX_COMPILER="$clang/clang++" > "$dir/cmake.log" 2>&1 \
		|| { tail -30 "$dir/cmake.log"; die "configure failed"; }
	cmake --build "$b" -j4 --target "${targets[@]/#/fuzz_}" > "$dir/build.log" 2>&1 \
		|| { grep -m20 -B2 -A8 'error' "$dir/build.log"; die "the fuzz targets did not build"; }
	for t in "${targets[@]}"; do cp "$b/fuzz_$t" "$dir/"; done
	rm -rf "$b"
	git -C "$src" rev-parse HEAD
	"$clang/clang++" --version | head -1
	echo "fuzz targets in $dir"
}

# One pass of the checked-in corpus under the fuzzer binary, minus a pass over an empty corpus (the
# static bases' build): the least of three, CPU seconds.
cost() {
	local name dir t n best cpu empty seeds
	name=$(named "${1:?cost NAME}")
	dir=$root/$name
	echo "target inputs startup_s corpus_s ms_per_input"
	for t in "${targets[@]}"; do
		[ -x "$dir/fuzz_$t" ] || die "no $dir/fuzz_$t: run build"
		seeds=$tree/fuzz/corpus/$t
		n=$(find "$seeds" -type f | wc -l | tr -d ' ')
		mkdir -p "$dir/empty"
		empty=999999 best=999999
		for _ in 1 2 3; do
			/usr/bin/time -p "$dir/fuzz_$t" -runs=0 "$dir/empty" > "$dir/time.txt" 2>&1 || true
			cpu=$(cpu_of "$dir/time.txt")
			empty=$(echo "$cpu $empty" | awk '{print ($1 < $2) ? $1 : $2}')
			/usr/bin/time -p "$dir/fuzz_$t" -runs=0 -timeout=60 "$seeds" > "$dir/time.txt" 2>&1 || true
			cpu=$(cpu_of "$dir/time.txt")
			best=$(echo "$cpu $best" | awk '{print ($1 < $2) ? $1 : $2}')
		done
		echo "$t $n $empty $best $(echo "$best $empty $n" | awk '{printf "%.2f", ($1 - $2) * 1000 / $3}')"
	done
}

run() {
	local t=${1:?run TARGET NAME [CPU_SECONDS]} name budget=${3:-3600}
	name=$(named "${2:?run TARGET NAME [CPU_SECONDS]}")
	local f=$root/$name/fuzz_$t corpus=$corpora/$t found=$root/findings/$t-$name used=0 round=0 left rc=0 execs=0 news=0 peak=0 flags
	[ -x "$f" ] || die "no $f: run build"
	mkdir -p "$corpus" "$found"
	flags=$(len_flags "$t")
	log "fuzz_$t: $budget CPU seconds, corpus $corpus ($(find "$corpus" -type f | wc -l | tr -d ' ') files), ${flags:-default length limit}"
	while :; do
		left=$(echo "$budget $used" | awk '{printf "%d", $1 - $2}')
		[ "$left" -ge 5 ] || break
		round=$((round + 1))
		# The wall limit is 2% past the CPU left: a fuzzer on an idle host uses 98-100% of a core.
		# shellcheck disable=SC2086
		/usr/bin/time -p "$f" -max_total_time=$((left * 102 / 100 + 1)) -timeout=10 -rss_limit_mb=2048 -seed="$round" $flags -print_final_stats=1 \
			-artifact_prefix="$found/" "$corpus" "$tree/fuzz/corpus/$t" > "$root/$name/$t-$round.log" 2>&1 || rc=$?
		grep -E '^(INFO: -max_len|#[0-9]+ +DONE|stat::)|ERROR|SUMMARY|^(real|user|sys) ' "$root/$name/$t-$round.log" | head -14
		used=$(echo "$used $(cpu_of "$root/$name/$t-$round.log")" | awk '{printf "%.2f", $1 + $2}')
		execs=$((execs + $(awk '/^stat::number_of_executed_units/{print $2}' "$root/$name/$t-$round.log")))
		news=$((news + $(awk '/^stat::new_units_added/{print $2}' "$root/$name/$t-$round.log")))
		peak=$(awk -v p="$peak" '/^stat::peak_rss_mb/{if ($2 > p) p = $2} END{print p}' "$root/$name/$t-$round.log")
		log "round $round exit $rc: $used of $budget CPU s"
		[ "$rc" = 0 ] || break
	done
	local files
	files=$(find "$found" -type f | wc -l | tr -d ' ')
	echo "fuzz_$t: $used CPU s in $round rounds, $execs executions, $news new units, peak RSS $peak MiB, corpus $(find "$corpus" -type f | wc -l | tr -d ' ') files, $files findings"
	find "$found" -type f | sed 's/^/finding: /'
	[ "$rc" = 0 ] && [ "$files" = 0 ] || die "fuzz_$t exited $rc with $files findings in $found"
	rmdir "$found" 2> /dev/null || true
}

case $step in
build) build "$@" ;;
cost) cost "$@" ;;
run) run "$@" ;;
*) die "unknown step $step" ;;
esac
