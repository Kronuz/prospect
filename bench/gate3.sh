#!/bin/bash
# Phase 3's gate rows on the gate VM, one step per campaign job: tools/vm/gate_job.sh ships
# bench/ and the engine's source, then runs a step here. Each step prints the setup it ran
# with and stops at the first unmet assertion ("ASSERT:"); its files land in $OUT.
#   gate3.sh prepare                      build prospect_engine and the one-thread baselines; print the setup
#   gate3.sh smoke                        every step below end to end at 20,000 documents, the
#                                         baselines' kept indexes left alone
#   gate3.sh unit ENGINE TAG              one keyed indexing run (T4) of DOCS documents (default: the 5M
#                                         corpus), the baselines with NO_MERGE=1: times, peak RSS, cgroup
#                                         peaks, segments, bytes. KEEP=1 keeps it as engines/ENGINE/idx
#   gate3.sh merge                        engines/prospect/idx force-merged into engines/prospect-merged/idx
#   gate3.sh sections                     prospect-merged's sections and bytes, beside keyed Lucene -bp's
#   gate3.sh verify                       prospect_engine verify, before and after merging, both query sets
#   gate3.sh queries TRIAL SET ENGINE...  one query trial over SET (queries, terms-stratified, ...), the
#                                         engine order rotated by TRIAL
#   gate3.sh allocs [IDX [SET...]]        allocations per query by command and class (bench/allocs.py) on
#                                         engines/IDX/idx (prospect-merged), over SETs (queries terms-stratified)
#   gate3.sh servemem ENGINE...           serving memory over the gate queries (bench/serve_mem.py)
#   gate3.sh wamp MODE                    write amplification (T3, bench/write_amp.sh) of DOCS documents
#                                         (default 1,000,000), MODE append or replace, every engine keyed;
#                                         LUCENE_RAM_MB and TANTIVY_HEAP_MB size the baselines' buffers
#   gate3.sh parity                       tools/parity.cc (TREE_SRC's) against the oracle's rank vectors at 1,
#                                         4, and 16 segments, and with deletes (PARITY_DELETES=7) at 4 and 16
# Engines: prospect (PROSPECT_SRC, PROSPECT_BUDGET_MB, PROSPECT_THREADS), tantivy-0.26-st,
# lucene-10.4.0-st, and for queries any harness engine with an index.
set -euo pipefail
step=${1:?usage: gate3.sh STEP [ARG...]}
shift
: "${OUT:?OUT is required}" "${PROSPECT_SRC:?PROSPECT_SRC is required}"
h=~/bench/search-benchmark-game
cd "$h"
export PATH=$HOME/.cargo/bin:$PATH
log() { echo "$(date +%T) $*"; }
die() { echo "ASSERT: $*" >&2; exit 1; }
engine=$h/engines/prospect/build/prospect_engine

corpus() {  # the corpus file for DOCS
	local d=${DOCS:-all}
	if [ "$d" = all ]; then echo "$h/corpus.json"; return; fi
	[ -f "$h/corpus-$d.json" ] || head -n "$d" "$h/corpus.json" > "$h/corpus-$d.json"
	echo "$h/corpus-$d.json"
}

lines_of() {  # FILE: its documents (non-empty lines), cached beside it; a cold read of the 5M corpus takes minutes
	[ "$1.lines" -nt "$1" ] || grep -c . "$1" > "$1.lines"
	cat "$1.lines"
}

segments_of() {  # ENGINE DIR LOG
	case $1 in
	prospect) grep -o '[0-9]* segments' "$3" | tail -1 | awk '{print $1}' ;;
	lucene*) find "$2" -name '*.si' | wc -l ;;
	tantivy*) find "$2" -name '*.idx' | wc -l ;;
	esac
}

