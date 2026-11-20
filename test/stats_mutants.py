#!/usr/bin/env python3
"""Revert check for the supplied statistics in prospect/searcher.hh: applies one mutant at a time,
rebuilds stats_test and expects it to fail. The header is restored after each. A survivor, or a
mutant that does not build, is an error; the exit status is 0 iff every selected mutant was caught.

  stats_mutants.py BUILD_DIR [NAME_FRAGMENT ...]   (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/searcher.hh")

# (name, old, new); each old text occurs once in the header.
MUTANTS = [
    ("ignored: the collection statistics are the snapshot's", "st_(supplied ? supplied->collection : snapshot_statistics(segs))", "st_(snapshot_statistics(segs))"),
    ("ignored: the term frequencies are the snapshot's", "		if (supplied_) {\n			try {", "		if (false) {\n			try {"),
    ("ignored: the whole input", "segs_(segs), supplied_(supplied), st_(supplied ? supplied->collection : snapshot_statistics(segs))", "segs_(segs), supplied_(nullptr), st_(snapshot_statistics(segs))"),
    ("collection: the document count only", "st_(supplied ? supplied->collection : snapshot_statistics(segs))", "st_(supplied ? statistics(supplied->collection.doc_count, snapshot_statistics(segs).total_length) : snapshot_statistics(segs))"),
    ("collection: the total length only", "st_(supplied ? supplied->collection : snapshot_statistics(segs))", "st_(supplied ? statistics(snapshot_statistics(segs).doc_count, supplied->collection.total_length) : snapshot_statistics(segs))"),
    ("terms: the document frequency is the snapshot's", "				c.df = t.df;\n				c.cf = t.cf;", "				c.cf = t.cf;"),
    ("terms: the collection frequency is the snapshot's", "				c.df = t.df;\n				c.cf = t.cf;", "				c.df = t.df;"),
    ("sum: the collection's document count is not added", "collection.doc_count += o.collection.doc_count;", ""),
    ("sum: the collection's total length is not added", "collection.total_length += o.collection.total_length;", ""),
    ("sum: a term's document frequency is replaced", "mine.df += t.df;", "mine.df = t.df;"),
    ("sum: a term's collection frequency is not added", "mine.cf += t.cf;", ""),
    ("sum: terms only the other side has are dropped", "auto& mine = terms[field][term];", "auto f = terms.find(field);\n				if (f == terms.end() || f->second.find(term) == f->second.end()) { continue; }\n				auto& mine = terms[field][term];"),
    ("gather: deleted documents are not counted", "t.df += info->df;", "t.df += info->df - (s.any_deleted() && info->df > 1 ? 1 : 0);"),
    ("gather: the collection frequency is the document frequency", "t.cf += info->cf;", "t.cf += info->df;"),
    ("find: the field is ignored", "const auto f = terms.find(field);\n		if (f == terms.end()) { return nullptr; }\n		const auto t = f->second.find(term);\n		return t == f->second.end() ? nullptr : &t->second;", "for (const auto& [name, by_term] : terms) {\n			const auto t = by_term.find(term);\n			if (t != by_term.end()) { return &t->second; }\n		}\n		return nullptr;"),
    ("refusal: a lacking term weighs by the snapshot's own", "if (local.df) { throw std::invalid_argument(\"prospect: supplied statistics lack a term the snapshot holds\"); }\n	return {};", "return local;"),
    ("refusal: the table keeps the refused column", "index_.erase(at);\n				columns_.pop_back();\n				throw;", "throw;"),
    ("per-segment frequencies follow the supplied ones", "const auto& e = column(field, term).infos[index_of(seg)];\n		return e ? e->df : 0;", "(void)index_of(seg);\n		return column(field, term).df;"),
]


def run(cmd, **kw):
    prefix = [] if os.environ.get("NICE") == "0" else ["nice", "-n", "19"]
    return subprocess.run(prefix + cmd, capture_output=True, text=True, **kw)


def main(build: str) -> int:
    jobs = os.environ.get("JOBS", "1")
    only = sys.argv[2:]
    bad = 0
    for name, old, new in MUTANTS:
        if only and not any(o in name for o in only):
            continue
        original = open(header).read()
        if original.count(old) != 1:
            print(f"ERROR   {name}: the text to mutate occurs {original.count(old)} times")
            bad += 1
            continue
        backup = header + ".orig"
        shutil.copy(header, backup)
        try:
            open(header, "w").write(original.replace(old, new))
            built = run(["cmake", "--build", build, "--target", "stats_test", "-j", jobs])
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                bad += 1
                continue
            test = run([os.path.join(build, "stats_test")])
        finally:
            shutil.copy(backup, header)
            os.remove(backup)
        if test.returncode:
            print(f"caught  {name}: {(test.stderr.strip().splitlines() or ['exit ' + str(test.returncode)])[0][:110]}")
        else:
            print(f"SURVIVED {name}")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
