#!/usr/bin/env python3
"""Revert check for prospect/shards.hh: applies one mutant at a time, rebuilds shards_test and
expects it to fail. The header is restored after each. A survivor, or a mutant that does not build,
is an error.

  shards_mutants.py BUILD_DIR [NAME_FRAGMENT ...]   (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/shards.hh")

# (name, old, new); each old text occurs once.
MUTANTS = [
    ("interleave: shard-major numbering", "return (did - 1) * shards + shard + 1;", "return shard * 1000000 + did;"),
    ("interleave: off by one", "return (did - 1) * shards + shard + 1;", "return (did - 1) * shards + shard;"),
    ("interleave: no overflow check", "if (did - 1 > (std::numeric_limits<std::uint64_t>::max() - shard - 1) / shards) { throw std::overflow_error(\"prospect: interleaved docid overflows\"); }", ""),
    ("split: shard from the wrong end", "return {std::size_t((docid - 1) % shards), (docid - 1) / shards + 1};", "return {std::size_t(shards - 1 - (docid - 1) % shards), (docid - 1) / shards + 1};"),
    ("statistics: the first shard's only", "statistics stats() const { return snapshot_statistics(all_); }", "statistics stats() const { return snapshot_statistics(segments(0)); }"),
    ("document frequency: the first shard's only", "return snapshot_df(all_, term, key_prefix); }", "return snapshot_df(segments(0), term, key_prefix); }"),
    ("last docid: the smallest, not the largest", "best = std::max(best, interleave_docid(last, i, shards_.size()));", "best = best ? std::min(best, interleave_docid(last, i, shards_.size())) : interleave_docid(last, i, shards_.size());"),
    ("last docid: the shard's own, not interleaved", "best = std::max(best, interleave_docid(last, i, shards_.size()));", "best = std::max(best, last);"),
    ("length bounds: the lower bound is the largest minimum", "b.lower = any ? std::min(b.lower, inv.min_length()) : inv.min_length();", "b.lower = any ? std::max(b.lower, inv.min_length()) : inv.min_length();"),
    ("length bounds: empty segments count", "if (!s.seg->core().doc_count()) { continue; }", ""),
    ("pruned merge: shard-local docids break ties", "out.hits.push_back({interleave_docid(h.docid, s, shards), h.score});", "out.hits.push_back({h.docid, h.score});"),
    ("pruned merge: ties break by the larger docid", "return a.score != b.score ? a.score > b.score : a.docid < b.docid; };\n	if (out.hits.size() > o.k)", "return a.score != b.score ? a.score > b.score : a.docid > b.docid; };\n	if (out.hits.size() > o.k)"),
    ("pruned merge: counts only the last shard", "total += n;", "total = n;"),
    ("pruned merge: each shard keeps one hit fewer", "search(snap.segments(s), build, o.k, o.count ? &n : nullptr)", "search(snap.segments(s), build, o.k ? o.k - 1 : 0, o.count ? &n : nullptr)"),
    ("exhaustive: the key is ignored", "if (o_.key) {\n			const int c = a.key.compare(b.key);", "if (false) {\n			const int c = a.key.compare(b.key);"),
    ("exhaustive: the key sorts descending", "return o_.reverse_key ? c > 0 : c < 0; }\n		}\n		if (a.score", "return o_.reverse_key ? c < 0 : c > 0; }\n		}\n		if (a.score"),
    ("exhaustive: equal keys break by docid before relevance", "if (a.score != b.score) { return a.score > b.score; }\n		return o_.order", "return o_.order == docid_order::descending ? a.docid > b.docid : a.docid < b.docid;\n		if (a.score != b.score) { return a.score > b.score; }\n		return o_.order"),
    ("exhaustive: relevance ascending", "if (a.score != b.score) { return a.score > b.score; }\n		return o_.order", "if (a.score != b.score) { return a.score < b.score; }\n		return o_.order"),
    ("exhaustive: descending docids ignored", "return o_.order == docid_order::descending ? a.docid > b.docid : a.docid < b.docid;\n	}", "return a.docid < b.docid;\n	}"),
    ("exhaustive: dont_care is descending", "return o_.order == docid_order::descending ? a.docid > b.docid : a.docid < b.docid;\n	}", "return o_.order != docid_order::ascending ? a.docid > b.docid : a.docid < b.docid;\n	}"),
    ("exhaustive: a worse key still enters the full heap", "if (o_.reverse_key ? c < 0 : c > 0) { return; }", ""),
    ("exhaustive: a better key is rejected from the full heap", "if (o_.reverse_key ? c < 0 : c > 0) { return; }", "if (o_.reverse_key ? c > 0 : c < 0) { return; }"),
    ("exhaustive: keeps one hit fewer", "const bool full = heap_.size() == o_.k;", "const bool full = heap_.size() + 1 == o_.k;"),
    ("exhaustive: deleted documents are matches", "if (snap.any_deleted()) { q = std::make_unique<live_scorer>(std::move(q), *snap.deleted); }\n		match m;", "match m;"),
    ("exhaustive: the count skips the first match of a segment", "for (ordinal_t d = q->next(); d != no_more_docs; d = q->next()) {\n			++count_;", "for (ordinal_t d = q->next(); d != no_more_docs; d = q->next()) {\n			if (d == 0) { continue; }\n			++count_;"),
    ("exhaustive: the visitor misses the last shard's matches", "if (o_.visit) { o_.visit(m); }", "if (o_.visit && shard + 1 < shards) { o_.visit(m); }"),
    ("exhaustive: the visitor sees a match twice", "if (o_.visit) { o_.visit(m); }", "if (o_.visit) { o_.visit(m); if (d % 7 == 3) { o_.visit(m); } }"),
    ("exhaustive: the visitor sees the shard-local docid", "m.docid = interleave_docid(m.did, shard, shards);", "m.docid = m.did;"),
    ("exhaustive: the visitor's document is the previous ordinal", "m.ord = d;\n", "m.ord = d ? d - 1 : d;\n"),
    ("exhaustive: a stale score on the next match", "m.scored_ = false;", ""),
    ("exhaustive: the pruned path serves a key", "bool exhaustive() const { return bool(key) || bool(visit) || order == docid_order::descending; }", "bool exhaustive() const { return bool(visit) || order == docid_order::descending; }"),
    ("exhaustive: the pruned path serves a visitor", "bool exhaustive() const { return bool(key) || bool(visit) || order == docid_order::descending; }", "bool exhaustive() const { return bool(key) || order == docid_order::descending; }"),
]


def run(cmd, **kw):
    prefix = [] if os.environ.get("NICE") == "0" else ["nice", "-n", "19"]
    return subprocess.run(prefix + cmd, capture_output=True, text=True, **kw)


def main(build: str) -> int:
    jobs = os.environ.get("JOBS", "1")
    only = sys.argv[2:]
    original = open(header).read()
    backup = header + ".orig"
    shutil.copy(header, backup)
    bad = 0
    try:
        for name, old, new in MUTANTS:
            if only and not any(o in name for o in only):
                continue
            if original.count(old) != 1:
                print(f"ERROR   {name}: the text to mutate occurs {original.count(old)} times")
                bad += 1
                continue
            open(header, "w").write(original.replace(old, new))
            built = run(["cmake", "--build", build, "--target", "shards_test", "-j", jobs])
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                bad += 1
                continue
            test = run([os.path.join(build, "shards_test")])
            if test.returncode:
                print(f"caught  {name}: {(test.stderr.strip().splitlines() or ['exit ' + str(test.returncode)])[0][:110]}")
            else:
                print(f"SURVIVED {name}")
                bad += 1
    finally:
        shutil.copy(backup, header)
        os.remove(backup)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