setup() {
	log "setup: prospect $(cat "$PROSPECT_SRC/COMMIT") from $PROSPECT_SRC"
	echo "host: $(uname -srm), $(nproc) CPUs, $(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2}'), $(free -m | awk '/Mem:/ {print $2}') MiB"
	echo "toolchain: $(clang++ --version | head -1); $(java -version 2>&1 | head -1); $(cargo --version 2>/dev/null)"
	echo "disk: $(df -h ~ | awk 'NR == 2 {print $4 " free of " $2}')"
	echo "env: DOCS=${DOCS:-all} PROSPECT_BUDGET_MB=${PROSPECT_BUDGET_MB:-} PROSPECT_THREADS=${PROSPECT_THREADS:-1} PROSPECT_AUTO_MERGE=${PROSPECT_AUTO_MERGE:-1} KEEP=${KEEP:-0} WARMUP_TIME=${WARMUP_TIME:-60} NUM_ITER=${NUM_ITER:-10}"
	[ -x "$engine" ] && [ "$(cat "$h/engines/prospect/build/.source" 2>/dev/null)" = "$(cat "$PROSPECT_SRC/COMMIT")" ] \
		|| { [ "$step" = prepare ] || die "prospect_engine is not built from $(cat "$PROSPECT_SRC/COMMIT"): run prepare"; }
}

prepare() {
	export CMAKE_BUILD_PARALLEL_LEVEL=4 CARGO_BUILD_JOBS=4  # the host's build rule: four jobs
	mkdir -p engines/prospect engines/prospect-merged ~/p3r/idx
	cp ~/bench/phase2/engine/Makefile ~/bench/phase2/engine/details.json engines/prospect/
	cp ~/bench/engines/prospect-merged/Makefile ~/bench/engines/prospect-merged/details.json engines/prospect-merged/
	for e in tantivy-0.26-st lucene-10.4.0-st; do (cd ~/bench/engines/$e && tar cf - .) | tar xf - -C engines/$e; done
	local want; want=$(cat "$PROSPECT_SRC/COMMIT")
	# The cmake cache names one source tree: another commit's build starts over.
	[ "$(cat engines/prospect/build/.source 2>/dev/null)" = "$want" ] || rm -rf engines/prospect/build
	PROSPECT_SRC=$PROSPECT_SRC make -s -C engines/prospect compile > "$OUT/compile-prospect.log" 2>&1 || { tail -30 "$OUT/compile-prospect.log"; die "prospect build failed"; }
	echo "$want" > engines/prospect/build/.source
	file "$engine" | tee "$OUT/engine.file"
	grep -q 'x86-64' "$OUT/engine.file" || die "prospect_engine is not an x86-64 binary"
	for e in tantivy-0.26-st lucene-10.4.0-st; do
		make -s -C engines/$e compile > "$OUT/compile-$e.log" 2>&1 || { tail -30 "$OUT/compile-$e.log"; die "$e build failed"; }
	done
	# The "after merging" baselines: their builders force-merged them (one segment each).
	for e in lucene-10.4.0 lucene-10.4.0-bp tantivy-0.26; do
		local n; n=$(segments_of "$e" engines/$e/idx /dev/null)
		echo "query baseline $e: $n segments, $(du -sb engines/$e/idx | cut -f1) bytes"
		[ "$n" = 1 ] || die "$e's index has $n segments, not 1"
	done
	local d
	for d in all 1000000 500000 20000; do echo "corpus $d: $(lines_of "$(DOCS=$d corpus)") documents"; done
}

