#!/usr/bin/env bash
# Copies the baseline engines and strata's engine into a search-benchmark-game checkout.
#   bench/engines/install.sh HARNESS
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
harness="${1:?usage: install.sh HARNESS}"
for e in tantivy-0.26-st lucene-10.4.0-st xapian-2.1.0; do
	mkdir -p "$harness/engines/$e"
	(cd "$here/$e" && tar cf - .) | tar xf - -C "$harness/engines/$e"
done
mkdir -p "$harness/engines/strata"
cp "$here/../phase2/engine/Makefile" "$here/../phase2/engine/details.json" "$harness/engines/strata/"
