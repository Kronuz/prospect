#!/usr/bin/env python3
"""Revert check for prospect/operators.hh: applies one mutant at a time, rebuilds operators_test and
expects it to fail. The header is restored after each. A survivor, or a mutant that does not build,
is an error.

  ops_mutants.py BUILD_DIR            (nice -n 19 and one build job unless NICE=0 / JOBS=n)
"""

import os
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = os.path.join(root, "include/prospect/operators.hh")

# (name, old, new)
MUTANTS = [
    ("SYNONYM df: floor, not nearest", "docs) * (1.0 - miss) + 0.5)", "docs) * (1.0 - miss))"),
    ("SYNONYM df: the independence estimate drops the last term", "for (auto df : dfs) { miss *= 1.0 - double(df) / double(docs); }", "for (std::size_t i = 0; i + 1 < dfs.size(); ++i) { miss *= 1.0 - double(dfs[i]) / double(docs); }"),
    ("SYNONYM wdf: not capped at the length", "std::min<std::uint64_t>(wdf, length));\n		return weight_.kind", "wdf);\n		return weight_.kind"),
    ("SYNONYM wdf: the last term's alone", "if (it->doc() == doc_) { wdf += it->wdf(); }", "if (it->doc() == doc_) { wdf = it->wdf(); }"),
    ("SYNONYM bound: halved", "* (1.0 + 0x1p-40);\n	}", "* 0.5;\n	}"),
    ("SYNONYM: a document needs every term", "for (auto& it : its_) { d = std::min(d, it->doc()); }", "for (auto& it : its_) { d = std::max(d, it->doc()); }"),
    ("XOR: an even number of children", "return holding % 2 == 1; }", "return holding % 2 == 0; }"),
    ("XOR: the best child's score, not the sum", "if (c->doc() == d) { sum += c->score(); }", "if (c->doc() == d) { sum = std::max(sum, c->score()); }"),
    ("XOR bound: the largest child, not the sum", "for (const auto& c : children_) { bound_ += c->max_score_total(); }", "for (const auto& c : children_) { bound_ = std::max(bound_, c->max_score_total()); }"),
    ("MAX: the children's sum", "if (c->doc() == d) { best = std::max(best, c->score()); }", "if (c->doc() == d) { best += c->score(); }"),
    ("MAX bound: the smallest child", "for (const auto& c : children_) { bound_ = std::max(bound_, c->max_score_total()); }", "bound_ = 1e300; for (const auto& c : children_) { bound_ = std::min(bound_, c->max_score_total()); }"),
    ("MAX: prunes children at twice the minimum", "for (auto& c : children_) { c->set_min_competitive_score(s); }", "for (auto& c : children_) { c->set_min_competitive_score(s * 2); }"),
    ("SCALE: the factor is not applied", "return factor_ == 0.0 ? 0.0 : x * factor_;", "return x;"),
    ("SCALE: a negative factor is accepted", "if (!(factor >= 0.0)) {", "if (false) {"),
    ("SCALE: a factor that is not a number is accepted", "if (!(factor >= 0.0)) {", "if (factor < 0.0) {"),
    ("SCALE: the minimum is passed down unscaled", "child_->set_min_competitive_score(std::nextafter(std::nextafter(s / factor_, 0.0), 0.0));", "child_->set_min_competitive_score(s);"),
    ("SCALE: the minimum is passed down without the rounding margin", "child_->set_min_competitive_score(std::nextafter(std::nextafter(s / factor_, 0.0), 0.0));", "child_->set_min_competitive_score(s / factor_);"),
    ("ELITE_SET: the lightest children are kept", "return children[a].key > children[b].key;", "return children[a].key < children[b].key;"),
    ("ELITE_SET: the default size is nine", "default_elite_size = 10;", "default_elite_size = 9;"),
    ("ELITE_SET: a size at the count drops a child", "if (children.size() > size) {", "if (children.size() >= size) {"),
    ("ELITE_SET: ties keep the later child", "std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return children[a].key > children[b].key; });", "std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return children[a].key > children[b].key || (children[a].key == children[b].key && a > b); });"),
    ("source: every value is accepted", "if (source_->accept(std::string_view(slot_.value(d)))) { return d; }", "if (true) { return d; }"),
    ("source: a source that gives no max_weight is bounded at zero", "bound_ = m > 0.0 ? m : std::numeric_limits<double>::infinity();", "bound_ = m;"),
    ("source: advance moves backwards", "if (started_ && doc_ >= t) { return doc_; }\n		return doc_ = scan(t);", "return doc_ = scan(t);"),
    ("source: the first ordinal is skipped", "next() override { return doc_ = scan(started_ ? doc_ + 1 : 0); }", "next() override { return doc_ = scan(started_ ? doc_ + 1 : 1); }"),
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
            built = run(["cmake", "--build", build, "--target", "operators_test", "-j", jobs])
            if built.returncode:
                print(f"ERROR   {name}: does not build\n{built.stdout[-800:]}{built.stderr[-800:]}")
                bad += 1
                continue
            test = run([os.path.join(build, "operators_test")])
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