unit() {  # ENGINE TAG
	local e=$1 tag=$2 c dir res
	local -a cmd
	c=$(corpus)
	dir=~/p3r/idx/$e-$tag
	rm -rf "$dir" && mkdir -p "$dir"
	case $e in
	prospect) cmd=("$engine" index "$dir") ;;
	tantivy-0.26-st) cmd=("$h/engines/$e/target/release/build_index" "$dir") ;;
	lucene-10.4.0-st)
		cmd=(java -server --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
			-cp "$h/engines/$e/build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:$h/engines/$e/build/dependencies/*" BuildIndex "$dir") ;;
	*) die "unit: unknown engine $e" ;;
	esac
	[ "$e" != prospect ] || [ -n "${PROSPECT_BUDGET_MB:-}" ] || die "unit prospect needs PROSPECT_BUDGET_MB"
	res=$OUT/unit-$tag-$e
	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
	log "unit $tag $e: $(lines_of "$c") documents of $(basename "$c")"
	# The corpus comes through a pipe from outside the cgroup, so its pages are not charged.
	cat "$c" | KEYED=1 NO_MERGE=1 ~/bench/cgmem.sh "$res.cg" /usr/bin/time -v -o "$res.time" "${cmd[@]}" > "$res.log" 2>&1 \
		|| { tail -20 "$res.log"; die "unit $tag $e failed"; }
	local wall user sys rss segs bytes docs
	wall=$(awk -F': ' '/Elapsed/ {n = split($2, p, ":"); s = 0; for (i = 1; i <= n; i++) s = s * 60 + p[i]; print s}' "$res.time")
	user=$(awk -F': ' '/User time/ {print $2}' "$res.time")
	sys=$(awk -F': ' '/System time/ {print $2}' "$res.time")
	rss=$(awk -F': ' '/Maximum resident/ {print $2}' "$res.time")
	segs=$(segments_of "$e" "$dir" "$res.log")
	bytes=$(du -sb "$dir" | cut -f1)
	docs=$(lines_of "$c")
	if [ "$e" = prospect ]; then
		docs=$(grep -o 'index: [0-9]* documents' "$res.log" | awk '{print $2}')
		[ "$docs" = "$(lines_of "$c")" ] || die "prospect indexed $docs documents of $(lines_of "$c")"
	fi
	[ "${segs:-0}" -ge 1 ] || die "$e left no segments"
	read -r cg_peak cg_cur cg_anon cg_file cg_mapped < "$res.cg"
	{
		echo "engine=$e tag=$tag docs=$docs wall_s=$wall user_s=$user sys_s=$sys max_rss_kb=$rss"
		echo "cg_peak=$cg_peak cg_current_peak=$cg_cur cg_anon_peak=$cg_anon cg_file_peak=$cg_file cg_file_mapped_peak=$cg_mapped"
		echo "segments=$segs bytes=$bytes budget_mb=${PROSPECT_BUDGET_MB:-} threads=$(case $e in prospect) echo "${PROSPECT_THREADS:-1}" ;; lucene*) echo "${LUCENE_THREADS:-1}" ;; tantivy*) echo "${TANTIVY_THREADS:-1}" ;; esac)"
	} | tee "$res.txt"
	if [ "${KEEP:-0}" = 1 ]; then rm -rf "engines/$e/idx" && mv "$dir" "engines/$e/idx"; else rm -rf "$dir"; fi
}

merge() {
	[ -d engines/prospect/idx ] || die "no engines/prospect/idx: run unit prospect with KEEP=1"
	rm -rf engines/prospect-merged/idx
	cp -a engines/prospect/idx engines/prospect-merged/idx
	export PROSPECT_REORDER_THREADS=${PROSPECT_REORDER_THREADS:-8}
	log "merge: reordering on $PROSPECT_REORDER_THREADS threads"
	/usr/bin/time -v -o "$OUT/merge.time" "$engine" merge engines/prospect-merged/idx > "$OUT/merge.log" 2>&1 || { cat "$OUT/merge.log"; die "merge failed"; }
	cat "$OUT/merge.log"
	grep -q 'merged: 1 segments, reordered' "$OUT/merge.log" || die "the merge did not leave one reordered segment"
	grep -E 'Elapsed|Maximum resident|User time|System time' "$OUT/merge.time"
}

