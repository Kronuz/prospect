#!/usr/bin/env python3
"""Revert check for the phrase expansion (phrase_expand.hh): applies one mutant at a time, rebuilds
phrase_expansion_test and expansion_bounds_test and expects at least one to fail. Each file is
restored after its mutant. Exit 0 iff every selected mutant was caught; 1 when one survived or its
text is missing; 2 when the unmutated build, or a mutant's build, fails.

  phrase_expansion_mutants.py BUILD_DIR [NAME_FRAGMENT ...]   (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H = "include/prospect/phrase_expand.hh"
TESTS = ["phrase_expansion_test", "expansion_bounds_test"]

# (name, file, old, new); each old text occurs once in its file.
MUTANTS = [
    ("query: literal words count toward max_expansions", H,
     "				terms = expand_word(segs, i, words[i], left, o.max_expansions, o.walk);\n				if (left) { *left -= terms.size(); }",
     "				terms = expand_word(segs, i, words[i], left, o.max_expansions, o.walk);\n				if (left) { *left -= terms.size(); }\n			}\n			if (left && words[i].how == phrase_word::literal && *left) {\n				--*left;"),
    ("query: the terms a word selected are not subtracted", H,
     "				if (left) { *left -= terms.size(); }",
     "				(void)0;"),
    ("query: a result past what is left is not refused", H,
     "		if (left && out.size() > *left) { throw phrase_expansion_error(index, true, total); }",
     "		(void)0;"),
    ("word: the query's bound is named when the word's is tighter or equal", H,
     "		const bool query = room < own;",
     "		const bool query = room <= own;"),
    ("word: the word's own limit is dropped", H,
     "		expansion_limit eff = w.limit;",
     "		expansion_limit eff = {w.limit.type, 0};"),
    ("word: first and most_frequent are not seen past what is left", H,
     "eff.type == expansion_limit::error ? std::max<std::uint64_t>(room, 1) : room + 1;",
     "eff.type == expansion_limit::error ? std::max<std::uint64_t>(room, 1) : room;"),
    ("word: an empty query budget reads as none", H,
     "std::max<std::uint64_t>(room, 1)",
     "room"),
    ("word: the error names the previous word", H,
     "			throw phrase_expansion_error(index, query, query ? total : e.max);",
     "			throw phrase_expansion_error(index ? index - 1 : 0, query, query ? total : e.max);"),
    ("word: the walk's charge is not offered the glob's matcher", H,
     "automata::glob(w.text, w.glob_flags, walk.charge)",
     "automata::glob(w.text, w.glob_flags)"),
    ("word: a fuzzy word's transpositions are always on", H,
     "automata::levenshtein(w.text, w.distance, w.fixed, w.swaps, walk.charge)",
     "automata::levenshtein(w.text, w.distance, w.fixed, true, walk.charge)"),
    ("word: a wildcard's flags are ignored", H,
     "automata::glob(w.text, w.glob_flags, walk.charge)",
     "automata::glob(w.text, 3, walk.charge)"),
    ("slot: a term the snapshot holds is dropped from a literal word's slot", H,
     "				if (table.df(field, words[i].text)) { terms.push_back(words[i].text); }",
     "				if (table.df(field, words[i].text) > 1) { terms.push_back(words[i].text); }"),
    ("slot: SYNONYM's weight is taken from the first term's df", H,
     "				w.push_back(weigh(synonym_df(dfs, table.stats().doc_count)));",
     "				w.push_back(weigh(dfs.empty() ? 0 : dfs[0]));"),
    ("slot: an OR slot weighs every term as the first", H,
     "				for (const auto df : dfs) { w.push_back(weigh(df)); }",
     "				for (const auto df : dfs) { (void)df; w.push_back(weigh(dfs[0])); }"),
    ("segment: a slot's terms are read from the first term only", H,
     "				one.terms.push_back(*info);",
     "				if (one.terms.empty()) { one.terms.push_back(*info); }"),
    ("segment: a slot with no term in the segment does not stop it", H,
     "			if (one.terms.empty()) { return nullptr; }",
     "			if (one.terms.empty()) { continue; }"),
    ("segment: NEAR's window is dropped", H,
     "		return std::make_unique<multi_phrase_scorer>(inv, std::move(slots), window, offsets);",
     "		return std::make_unique<multi_phrase_scorer>(inv, std::move(slots), 0, offsets);"),
    ("segment: offsets are dropped", H,
     "		return std::make_unique<multi_phrase_scorer>(inv, std::move(slots), window, offsets);",
     "		return std::make_unique<multi_phrase_scorer>(inv, std::move(slots), window);"),
    ("key: the key field is accepted", H,
     "if (s.key_field() && s.key_field() == s.field_id(field)) { throw",
     "if (false) { throw"),
]


def run(cmd, **kw):
    prefix = [] if os.environ.get("NICE") == "0" else ["nice", "-n", "19"]
    return subprocess.run(prefix + cmd, capture_output=True, text=True, errors="replace", **kw)


def build(b: str, jobs: str):
    return run(["cmake", "--build", b, "--target", *TESTS, "-j", jobs])


def main(b: str) -> int:
    jobs = os.environ.get("JOBS", "1")
    only = sys.argv[2:]
    built = build(b, jobs)
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
            built = build(b, jobs)
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                return 2
            failed = None
            for t in TESTS:
                try:
                    test = run([os.path.join(b, t)], timeout=300)
                except subprocess.TimeoutExpired:
                    test = subprocess.CompletedProcess([], 124, "", "timed out after 300 s")
                if test.returncode:
                    failed = (t, (test.stderr.strip().splitlines() or ["exit " + str(test.returncode)])[0][:100])
                    break
        finally:
            shutil.copy(backup, path)
            os.remove(backup)
        if failed:
            print(f"caught  {name}: {failed[0]}: {failed[1]}")
        else:
            print(f"SURVIVED {name}")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
