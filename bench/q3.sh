#!/bin/bash
# Phase 3 query trials over the engines that build on the VM. Not exec, so this script
# stays the session leader quiet_trials.sh stops.
ENGINES="prospect tantivy-0.26 lucene-10.4.0 lucene-10.4.0-bp xapian-2.1.0" ~/bench/query_trials.sh p3 "$@"