sections() {
	# The merge keeps its sources until they are reclaimed, so the merged segment is the
	# directory's largest file; `sections` reads DIR/segment.sx.
	local d=engines/prospect-merged/idx seg
	ls -la "$d" | tee "$OUT/merged-ls.txt"
	seg=$(ls -S "$d" | head -1)
	rm -rf ~/p3r/sections && mkdir -p ~/p3r/sections && ln -s "$h/$d/$seg" ~/p3r/sections/segment.sx
	echo "segment: $seg"
	"$engine" sections ~/p3r/sections | tee "$OUT/sections.txt"
	grep -q '^file' "$OUT/sections.txt" || die "no sections"
	cp ~/bench/results/keyed-bp-n2.sizes "$OUT/keyed-bp.sizes"
}

verify() {
	local idx set
	for idx in prospect prospect-merged; do
		for set in queries terms-stratified; do
			log "verify $idx $set"
			"$engine" verify engines/$idx/idx < $set.txt > "$OUT/verify-$idx-$set.txt" 2>&1 || { cat "$OUT/verify-$idx-$set.txt"; die "verify $idx $set failed"; }
			tail -2 "$OUT/verify-$idx-$set.txt"
			grep -q ' 0 mismatches' "$OUT/verify-$idx-$set.txt" || die "verify $idx $set has mismatches"
		done
	done
}

