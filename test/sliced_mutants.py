#!/usr/bin/env python3
"""Revert check for sliced search (slice.hh, search.hh, searcher.hh, shards.hh): applies one mutant at a time, rebuilds sliced_test and
expects it to fail. The header is restored after each. A survivor, or a mutant that does not build,
is an error.

  sliced_mutants.py BUILD_DIR [NAME_FRAGMENT ...]   (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/search.hh")

# (name, old, new[, file]); each old text occurs once in its file (search.hh by default).
MUTANTS = [
    # Yields: a scorer or driver that stops awaiting the slice stops yielding.
    ("disjunction: no checkpoint per window", "	for (;;) {\n		co_await sl.checkpoint();\n		// Clauses that can still matter", "	for (;;) {\n		// Clauses that can still matter"),
    ("conjunction: no checkpoint per lead block", "		window_gate gate;\n		for (ordinal_t d = lead.next(); d != no_more_docs;) {\n			if (gate.due(d)) { co_await sl.checkpoint(); }", "		for (ordinal_t d = lead.next(); d != no_more_docs;) {"),
    ("term: no checkpoint per block", "			if (gate.due(d)) { co_await sl.checkpoint(); }\n			min_ = c.min_competitive();", "			min_ = c.min_competitive();"),
    ("term groups: no checkpoint per group", "			co_await sl.checkpoint();  // a group is 32 blocks\n", ""),
    ("count: no checkpoint per window", "	for (ordinal_t d = s.next(); d != no_more_docs; d = s.doc()) {\n		co_await sl.checkpoint();", "	for (ordinal_t d = s.next(); d != no_more_docs; d = s.doc()) {"),
    ("top_k: no checkpoint in the generic loop", "	for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {\n		if (gate.due(d)) { co_await sl.checkpoint(); }\n		++n;\n		if (heap.size() == k", "	for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {\n		++n;\n		if (heap.size() == k"),
    ("top_k: no checkpoint in the unordered loop", "		for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {\n			if (gate.due(d)) { co_await sl.checkpoint(); }\n			++n;", "		for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {\n			++n;"),
    ("disjunction: counting copy runs unsliced", "c.count += co_await count_task(*copy, sl);", "c.count += co_await count_task(*copy, slice{});", ),
    ("exclusion: intersection count runs unsliced", "co_return *n - co_await count_task(common, sl);", "co_return *n - co_await count_task(common, slice{});"),
    ("search: no checkpoint in the multi-segment generic loop", "		for (ordinal_t d = q->next(); d != no_more_docs; d = q->next()) {\n			if (gate.due(d)) { co_await sl.checkpoint(); }", "		for (ordinal_t d = q->next(); d != no_more_docs; d = q->next()) {", "include/prospect/searcher.hh"),
    ("exhaustive: no checkpoint in the scan", "			if (gate.due(d)) { co_await sl.checkpoint(); }\n			++count_;", "			++count_;", "include/prospect/shards.hh"),
    # Identity: a resume must continue where the search stopped.
    ("exhaustive: a resume drops the match", "			if (gate.due(d)) { co_await sl.checkpoint(); }\n			++count_;", "			if (gate.due(d)) { co_await sl.checkpoint(); continue; }\n			++count_;", "include/prospect/shards.hh"),
    ("top_k: a resume drops the match", "		if (gate.due(d)) { co_await sl.checkpoint(); }\n		++n;\n		if (heap.size() == k", "		if (gate.due(d)) { co_await sl.checkpoint(); continue; }\n		++n;\n		if (heap.size() == k"),
    # Cancellation and the pin.
    ("slice: a set token is not observed", "if (budget_) { a.inner.emplace(budget_->checkpoint()); }", "if (budget_ && !budget_->token().requested()) { a.inner.emplace(budget_->checkpoint()); }", "include/prospect/slice.hh"),
    ("slice: never checkpoints", "if (budget_) { a.inner.emplace(budget_->checkpoint()); }", "", "include/prospect/slice.hh"),
    ("pinned: the view is released before the search", "	Pin held = std::move(pin);\n", "	Pin held = std::move(pin);\n	held = Pin{};\n", "include/prospect/shards.hh"),
    ("pinned: the view is never released", "	Pin held = std::move(pin);\n", "	[[maybe_unused]] Pin* held = new Pin(std::move(pin));\n", "include/prospect/shards.hh"),
]


def run(cmd, **kw):
    prefix = [] if os.environ.get("NICE") == "0" else ["nice", "-n", "19"]
    return subprocess.run(prefix + cmd, capture_output=True, text=True, **kw)


def main(build: str) -> int:
    jobs = os.environ.get("JOBS", "1")
    only = sys.argv[2:]
    bad = 0
    for name, old, new, *rest in MUTANTS:
        if only and not any(o in name for o in only):
            continue
        path = os.path.join(root, rest[0]) if rest else header
        original = open(path).read()
        if original.count(old) != 1:
            print(f"ERROR   {name}: the text to mutate occurs {original.count(old)} times")
            bad += 1
            continue
        backup = path + ".orig"
        shutil.copy(path, backup)
        try:
            open(path, "w").write(original.replace(old, new))
            built = run(["cmake", "--build", build, "--target", "sliced_test", "-j", jobs])
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                bad += 1
                continue
            test = run([os.path.join(build, "sliced_test")])
        finally:
            shutil.copy(backup, path)
            os.remove(backup)
        if test.returncode:
            print(f"caught  {name}: {(test.stderr.strip().splitlines() or ['exit ' + str(test.returncode)])[0][:110]}")
        else:
            print(f"SURVIVED {name}")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
