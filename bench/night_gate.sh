#!/bin/bash
# Phase 3's indexing gate rows at 5M, in the gate VM under tools/vm/quiet_trials.sh
# (TRIALS='~/bench/night_gate.sh NAME'), every step skipping what it finished:
#   1. T4, one thread: prospect against one-thread Tantivy and Lucene, keyed, at the 5M
#      equal-memory budget (results/calibrate-n2-5m.out), five trials.
#   2. T4, eight threads: prospect at 8 against Tantivy's and Lucene's multi-threaded builders.
#   3. T3 write amplification at that budget, append-only and with replaces.
# DRY=1 shrinks every step to 20,000 documents (still five trials), under NAME-dry.
#   night_gate.sh NAME
set -u
name=${1:?usage: night_gate.sh NAME}
dry=${DRY:-0}
[ "$dry" = 1 ] && name=$name-dry
log() { echo "$(date +%T) $*"; }
res=~/bench/results
for s in index_trials.sh write_amp.sh; do [ -x ~/bench/$s ] || { log "missing ~/bench/$s"; exit 1; }; done
cal=$res/calibrate-n2-5m.out
budget=$(awk '/^BUDGET [0-9]+$/ {print $2}' "$cal")
[ -n "$budget" ] || { log "no 5M budget in $cal"; exit 1; }
docs=all
[ "$dry" = 1 ] && docs=20000
log "T4 one thread at ${budget} MB"
KEYED=1 DOCS=$docs PROSPECT_BUDGET_MB=$budget ENGINES="prospect tantivy-0.26-st lucene-10.4.0-st" \
	~/bench/index_trials.sh "$name-t4-1" > "$res/night-$name-t4-1.log" 2>&1 || exit 1
log "T4 eight threads"
KEYED=1 DOCS=$docs PROSPECT_BUDGET_MB=$budget PROSPECT_THREADS=8 ENGINES="prospect tantivy-0.26 lucene-10.4.0" \
	~/bench/index_trials.sh "$name-t4-8" > "$res/night-$name-t4-8.log" 2>&1 || exit 1
for mode in append replace; do
	[ -f "$res/wamp-$name-$mode.done" ] && continue
	log "T3 write amplification, $mode"
	r=0; [ "$mode" = replace ] && r=1
	DOCS=$docs REPLACES=$r PROSPECT_BUDGET_MB=$budget ENGINES="prospect lucene-10.4.0-st tantivy-0.26-st" \
		~/bench/write_amp.sh "$name-$mode" > "$res/night-$name-wamp-$mode.log" 2>&1 && touch "$res/wamp-$name-$mode.done" || exit 1
done
log NIGHT_DONE
echo TRIALS_DONE
