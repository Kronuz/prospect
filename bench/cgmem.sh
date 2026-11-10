#!/bin/bash
# Runs CMD in its own cgroup v2 and records its memory (BENCHMARKS.md, "Memory"): the peaks of
# memory.current and of memory.stat's anon, file, and file_mapped, sampled every 0.2 s, and the
# kernel's memory.peak. stdin passes through to CMD, so a corpus piped from a process outside
# the cgroup is not charged to it. Linux with cgroup v2 and passwordless sudo.
#   cgmem.sh OUT CMD...    OUT gets "peak current anon file file_mapped" in bytes; exit is CMD's
set -u
out=$1; shift
cg=/sys/fs/cgroup/cgmem-$$
sudo mkdir "$cg" || exit 1
# The child moves itself into the cgroup before it execs CMD; <&0 keeps stdin, which a
# background command would otherwise read from /dev/null.
bash -c 'echo $$ | sudo tee "$0/cgroup.procs" > /dev/null && exec "$@"' "$cg" "$@" <&0 &
pid=$!
cur=0 anon=0 file=0 mapped=0
while kill -0 "$pid" 2>/dev/null; do
	read -r c a f m < <(awk -v c="$(cat "$cg/memory.current" 2>/dev/null || echo 0)" \
		'$1 == "anon" {a = $2} $1 == "file" {f = $2} $1 == "file_mapped" {m = $2} END {print c, a + 0, f + 0, m + 0}' \
		"$cg/memory.stat" 2>/dev/null)
	[ "${c:-0}" -gt "$cur" ] && cur=$c
	[ "${a:-0}" -gt "$anon" ] && anon=$a
	[ "${f:-0}" -gt "$file" ] && file=$f
	[ "${m:-0}" -gt "$mapped" ] && mapped=$m
	sleep 0.2
done
wait "$pid"; rc=$?
peak=$(cat "$cg/memory.peak" 2>/dev/null || echo 0)
echo "$peak $cur $anon $file $mapped" > "$out"
sudo rmdir "$cg" 2>/dev/null || true
exit "$rc"
