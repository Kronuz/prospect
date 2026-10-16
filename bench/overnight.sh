#!/bin/bash
# One night of gate rows in the gate VM, every step skipping what it finished, so
# tools/vm/quiet_trials.sh can stop and restart it (run it there with TRIALS='~/bench/overnight.sh NAME'):
#   1. "after merging" query trials: strata force-merged against the baselines' own
#      force-merged indexes (built once; existing ones are reused by marker);
#   2. equal-memory calibration of strata's node budget against Lucene on the 1M corpus;
#   3. T4 indexing rows, keyed, at 1 and 8 strata threads, five trials;
#   4. T3 write amplification at that budget, append-only and with replaces.
# Not exec'd steps, so this script stays the session leader quiet_trials.sh stops; each
# step logs to its own file, since only this script may print TRIALS_DONE.
#   overnight.sh NAME
set -u
name=${1:?usage: overnight.sh NAME}
log() { echo "$(date +%T) $*"; }
for s in query_trials.sh calibrate_rss.sh index_trials.sh write_amp.sh; do  # fail before the first step
	[ -x ~/bench/$s ] || { log "missing ~/bench/$s: copy bench/ to the VM"; exit 1; }
done
res=~/bench/results
q=$res/query-$name-merged
mkdir -p "$q"
for e in tantivy-0.26 lucene-10.4.0 lucene-10.4.0-bp xapian-2.1.0; do  # reuse built baselines
	[ -f "$res/query-p3/index-$e.done" ] && touch "$q/index-$e.done"
done
engines="strata-merged tantivy-0.26 lucene-10.4.0 lucene-10.4.0-bp xapian-2.1.0"
log "query build"
ENGINES="$engines" ~/bench/query_trials.sh "$name-merged" build > "$q/build.log" 2>&1 || exit 1
log "query trials"
ENGINES="$engines" ~/bench/query_trials.sh "$name-merged" > "$q/trials.log" 2>&1 || exit 1
cal=$res/calibrate-$name.out
if ! grep -q "^[0-9]*$" <(tail -1 "$cal" 2>/dev/null); then
	log "calibration"
	KEYED=1 ~/bench/calibrate_rss.sh "$name" lucene-10.4.0-st > "$cal.tmp" 2>&1 && mv "$cal.tmp" "$cal" || exit 1
fi
budget=$(tail -1 "$cal")
log "T4 indexing at ${budget} MB"
KEYED=1 STRATA_BUDGET_MB=$budget ~/bench/index_trials.sh "$name-t4-1" > "$res/overnight-$name-t4-1.log" 2>&1 || exit 1
KEYED=1 STRATA_BUDGET_MB=$budget STRATA_THREADS=8 ~/bench/index_trials.sh "$name-t4-8" > "$res/overnight-$name-t4-8.log" 2>&1 || exit 1
for mode in append replace; do
	[ -f "$res/wamp-$name-$mode.done" ] && continue
	log "T3 write amplification, $mode"
	if [ "$mode" = replace ]; then r=1; else r=0; fi
	REPLACES=$r STRATA_BUDGET_MB=$budget ~/bench/write_amp.sh "$name-$mode" > "$res/overnight-$name-wamp-$mode.log" 2>&1 && touch "$res/wamp-$name-$mode.done" || exit 1
done
log OVERNIGHT_DONE
echo TRIALS_DONE
