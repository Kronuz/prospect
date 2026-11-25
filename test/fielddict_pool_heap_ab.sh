#!/usr/bin/env bash
# The pool reader's heap A/B (docs/perf/fielddict-pool-reader.md): fielddict_pool_heap built at a
# base commit (`base`), at HEAD (`after`) and at the base again (`aa`, the A/A), then run
# interleaved. Two steps, so a job builds under light and measures under strict:
#   fielddict_pool_heap_ab.sh build DIR BASE   configure and build DIR/build, then the three binaries
#   fielddict_pool_heap_ab.sh run DIR [ROUNDS] SEGMENTS per side, each FIELDS (4, 10) and ARM
set -euo pipefail
cmd=$1 dir=$2
here="$(cd "$(dirname "$0")/.." && pwd)"
case "$cmd" in
build)
	base=$3
	cmake -G Ninja -S "$here" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	ninja -C "$dir/build" fielddict_test >/dev/null
	rm -rf "$dir/base-src"
	mkdir -p "$dir/base-src"
	git -C "$here" archive "$base" include | tar -x -C "$dir/base-src"
	"$here/test/fielddict_pool_heap.sh" "$dir/build" "$dir/base-src" "$dir/base"
	"$here/test/fielddict_pool_heap.sh" "$dir/build" "$dir/base-src" "$dir/aa"
	"$here/test/fielddict_pool_heap.sh" "$dir/build" "$here" "$dir/after"
	{ echo "base $base"; echo "after $(git -C "$here" rev-parse HEAD)"; } >"$dir/commits"
	;;
run)
	rounds=${3:-9} segments=${4:-1000}
	cat "$dir/commits"
	for r in $(seq 1 "$rounds"); do
		for side in base after aa; do
			for f in 4 10; do
				for a in open check query; do
					echo "side=$side $("$dir/$side" "$f" "$segments" "$a" 15)"
				done
			done
		done
	done | python3 -c '
import collections, re, statistics, sys
rows = collections.defaultdict(list)
for line in sys.stdin:
    m = re.match(r"side=(\w+) fields=(\d+) segments=(\d+) arm=(\w+) heap_per_segment=([\d.]+) open_ns_median=(\d+)", line)
    s, f, n, a, h, t = m.groups()
    rows[(int(f), a, s)].append((float(h), float(t)))
print("fields arm   side   heap B/segment   open ns/segment median (min..max over runs)")
for (f, a, s), v in sorted(rows.items(), key=lambda kv: (kv[0][0], ["open", "check", "query"].index(kv[0][1]), ["base", "after", "aa"].index(kv[0][2]))):
    t = [x[1] for x in v]
    print(f"{f:6d} {a:6s} {s:6s} {v[0][0]:14.1f}   {statistics.median(t):8.0f} ({min(t):.0f}..{max(t):.0f})")
'
	;;
*)
	echo "usage: fielddict_pool_heap_ab.sh build|run DIR ..." >&2
	exit 2
	;;
esac
