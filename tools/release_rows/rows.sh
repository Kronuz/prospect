#!/usr/bin/env bash
# The release's correctness rows (docs/gates/release-rows.md), one step per campaign job, at the
# composition Xapiand master builds: this checkout's prospect (its include/ must equal the pin's)
# with strata, compressors, hashes and threadpool at Xapiand's pins, from the job's refs
# ($JOB_REF_STRATA, ..., and $JOB_REF_PIN, prospect at Xapiand's pin). Binaries another step
# runs go to $HOME/.scratch/release-rows/COMMIT.
#   rows.sh suite                 prospect at the pin: its whole suite at the release's strata
#   rows.sh build                 this checkout's row tests at the release's strata; parity,
#                                 prospect_engine and stem_parity for the steps below
#   rows.sh parity                tools/parity.cc at 1, 4, 16 segments and with deletes at 4, 16
#   rows.sh verify                prospect_engine verify on a multi-segment 1M index with deletes,
#                                 then force-merged, both query sets
#   rows.sh stems                 every stem language Xapiand's schema accepts against Xapian's
#   rows.sh fuzz-build            the libFuzzer targets (ASan, UBSan) at the release's strata
#   rows.sh fuzz TARGET SEED      one 10-minute run of fuzz_TARGET
set -euo pipefail
tree=$(cd "$(dirname "$0")/../.." && pwd)
step=${1:?usage: rows.sh STEP [ARG...]}
shift
commit=$(git -C "$tree" rev-parse HEAD)
bin=$HOME/.scratch/release-rows/${commit:0:12}
out=${JOB_OUT:-$tree/.scratch/release-rows}
inputs=${ROWS_INPUTS:-$HOME/.scratch/release-rows/inputs}
oracle=${ORACLE:-$HOME/code/Kronuz/strata-oracle}
mkdir -p "$bin" "$out"
log() { echo "$(date +%T) $*"; }
die() { echo "ASSERT: $*" >&2; exit 1; }

# Xapiand master's pins (CMakeLists.txt: strata, prospect, and the libraries it overrides in strata).
declare -A want=(
	[PIN]=850d4e8ce9a76fdf15c8e0028c6fd578a3374d34
	[STRATA]=5d742b2407f5048713cb39a625c017c455cb814e
	[COMPRESSORS]=5566260a30bbd2d77d5cfa890bfe33f503a2add6
	[HASHES]=03ab2d69f7e1b0af6039733e0e06cc1e0118a45e
	[THREADPOOL]=63e03a5deca9c91f08755d900c61885dd5e75822
)
overrides=()
pins() {
	local name dir got
	for name in PIN STRATA COMPRESSORS HASHES THREADPOOL; do
		dir=$(printenv "JOB_REF_$name") || die "no \$JOB_REF_$name: submit with --ref ${name,,}=PATH@${want[$name]}"
		got=$(git -C "$dir" rev-parse HEAD)
		[ "$got" = "${want[$name]}" ] || die "${name,,} is at $got, not Xapiand's pin ${want[$name]}"
		echo "pin ${name,,} $got"
		[ "$name" = PIN ] || overrides+=("-DFETCHCONTENT_SOURCE_DIR_$name=$dir")
	done
	diff -r "$JOB_REF_PIN/include" "$tree/include" > /dev/null || die "include/ differs from prospect ${want[PIN]}"
	echo "prospect $commit: include/ equals ${want[PIN]}'s"
}
sha() { shasum -a 256 "$1" | cut -c1-16; }

suite() {
	local b=$out/build-pin
	cmake -S "$JOB_REF_PIN" -B "$b" -DCMAKE_BUILD_TYPE=Release "${overrides[@]}" > "$out/suite-cmake.log" 2>&1 || { tail -30 "$out/suite-cmake.log"; die "configure failed"; }
	cmake --build "$b" -j4 > "$out/suite-build.log" 2>&1 || { grep -m20 -B2 -A8 'error' "$out/suite-build.log"; die "the pin's suite did not build"; }
	ctest --test-dir "$b" --output-on-failure -j4 --timeout 1800 2>&1 | tee "$out/suite-ctest.log" | tail -40
	grep -q '100% tests passed' "$out/suite-ctest.log" || die "the pin's suite failed"
}

