#!/usr/bin/env bash
# Runs the gate VM's benchmark trials only while the host is quiet: waits until the
# 1-minute load average stays below LIMIT for QUIET_SECONDS, starts ~/bench/trials.sh in
# the VM, and samples host load every 30 s while it runs; NOISY consecutive samples above
# LIMIT (default 3, so 90 s) stop the trials, and it waits for quiet again and starts over.
# One sample is not enough: when the VM goes idle between units, its CPU drops at once while
# the 1-minute load average lags, which reads as a spike. The default LIMIT of 6 sits above
# the host's idle load (2.5-3.5 on 16 threads) and below a compile's (8 and up). TRIALS
# names another VM script to run the same way (bench/ab.sh, for one): a script path and
# arguments, not an environment assignment, and one that does not exec another script,
# since stopping it looks the script up by its own path.
#   [TRIALS='~/bench/ab.sh NAME A B'] tools/vm/quiet_trials.sh [LIMIT] [QUIET_SECONDS]
set -uo pipefail
LIMIT=${1:-6}
QUIET=${2:-300}
NOISY=${NOISY:-3}
TRIALS=${TRIALS:-'~/bench/trials.sh'}
script=$(basename "${TRIALS%% *}")  # the VM script TRIALS runs, stopped on noise
here=$(cd "$(dirname "$0")" && pwd)
log=${STRATA_TRIALS_LOG:-$here/../../.scratch/quiet-trials.log}
vm_pid=$(cat "$here/../../.scratch/linux-vm/qemu.pid" 2>/dev/null)
# Host load from everything but the VM: the trials' own threads (up to its vCPUs) are not noise.
load() {
	local l q
	l=$(sysctl -n vm.loadavg | awk '{print $2}')
	q=$( [ -n "$vm_pid" ] && ps -o %cpu= -p "$vm_pid" 2>/dev/null || echo 0)
	awk -v l="$l" -v q="${q:-0}" 'BEGIN { x = l - q / 100; printf "%.2f\n", (x < 0 ? 0 : x) }'
}
above() { awk -v l="$1" -v m="$LIMIT" 'BEGIN { exit !(l > m) }'; }
# Other agents' work stays off the host while the trials hold it (`quiet`, where installed).
quiet_sh=${QUIET_SH:-$(command -v quiet || echo :)}
trap '"$quiet_sh" release strata-trials' EXIT
for attempt in $(seq 1 200); do
	"$quiet_sh" release strata-trials  # its own hold must not block its next attempt
	"$quiet_sh" wait
	"$quiet_sh" hold strata-trials 720 "strata: gate VM trials ($TRIALS)" $$
	quiet_since=$(date +%s)
	while true; do
		if above "$(load)"; then quiet_since=$(date +%s); fi
		[ $(( $(date +%s) - quiet_since )) -ge "$QUIET" ] && break
		sleep 15
	done
	echo "$(date) attempt $attempt: quiet, starting" >> "$log"
	"$here/linux.sh" ssh 'rm -f ~/bench/results/trials.log; setsid nohup '"$TRIALS"' > ~/bench/results/trials.log 2>&1 < /dev/null &'
	noisy=0 over=0
	until "$here/linux.sh" ssh 'grep -q TRIALS_DONE ~/bench/results/trials.log' 2>/dev/null; do
		l=$(load)
		echo "$(date +%s) $l" >> "$log.samples"
		if above "$l"; then over=$((over + 1)); else over=0; fi
		if [ "$over" -ge "$NOISY" ]; then
			noisy=1
			# The trials script leads its own session: stop it and everything it started.
			"$here/linux.sh" ssh 'g=$(pgrep -f "^/bin/bash /home/strata/bench/'"$script"'" | head -1); [ -n "$g" ] && kill -TERM -- -$g; sleep 2; pkill -f "strata_engine|do_query|java|xapian_engine"' || true
			break
		fi
		sleep 30
	done
	if [ "$noisy" = 0 ]; then echo "$(date) attempt $attempt: clean" >> "$log"; echo CLEAN; exit 0; fi
	echo "$(date) attempt $attempt: load exceeded $LIMIT, stopped; waiting for quiet" >> "$log"
done
echo "$(date) gave up after 200 noisy attempts" >> "$log"
exit 1
