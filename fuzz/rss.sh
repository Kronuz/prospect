#!/usr/bin/env bash
# Runs a command (a libFuzzer target) and samples its RSS: one line per interval to RSS_LOG,
# "<seconds> <rss MiB> <peak MiB>". Usage: RSS_LOG=rss.log RSS_EVERY=30 fuzz/rss.sh CMD ARGS...
set -u
log=${RSS_LOG:-rss.log}
every=${RSS_EVERY:-30}
"$@" &
pid=$!
start=$(date +%s)
peak=0
: >"$log"
while kill -0 "$pid" 2>/dev/null; do
	kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
	if [ -n "$kb" ]; then
		mb=$((kb / 1024))
		[ "$mb" -gt "$peak" ] && peak=$mb
		echo "$(($(date +%s) - start)) $mb $peak" >>"$log"
	fi
	sleep "$every" &
	wait $! 2>/dev/null
done
wait "$pid"
status=$?
echo "exit $status, peak ${peak} MiB" >>"$log"
exit "$status"
