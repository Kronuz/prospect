#!/usr/bin/env bash
# Copies the baseline engines and prospect's engine into a search-benchmark-game checkout.
#   bench/engines/install.sh HARNESS
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
harness="${1:?usage: install.sh HARNESS}"
for e in tantivy-0.26-st lucene-10.4.0-st xapian-2.1.0 prospect-merged; do
	mkdir -p "$harness/engines/$e"
	(cd "$here/$e" && tar cf - .) | tar xf - -C "$harness/engines/$e"
done
mkdir -p "$harness/engines/prospect"
cp "$here/../phase2/engine/Makefile" "$here/../phase2/engine/details.json" "$harness/engines/prospect/"
# The harness's own IResearch and PISA engines: their sources are submodules. PISA
# builds natively; IResearch's Makefile builds inside Docker, installed when missing.
git -C "$harness" submodule update --init --recursive engines/pisa-0.8.2/pisa
git -C "$harness" -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init engines/iresearch-26.03.1/serenedb
git -C "$harness/engines/iresearch-26.03.1/serenedb" -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive
if ! command -v docker >/dev/null; then
	case "$(uname -s)" in
	Linux) sudo apt-get install -y docker.io && sudo usermod -aG docker "$USER" ;;
	*) echo "IResearch needs Docker" >&2 ;;
	esac
fi
