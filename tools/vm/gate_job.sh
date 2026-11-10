#!/usr/bin/env bash
# Runs one bench/gate3.sh step on strata's Linux gate VM for a campaign job: ships this
# checkout's bench/ to the VM's ~/bench and the engine's source ($JOB_REF_MAIN when the job
# names a main ref, else this checkout) to ~/libs/p3r-COMMIT, runs the step there, and copies
# its files to $JOB_OUT/STEP (default .scratch/gate3/STEP). The exit status is the step's.
#   [DOCS=N] [PROSPECT_BUDGET_MB=N] [PROSPECT_THREADS=N] [KEEP=1] [WARMUP_TIME=S] tools/vm/gate_job.sh STEP [ARG...]
set -euo pipefail
tree=$(cd "$(dirname "$0")/../.." && pwd)
vm=${GATE_VM:-$HOME/code/Kronuz/strata/tools/vm/linux.sh}
src=${JOB_REF_MAIN:-$tree}
commit=$(git -C "$src" rev-parse HEAD)
lib=p3r-${commit:0:12}
out=${JOB_OUT:-$tree/.scratch/gate3}/$1
mkdir -p "$out"
[ "$("$vm" status)" = running ] || "$vm" start >&2
tar -C "$tree" -cf - bench | "$vm" ssh 'tar -xmf - -C ~'
if ! "$vm" ssh "test -f ~/libs/$lib/COMMIT"; then
	tar -C "$src" --exclude ./.git --exclude ./.scratch --exclude ./.campaign -cf - . \
		| "$vm" ssh "rm -rf ~/libs/$lib && mkdir -p ~/libs/$lib && tar -xf - -C ~/libs/$lib && echo $commit > ~/libs/$lib/COMMIT"
fi
fwd=""
for v in DOCS PROSPECT_BUDGET_MB PROSPECT_THREADS PROSPECT_REORDER_THREADS KEEP WARMUP_TIME NUM_ITER COMMANDS; do
	[ -n "${!v:-}" ] && fwd+="$v=$(printf %q "${!v}") "
done
run=$(date -u +%Y%m%dT%H%M%SZ)-$$
rc=0
"$vm" ssh "mkdir -p ~/p3r/out/$run && cd ~/p3r/out/$run && ${fwd}OUT=\$HOME/p3r/out/$run PROSPECT_SRC=\$HOME/libs/$lib bash ~/bench/gate3.sh $(printf '%q ' "$@")" < /dev/null || rc=$?
"$vm" ssh "tar -C ~/p3r/out/$run -cf - ." | tar -xf - -C "$out"
echo "gate_job: prospect $commit, step $*, exit $rc, files in $out" >&2
exit "$rc"
