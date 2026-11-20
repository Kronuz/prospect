#!/usr/bin/env python3
"""Revert check for the term expansion (expand.hh, fielddict.hh's field_cursor, searcher.hh's
hashed columns): applies one mutant at a time, rebuilds expand_test and expects it to fail. Each
file is restored after its mutant. Exit 0 iff every selected mutant was caught; 1 when one
survived or its text is missing; 2 when the unmutated build, or a mutant's build, fails.

  expand_mutants.py BUILD_DIR [NAME_FRAGMENT ...]   (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, old, new); each old text occurs once in its file.
MUTANTS = [
    ("liveness: terms with no live document are selected", "include/prospect/expand.hh",
     "if (it->second.live || (deletes && !detail::has_live(s, info))) { return true; }",
     "if (it->second.live) { return true; }"),
    ("liveness: a single posting's deleted ordinal counts as live", "include/prospect/expand.hh",
     "if (info.flags & format::single_posting) { return !dead(ordinal_t(info.postings)); }",
     "if (info.flags & format::single_posting) { return true; }"),
    ("limits: the limit is one term short", "include/prospect/expand.hh",
     "if (!capped || live <= limit.max) { return true; }",
     "if (!capped || live < limit.max) { return true; }"),
    ("first: the smallest term is dropped past the limit", "include/prospect/expand.hh",
     "const auto last = std::prev(merged.end());",
     "const auto last = merged.begin();"),
    ("most_frequent: ties by reverse term order", "include/prospect/expand.hh",
     "std::stable_sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first; });",
     "std::reverse(ranked.begin(), ranked.end());\n\t\tstd::stable_sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first; });"),
    ("walk: a seek's passed values are not decoded", "include/prospect/fielddict.hh",
     "auto visit = [this](std::string_view v) { pass(v); };",
     "auto visit = [this](std::string_view v) { at_ = v.data(); };"),
    ("walk: the info uses the run after the entry", "include/prospect/fielddict.hh",
     "format::term_run run = before_;",
     "format::term_run run = run_;"),
    ("pool: the walk runs past its field", "include/prospect/fielddict.hh",
     "on_ = found && c_->key().starts_with(prefix_);",
     "on_ = found;"),
    ("columns: put keeps the first segment's info only", "include/prospect/searcher.hh",
     "if (index_.find(key{field, term}) == index_.end()) { add(field, term, std::move(infos)); }",
     "if (index_.find(key{field, term}) == index_.end()) {\n\t\t\tfor (std::size_t i = 1; i < infos.size(); ++i) { infos[i].reset(); }\n\t\t\tadd(field, term, std::move(infos));\n\t\t}"),
    ("corrupt: a match's df is not checked", "include/prospect/reader.hh",
     "				if (info.df > doc_count_) { corrupt(\"document frequency\"); }\n				return info;",
     "				return info;"),
]


def run(cmd, **kw):
    prefix = [] if os.environ.get("NICE") == "0" else ["nice", "-n", "19"]
    return subprocess.run(prefix + cmd, capture_output=True, text=True, errors="replace", **kw)


def main(build: str) -> int:
    jobs = os.environ.get("JOBS", "1")
    only = sys.argv[2:]
    built = run(["cmake", "--build", build, "--target", "expand_test", "-j", jobs])
    if built.returncode:
        print(f"ERROR   the unmutated build fails\n{built.stdout[-800:]}{built.stderr[-800:]}")
        return 2
    bad = 0
    for name, rel, old, new in MUTANTS:
        if only and not any(o in name for o in only):
            continue
        path = os.path.join(root, rel)
        original = open(path).read()
        if original.count(old) != 1:
            print(f"ERROR   {name}: the text to mutate occurs {original.count(old)} times in {rel}")
            bad += 1
            continue
        backup = path + ".orig"
        shutil.copy(path, backup)
        try:
            open(path, "w").write(original.replace(old, new))
            built = run(["cmake", "--build", build, "--target", "expand_test", "-j", jobs])
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                return 2
            try:
                test = run([os.path.join(build, "expand_test")], timeout=120)
            except subprocess.TimeoutExpired:
                test = subprocess.CompletedProcess([], 124, "", "timed out after 120 s")
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
