#!/bin/bash
# T3 write amplification on Linux: bytes the device wrote while each engine indexed, over
# the index's final bytes, counted the same way for every engine (the block device's
# sectors written in /proc/diskstats, so commits, merges, and metadata all count). Run on a
# quiet VM: anything else writing to the device counts too. Append-only, and with
# REPLACES=1 a stream where half the documents replace an earlier one by id (keyed
# engines). Results in ~/bench/results/wamp-NAME.
#   STRATA_BUDGET_MB=N [ENGINES="strata lucene-10.4.0-st tantivy-0.26-st"] [DOCS=1000000|all] [REPLACES=1] write_amp.sh NAME
set -u
name=${1:-run}
cd ~/bench/search-benchmark-game
out=~/bench/results/wamp-$name
mkdir -p "$out"
export PATH=$HOME/.cargo/bin:$PATH KEYED=1
# Equal memory (T3): strata runs on the node budget whose peak RSS matches the reference
# engine's (bench/calibrate_rss.sh); buffer settings are not comparable across engines.
[ -n "${STRATA_BUDGET_MB:-}" ] || { echo "STRATA_BUDGET_MB is required: run calibrate_rss.sh"; exit 1; }
export STRATA_BUDGET_MB
docs=${DOCS:-1000000}
if [ "$docs" = all ]; then corpus=$PWD/corpus.json; else
	[ -f corpus-$docs.json ] || head -n "$docs" corpus.json > corpus-$docs.json
	corpus=$PWD/corpus-$docs.json
fi
if [ "${REPLACES:-0}" = 1 ]; then
	# Every second line repeats the id of an earlier document, so it replaces it.
	[ -f corpus-$docs-replace.json ] || python3 - "$corpus" "$PWD/corpus-$docs-replace.json" <<'PY'
import json, random, sys
random.seed(7)
src, dst = sys.argv[1], sys.argv[2]
ids = []
with open(src) as f, open(dst, "w") as o:
    for n, line in enumerate(f):
        d = json.loads(line)
        if n % 2 and ids:
            d["id"] = random.choice(ids)
        else:
            ids.append(d["id"])
        o.write(json.dumps(d) + "\n")
PY
	corpus=$PWD/corpus-$docs-replace.json
fi
device=$(lsblk -no PKNAME "$(df --output=source ~ | tail -1)" 2>/dev/null || basename "$(df --output=source ~ | tail -1)")
written() { awk -v d="$device" '$3 == d { print $10 * 512 }' /proc/diskstats; }
read -r -a engines <<< "${ENGINES:-strata lucene-10.4.0-st tantivy-0.26-st}"
[ -s "$corpus" ] || { echo "empty corpus $corpus"; exit 1; }
failed=0
for e in "${engines[@]}"; do
	[ -f "$out/$e.result" ] && continue
	dir=~/wamp-$e; rm -rf "$dir" && mkdir -p "$dir"
	case $e in
	strata) cmd=(engines/strata/build/strata_engine index "$dir") ;;
	tantivy-0.26-st) cmd=(engines/tantivy-0.26-st/target/release/build_index "$dir") ;;
	lucene-10.4.0-st) cmd=(java -server --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED -cp "engines/lucene-10.4.0-st/build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:engines/lucene-10.4.0-st/build/dependencies/*" BuildIndex "$dir") ;;
	*) echo "unknown engine $e"; continue ;;
	esac
	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
	before=$(written)
	"${cmd[@]}" < "$corpus" > "$out/$e.log" 2>&1 || { echo "UNIT_FAILED $e"; failed=1; continue; }
	sync
	after=$(written)
	final=$(find "$dir" -type f -exec stat -c%s {} + | awk '{n += $1} END {print n}')
	echo "$e written $((after - before)) final $final amplification $(awk -v w=$((after - before)) -v f="$final" 'BEGIN { printf "%.3f", w / f }')" | tee "$out/$e.result"
	rm -rf "$dir"
done
[ "$failed" = 0 ] || exit 1
echo WAMP_DONE
