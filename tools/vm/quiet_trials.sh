#!/usr/bin/env bash
# Runs the gate VM's benchmark trials only while the host is quiet: waits until the
# 1-minute load average stays below LIMIT for QUIET_SECONDS, starts ~/bench/trials.sh in
# the VM, and samples host load every 30 s while it runs; a sample above LIMIT stops the
# trials, and it waits for quiet again and starts over.
#   tools/vm/quiet_trials.sh [LIMIT] [QUIET_SECONDS]
set -uo pipefail
LIMIT=${1:-1.5}
QUIET=${2:-300}
here=$(cd "$(dirname "$0")" && pwd)
log=${STRATA_TRIALS_LOG:-$here/../../.scratch/quiet-trials.log}
load() { sysctl -n vm.loadavg | awk '{print $2}'; }
above() { awk -v l="$1" -v m="$LIMIT" 'BEGIN { exit !(l > m) }'; }
for attempt in $(seq 1 20); do
	quiet_since=$(date +%s)
	while true; do
		if above "$(load)"; then quiet_since=$(date +%s); fi
		[ $(( $(date +%s) - quiet_since )) -ge "$QUIET" ] && break
		sleep 15
	done
	echo "$(date) attempt $attempt: quiet, starting" >> "$log"
	"$here/linux.sh" ssh 'rm -f ~/bench/results/trials.log; (nohup ~/bench/trials.sh > ~/bench/results/trials.log 2>&1 &)'
	noisy=0
	until "$here/linux.sh" ssh 'grep -q TRIALS_DONE ~/bench/results/trials.log' 2>/dev/null; do
		l=$(load)
		echo "$(date +%s) $l" >> "$log.samples"
		if above "$l"; then
			noisy=1
			"$here/linux.sh" ssh 'pkill -f "^/bin/bash /home/strata/bench/trials.sh"; pkill -f "^python3 src/client.py"; sleep 2; pkill -f "strata_engine serve|do_query|java"' || true
			break
		fi
		sleep 30
	done
	if [ "$noisy" = 0 ]; then echo "$(date) attempt $attempt: clean" >> "$log"; echo CLEAN; exit 0; fi
	echo "$(date) attempt $attempt: load exceeded $LIMIT, stopped; waiting for quiet" >> "$log"
done
echo "$(date) gave up after 20 noisy attempts" >> "$log"
exit 1