queries() {  # TRIAL SET ENGINE...
	local t=$1 set=$2; shift 2
	local -a es=("$@") order=()
	local n=${#es[@]} i
	[ -f "$set.txt" ] || die "no $set.txt in the harness"
	for i in $(seq 0 $((n - 1))); do
		order+=("${es[$(((i + t - 1) % n))]}")
		[ -d "engines/${es[$i]}/idx" ] || die "${es[$i]} has no index"
	done
	log "queries q$t-$set: ${order[*]}"
	COMMANDS=${COMMANDS:-"TOP_10 TOP_100 TOP_1000 TOP_100_COUNT COUNT"} WARMUP_TIME=${WARMUP_TIME:-60} NUM_ITER=${NUM_ITER:-10} \
		python3 src/client.py "$set.txt" "${order[@]}" > "$OUT/q$t-$set.log" 2>&1 || { tail -20 "$OUT/q$t-$set.log"; die "client failed"; }
	cp results.json "$OUT/q$t-$set.json"
	# Counts must agree between every pair of engines on every query and command.
	python3 - "$OUT/q$t-$set.json" <<-'PY'
	import json, sys
	r = json.load(open(sys.argv[1]))["results"]
	bad = 0
	for cmd, per in r.items():
	    if cmd not in ("COUNT", "TOP_100_COUNT"):
	        continue
	    engines = sorted(per)
	    for i, q in enumerate(per[engines[0]]):
	        counts = {e: per[e][i].get("count") for e in engines}
	        if len(set(counts.values())) != 1:
	            bad += 1
	            if bad <= 5:
	                print(f"count mismatch {cmd} {q['query']!r}: {counts}")
	print(f"counts: {bad} mismatches over {len(r)} commands")
	sys.exit(1 if bad else 0)
	PY
}

allocs() {  # [IDX [SET...]]
	local idx=${1:-prospect-merged}; shift || true
	local -a sets=("$@")
	[ ${#sets[@]} -gt 0 ] || sets=(queries terms-stratified)
	clang -O2 -shared -fPIC -o ~/p3r/alloc_count.so ~/bench/alloc_count.c || die "alloc_count.so did not build"
	OUT=$OUT python3 ~/bench/allocs.py ~/p3r/alloc_count.so "$engine" engines/$idx/idx "${sets[@]/%/.txt}" | tee "$OUT/allocs-$idx.md"
	grep -q 'rows grow with the number of matches' "$OUT/allocs-$idx.md" || die "allocs.py did not finish"
}

servemem() {  # ENGINE...
	python3 ~/bench/serve_mem.py "$@" | tee "$OUT/servemem.md"
	[ "$(grep -c '^| [a-z]' "$OUT/servemem.md")" -ge "$#" ] || die "serve_mem.py measured fewer engines than asked"
}

wamp() {  # MODE
	local mode=${1:?wamp MODE: append or replace} name r=0
	[ "$mode" = append ] || [ "$mode" = replace ] || die "wamp: unknown mode $mode"
	[ -n "${PROSPECT_BUDGET_MB:-}" ] || die "wamp needs PROSPECT_BUDGET_MB"
	name=p3r-$mode-$(date -u +%Y%m%dT%H%M%S)
	log "wamp $mode: ${DOCS:-1000000} documents, results/wamp-$name"
	DOCS=${DOCS:-1000000} REPLACES=$([ "$mode" = replace ] && echo 1 || echo 0) bash ~/bench/write_amp.sh "$name" || r=$?
	cp ~/bench/results/wamp-"$name"/* "$OUT"/ 2>/dev/null || true
	[ "$r" = 0 ] || die "write_amp.sh failed"
	grep -h amplification "$OUT"/*.result
}

parity() {
	: "${TREE_SRC:?TREE_SRC is required}" "${ORACLE_RANK_SHA256:?ORACLE_RANK_SHA256 is required}"
	echo "$ORACLE_RANK_SHA256  $HOME/p3r/rank.jsonl" | sha256sum -c - || die "rank vectors are not $ORACLE_RANK_SHA256"
	local b; b=~/p3r/build-tree-$(cut -c1-12 "$TREE_SRC/COMMIT")
	log "parity from $(cat "$TREE_SRC/COMMIT")"
	[ -f "$b/build.ninja" ] || cmake -S "$TREE_SRC" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$OUT/parity-cmake.log" 2>&1 \
		|| { tail -30 "$OUT/parity-cmake.log"; die "configure failed"; }
	CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build "$b" --target parity > "$OUT/parity-build.log" 2>&1 || { tail -40 "$OUT/parity-build.log"; die "parity did not build"; }
	local segs rc=0
	for segs in 1 4 16; do
		"$b/parity" corpus.json 10000 ~/p3r/rank.jsonl $segs | tee "$OUT/parity-$segs.txt" || rc=1
	done
	for segs in 4 16; do
		PARITY_DELETES=7 "$b/parity" corpus.json 10000 ~/p3r/rank.jsonl $segs | tee "$OUT/parity-deletes-$segs.txt" || rc=1
	done
	[ "$rc" = 0 ] || die "a parity run failed"
}

smoke() {
	export DOCS=20000 KEEP=1 PROSPECT_BUDGET_MB=4  # a budget small enough for several segments
	unit prospect smoke
	# The baselines' kept indexes are the 5M unmerged ones the "before merging" rows read.
	for e in tantivy-0.26-st lucene-10.4.0-st; do KEEP=0 unit "$e" smoke; done
	merge
	sections
	local idx set
	for idx in prospect prospect-merged; do
		"$engine" verify engines/$idx/idx < queries-dry.txt > "$OUT/verify-$idx-dry.txt" 2>&1
		tail -1 "$OUT/verify-$idx-dry.txt"
		grep -q ' 0 mismatches' "$OUT/verify-$idx-dry.txt" || die "verify $idx has mismatches"
	done
	WARMUP_TIME=1 NUM_ITER=2 queries 1 queries-dry prospect prospect-merged
	allocs prospect-merged queries-dry
	log "smoke passed"
}

setup
case $step in
prepare) prepare ;;
smoke) smoke ;;
unit) unit "$@" ;;
merge) merge ;;
sections) sections ;;
verify) verify ;;
queries) queries "$@" ;;
allocs) allocs "$@" ;;
servemem) servemem "$@" ;;
wamp) wamp "$@" ;;
parity) parity ;;
*) die "unknown step $step" ;;
esac
log "$step done"