rows=(segment_test aggregate_test operators_test shards_test fuzz_segment_replay fuzz_postings_replay fuzz_values_replay fuzz_fielddict_replay)
build() {
	local b=$out/build-release
	cmake -S "$tree" -B "$b" -DCMAKE_BUILD_TYPE=Release "${overrides[@]}" > "$out/build-cmake.log" 2>&1 || { tail -30 "$out/build-cmake.log"; die "configure failed"; }
	cmake --build "$b" -j4 --target "${rows[@]}" parity prospect_engine > "$out/build.log" 2>&1 || { grep -m20 -B2 -A8 'error' "$out/build.log"; die "the rows did not build"; }
	ctest --test-dir "$b" --output-on-failure -j4 -R '^(segment|aggregate|operators|shards|fuzz_(segment|postings|values|fielddict)_replay)$' 2>&1 | tee "$out/build-ctest.log" | tail -30
	grep -Eq '^100% tests passed(, 0 tests failed)? out of 8$' "$out/build-ctest.log" || die "a row test failed"
	cmake -S "$tree/tools/release_rows" -B "$out/build-stems" -DCMAKE_BUILD_TYPE=Release > "$out/stems-cmake.log" 2>&1 || { tail -30 "$out/stems-cmake.log"; die "stem_parity did not configure"; }
	cmake --build "$out/build-stems" -j4 > "$out/stems-build.log" 2>&1 || { tail -30 "$out/stems-build.log"; die "stem_parity did not build"; }
	cp "$b/parity" "$b/prospect_engine" "$out/build-stems/stem_parity" "$bin/"
	echo "binaries in $bin"
}

parity() {
	local corpus=$inputs/corpus-20000.json rank=$oracle/vectors/out/rank.jsonl segs rc=0
	[ -x "$bin/parity" ] || die "no $bin/parity: run build"
	echo "corpus $(sha "$corpus") $corpus; rank $(sha "$rank") $rank"
	for segs in 1 4 16; do
		"$bin/parity" "$corpus" 10000 "$rank" $segs | tee "$out/parity-$segs.txt" || rc=1
	done
	for segs in 4 16; do
		PARITY_DELETES=7 "$bin/parity" "$corpus" 10000 "$rank" $segs | tee "$out/parity-deletes-$segs.txt" || rc=1
	done
	[ "$rc" = 0 ] || die "a parity pass failed"
}

verify() {
	local corpus=$inputs/corpus-1000000.json idx=$out/idx set n
	[ -x "$bin/prospect_engine" ] || die "no $bin/prospect_engine: run build"
	echo "corpus $(sha "$corpus") $corpus; queries $(sha "$inputs/queries.txt"); terms $(sha "$tree/bench/phase2/terms-stratified.txt")"
	rm -rf "$idx"
	log "index: 1M documents, a 64 MB buffer, every 11th deleted"
	PROSPECT_BUFFER_MB=64 PROSPECT_DELETE_EVERY=11 "$bin/prospect_engine" index "$idx" < "$corpus" 2> "$out/index.log" || { tail "$out/index.log"; die "index failed"; }
	tail -1 "$out/index.log"
	n=$(grep -o '[0-9]* segments' "$out/index.log" | tail -1 | awk '{print $1}')
	[ "${n:-0}" -ge 2 ] || die "the index has ${n:-0} segments, not several"
	for pass in before after; do
		if [ "$pass" = after ]; then
			log "merge"
			PROSPECT_REORDER_THREADS=4 "$bin/prospect_engine" merge "$idx" 2>&1 | tee "$out/merge.log"
			grep -q 'merged: 1 segments' "$out/merge.log" || die "the merge did not leave one segment"
		fi
		for set in "$inputs/queries.txt" "$tree/bench/phase2/terms-stratified.txt"; do
			log "verify $pass $(basename "$set")"
			"$bin/prospect_engine" verify "$idx" < "$set" > "$out/verify-$pass-$(basename "$set")" 2>&1 || { tail -5 "$out/verify-$pass-$(basename "$set")"; die "verify $pass failed"; }
			tail -1 "$out/verify-$pass-$(basename "$set")"
			grep -q ' 0 mismatches' "$out/verify-$pass-$(basename "$set")" || die "verify $pass has mismatches"
		done
	done
	rm -rf "$idx"
}

# Every stem_language Xapiand's schema accepts (src/database/schema.cc, _get_stem_language) and
# the snowball-data vocabulary it is checked over.
stems() {
	local words=${STEM_WORDS:-$HOME/code/Kronuz/snowball/.scratch/oracle-tsv} name voc x s rc=0
	[ -x "$bin/stem_parity" ] || die "no $bin/stem_parity: run build"
	local names="armenian:armenian hy:armenian basque:basque eu:basque catalan:catalan ca:catalan danish:danish da:danish
		dutch:dutch nl:dutch kraaij_pohlmann:dutch english:english en:english earlyenglish:earlymodernenglish
		english_lovins:lovins lovins:lovins english_porter:porter porter:porter finnish:finnish fi:finnish
		french:french fr:french german:german de:german german2:german hungarian:hungarian hu:hungarian
		italian:italian it:italian norwegian:norwegian nb:norwegian nn:norwegian no:norwegian
		portuguese:portuguese pt:portuguese romanian:romanian ro:romanian russian:russian ru:russian
		spanish:spanish es:spanish swedish:swedish sv:swedish turkish:turkish tr:turkish"
	mkdir -p "$out/words"
	for x in $names; do
		name=${x%%:*} voc=${x#*:}
		[ -f "$out/words/$voc" ] || { cut -f1 "$words/$voc.tsv" > "$out/words/$voc"; echo "vocabulary $voc: $(wc -l < "$out/words/$voc") words, $(sha "$words/$voc.tsv")"; }
		x=1 s=1
		"$oracle/vectors/.scratch/oracle" stem "$name" "$out/words/$voc" > "$out/xapian-$name.jsonl" 2> "$out/xapian-$name.err" && x=0
		"$bin/stem_parity" --accepts "$name" > /dev/null && s=0
		if [ "$x" = 0 ] && [ "$s" = 0 ]; then
			"$bin/stem_parity" "$name" < "$out/xapian-$name.jsonl" | tee "$out/stems-$name.txt" | tail -11 || rc=1
		elif [ "$x" = 1 ] && [ "$s" = 1 ]; then
			echo "$name: refused by both ($(head -c 120 "$out/xapian-$name.err" | tr '\n' ' '))"
		else
			rc=1
			echo "DIFF $name: Xapian $([ "$x" = 0 ] && echo stems || echo refuses), snowball $([ "$s" = 0 ] && echo stems || echo refuses)"
			if [ "$x" = 0 ]; then echo "  Xapian: $(head -3 "$out/xapian-$name.jsonl" | tr '\n' ' ')"; else echo "  Xapian: $(head -c 120 "$out/xapian-$name.err")"; fi
		fi
		rm -f "$out/xapian-$name.jsonl"
	done
	rm -rf "$out/words"
	[ "$rc" = 0 ] || die "a language differs"
}

fuzzers=(segment postings values fielddict)
fuzz_build() {
	local b=$out/build-fuzz t
	cmake -S "$tree" -B "$b" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPROSPECT_FUZZ=ON \
		-DCMAKE_C_COMPILER=/usr/local/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/opt/llvm/bin/clang++ "${overrides[@]}" > "$out/fuzz-cmake.log" 2>&1 \
		|| { tail -30 "$out/fuzz-cmake.log"; die "configure failed"; }
	cmake --build "$b" -j4 --target "${fuzzers[@]/#/fuzz_}" > "$out/fuzz-build.log" 2>&1 || { grep -m20 -B2 -A8 'error' "$out/fuzz-build.log"; die "the fuzz targets did not build"; }
	mkdir -p "$bin/fuzz"
	for t in "${fuzzers[@]}"; do cp "$b/fuzz_$t" "$bin/fuzz/"; done
	/usr/local/opt/llvm/bin/clang++ --version | head -1
	echo "fuzz targets in $bin/fuzz"
}

fuzz() {
	local t=${1:?fuzz TARGET SEED} seed=${2:?fuzz TARGET SEED} f=$bin/fuzz/fuzz_$1 rc=0
	[ -x "$f" ] || die "no $f: run fuzz-build"
	local work=$out/corpus-$t-$seed found=$bin/findings/$t-$seed
	mkdir -p "$work" "$found"
	log "fuzz_$t seed $seed: 600 s, -rss_limit_mb=2048, -timeout=10"
	"$f" -max_total_time=600 -timeout=10 -rss_limit_mb=2048 -seed="$seed" -print_final_stats=1 -artifact_prefix="$found/" "$work" "$tree/fuzz/corpus/$t" > "$out/fuzz-$t-$seed.log" 2>&1 || rc=$?
	grep -E '^(#[0-9]+.*DONE|stat::number_of_executed_units|stat::peak_rss_mb|stat::new_units_added)|ERROR|SUMMARY|==[0-9]+==' "$out/fuzz-$t-$seed.log" | head -20
	ls "$found" | sed 's/^/finding: /'
	rmdir "$found" 2> /dev/null || true
	rm -rf "$work"
	[ "$rc" = 0 ] || die "fuzz_$t seed $seed exited $rc (log $out/fuzz-$t-$seed.log)"
}

case $step in
suite) pins; suite ;;
build) pins; build ;;
parity) parity ;;
verify) verify ;;
stems) stems ;;
fuzz-build) pins; fuzz_build ;;
fuzz) fuzz "$@" ;;
*) die "unknown step $step" ;;
esac
log "$step done"
