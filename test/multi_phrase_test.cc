// PHRASE and NEAR over alternatives (prospect/operators.hh, multi_phrase_scorer) against a brute-force model:
// the documents where the slots occur (consecutively, at offsets with gaps, or within a window at distinct
// positions), and each document's score, which is the sum of its slots' scores as the slots score alone
// (an OR of terms sums the terms' weights, a SYNONYM weighs the union). Iteration, advance, top-k counted
// and pruned, and as a child of a conjunction.
#include "prospect/operators.hh"
#include "prospect/search.hh"
#include "prospect/searcher.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::ordinal_t;
namespace p = prospect;

namespace {

struct corpus {
	std::vector<std::vector<std::string>> docs;  // tokens; position i + 1 holds tokens[i]
	std::string file;
	std::unique_ptr<p::segment> seg;
	std::map<std::string, std::vector<std::uint32_t>> wdfs;  // term -> wdf per document

	explicit corpus(std::vector<std::vector<std::string>> d) : docs(std::move(d)) {
		p::segment_writer w;
		for (std::size_t i = 0; i < docs.size(); ++i) {
			std::map<std::string, std::vector<std::uint32_t>> positions;
			for (std::size_t k = 0; k < docs[i].size(); ++k) { positions[docs[i][k]].push_back(std::uint32_t(k + 1)); }
			std::vector<p::posting_input> in;
			for (auto& [t, pos] : positions) { in.push_back({t, std::uint32_t(pos.size()), pos}); }
			w.add(i + 1, in, std::uint32_t(docs[i].size()));
			for (auto& [t, pos] : positions) {
				auto& v = wdfs[t];
				v.resize(docs.size());
				v[i] = std::uint32_t(pos.size());
			}
		}
		file = w.flush({});
		seg = std::make_unique<p::segment>(file);
	}
	const p::inverted_plugin::reader& rd() const { return seg->get<p::inverted_plugin>(); }
	std::uint64_t df(const std::string& t) const {
		auto it = wdfs.find(t);
		if (it == wdfs.end()) { return 0; }
		return std::uint64_t(std::count_if(it->second.begin(), it->second.end(), [](auto x) { return x > 0; }));
	}
	std::uint32_t wdf(const std::string& t, std::size_t d) const {
		auto it = wdfs.find(t);
		return it == wdfs.end() ? 0 : it->second[d];
	}
	std::uint32_t length(std::size_t d) const { return std::uint32_t(docs[d].size()); }
};

enum class mode { any, synonym };   // how a slot's terms score: summed (an OR) or as one term

struct slot {
	mode how;
	std::vector<std::string> terms;
};

struct phrase {
	std::vector<slot> slots;
	std::vector<std::uint32_t> offsets;   // empty: consecutive
	std::uint32_t window = 0;             // 0: PHRASE
};

p::weight term_weight(const corpus& c, const std::string& t) { return p::weight::bm25_xapian(p::statistics(c.rd()), c.df(t), 1); }

p::weight synonym_weight(const corpus& c, const std::vector<std::string>& terms) {
	std::vector<std::uint64_t> dfs;
	for (const auto& t : terms) { dfs.push_back(c.df(t)); }
	return p::weight::bm25_xapian(p::statistics(c.rd()), p::synonym_df(dfs, c.docs.size()), 1);
}

// The scorer of one slot alone, or null when none of its terms is in the segment.
p::scorer_ptr slot_scorer(const corpus& c, const slot& s, std::vector<p::format::term_info>& infos) {
	std::vector<p::scorer_ptr> kids;
	std::vector<std::string> present;
	for (const auto& t : s.terms) {
		if (auto info = c.rd().term(0, t)) {
			infos.push_back(*info);
			present.push_back(t);
			kids.push_back(std::make_unique<p::term_scorer>(c.rd(), *info, term_weight(c, t)));
		}
	}
	if (kids.empty()) { return nullptr; }
	if (s.how == mode::synonym) {
		return std::make_unique<p::synonym_scorer>(c.rd(), infos, synonym_weight(c, present));
	}
	if (kids.size() == 1) { return std::move(kids[0]); }
	return std::make_unique<p::disjunction_scorer>(std::move(kids));
}

std::unique_ptr<p::multi_phrase_scorer> build(const corpus& c, const phrase& q) {
	std::vector<p::phrase_slot> slots;
	for (const auto& s : q.slots) {
		p::phrase_slot one;
		one.scorer = slot_scorer(c, s, one.terms);
		if (!one.scorer) { return nullptr; }
		slots.push_back(std::move(one));
	}
	return std::make_unique<p::multi_phrase_scorer>(c.rd(), std::move(slots), q.window, q.offsets);
}

// The positions of the slot's terms in document d.
std::set<std::uint32_t> where(const corpus& c, const slot& s, std::size_t d) {
	std::set<std::uint32_t> out;
	for (std::size_t k = 0; k < c.docs[d].size(); ++k) {
		if (std::find(s.terms.begin(), s.terms.end(), c.docs[d][k]) != s.terms.end()) { out.insert(std::uint32_t(k + 1)); }
	}
	return out;
}

// Distinct positions, one per slot, all within [lo, lo + window).
bool pick(const std::vector<std::set<std::uint32_t>>& at, std::size_t k, std::uint32_t lo, std::uint32_t window, std::set<std::uint32_t>& used) {
	if (k == at.size()) { return true; }
	for (const auto pos : at[k]) {
		if (pos < lo || pos - lo >= window || used.count(pos)) { continue; }
		used.insert(pos);
		if (pick(at, k + 1, lo, window, used)) { return true; }
		used.erase(pos);
	}
	return false;
}

bool occurs(const corpus& c, const phrase& q, std::size_t d) {
	std::vector<std::set<std::uint32_t>> at;
	for (const auto& s : q.slots) {
		at.push_back(where(c, s, d));
		if (at.back().empty()) { return false; }
	}
	if (q.window) {
		std::set<std::uint32_t> starts;
		for (const auto& a : at) { starts.insert(a.begin(), a.end()); }
		for (const auto lo : starts) {
			std::set<std::uint32_t> used;
			if (pick(at, 0, lo, q.window, used)) { return true; }
		}
		return false;
	}
	for (const auto first : at[0]) {
		bool all = true;
		for (std::size_t k = 1; k < at.size() && all; ++k) {
			const std::uint32_t offset = q.offsets.empty() ? std::uint32_t(k) : q.offsets[k] - q.offsets[0];
			all = at[k].count(first + offset) > 0;
		}
		if (all) { return true; }
	}
	return false;
}

// A slot's score alone in document d.
double slot_score(const corpus& c, const slot& s, std::size_t d) {
	const std::uint32_t length = c.length(d);
	if (s.how == mode::synonym) {
		std::vector<std::string> present;
		std::uint64_t wdf = 0;
		for (const auto& t : s.terms) {
			if (c.rd().term(0, t)) { present.push_back(t); }
			wdf += c.wdf(t, d);
		}
		return synonym_weight(c, present)(std::uint32_t(std::min<std::uint64_t>(wdf, length)), length);
	}
	double sum = 0;
	for (const auto& t : s.terms) {
		if (const auto wdf = c.wdf(t, d)) { sum += term_weight(c, t)(wdf, length); }
	}
	return sum;
}

bool close(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }

std::map<ordinal_t, double> model(const corpus& c, const phrase& q) {
	std::map<ordinal_t, double> out;
	for (std::size_t d = 0; d < c.docs.size(); ++d) {
		if (!occurs(c, q, d)) { continue; }
		double sum = 0;
		for (const auto& s : q.slots) { sum += slot_score(c, s, d); }
		out[ordinal_t(d)] = sum;
	}
	return out;
}

void check_against(const corpus& c, const phrase& q, const std::map<ordinal_t, double>& want, std::mt19937_64& rng) {
	// next(): every match, in order, with its score.
	{
		auto ph = build(c, q);
		CHECK(ph != nullptr);
		auto it = want.begin();
		for (auto d = ph->next(); d != strata::no_more_docs; d = ph->next(), ++it) {
			CHECK(it != want.end() && it->first == d);
			if (it == want.end() || it->first != d) { break; }
			CHECK(close(ph->score(), it->second));
		}
		CHECK(it == want.end());
	}
	// advance(): from a few random targets, the first match at or after each.
	for (int i = 0; i < 3; ++i) {
		auto ph = build(c, q);
		auto target = ordinal_t(rng() % c.docs.size());
		for (int step = 0; step < 6; ++step) {
			const auto d = ph->advance(target);
			const auto it = want.lower_bound(target);
			CHECK((it == want.end()) == (d == strata::no_more_docs));
			if (it == want.end() || d == strata::no_more_docs) { break; }
			CHECK(it->first == d);
			CHECK(close(ph->score(), it->second));
			target = d + 1 + ordinal_t(rng() % 20);
		}
	}
	// two-phase: the approximate documents hold every slot; matches() confirms.
	{
		auto ph = build(c, q);
		std::set<ordinal_t> got;
		for (auto d = ph->approx_next(); d != strata::no_more_docs; d = ph->approx_next()) {
			if (ph->matches()) { got.insert(d); }
		}
		CHECK(got.size() == want.size());
		for (const auto& [d, score] : want) { CHECK(got.count(d) == 1); }
	}
	// top-k, counted and not: the same documents and scores, best first.
	for (const bool counted : {false, true}) {
		auto ph = build(c, q);
		std::uint64_t n = 0;
		const auto hits = p::top_k(*c.seg, *ph, 1000, counted ? &n : nullptr);
		CHECK(hits.size() == want.size());
		if (counted) { CHECK(n == want.size()); }
		for (const auto& h : hits) {
			const auto it = want.find(h.ord);
			CHECK(it != want.end());
			if (it != want.end()) { CHECK(close(h.score, it->second)); }
		}
	}
	// beside a term: documents with the phrase and the term, scores summed.
	{
		const std::string extra = "a";
		auto ph = build(c, q);
		std::vector<p::scorer_ptr> kids;
		kids.push_back(std::move(ph));
		if (auto info = c.rd().term(0, extra)) {
			kids.push_back(std::make_unique<p::term_scorer>(c.rd(), *info, term_weight(c, extra)));
			p::conjunction_scorer both(std::move(kids));
			std::size_t found = 0;
			for (auto d = both.next(); d != strata::no_more_docs; d = both.next(), ++found) {
				const auto it = want.find(d);
				CHECK(it != want.end() && c.wdf(extra, d) > 0);
				if (it != want.end()) { CHECK(close(both.score(), it->second + term_weight(c, extra)(c.wdf(extra, d), c.length(d)))); }
			}
			std::size_t expect = 0;
			for (const auto& [d, score] : want) { expect += c.wdf(extra, d) > 0; }
			CHECK(found == expect);
		}
	}
}

void fixed() {
	corpus c({{"a", "x", "b"}, {"a", "b"}, {"b", "x", "a"}, {"q", "r", "b"}, {"q", "b", "r"}, {"a", "a"}, {"c", "a", "b", "c"}});
	std::mt19937_64 rng(1);
	const auto ids = [&](const phrase& q) {
		std::vector<ordinal_t> out;
		for (const auto& [d, s] : model(c, q)) { out.push_back(d); }
		auto ph = build(c, q);
		std::vector<ordinal_t> got;
		for (auto d = ph->next(); d != strata::no_more_docs; d = ph->next()) { got.push_back(d); }
		CHECK(got == out);
		check_against(c, q, model(c, q), rng);
		return got;
	};
	const slot any_ab{mode::any, {"a", "b"}}, syn_ab{mode::synonym, {"a", "b"}}, r{mode::any, {"r"}}, q{mode::any, {"q"}}, b{mode::any, {"b"}}, a{mode::any, {"a"}};
	// "q (b|r)": documents 3 and 4 (q b / q r)
	CHECK((ids({{q, slot{mode::any, {"b", "r"}}}, {}, 0}) == std::vector<ordinal_t>{3, 4}));
	// "(a|b) b": consecutive: doc 1 (a b), doc 6 (a b); doc 5 has no b
	CHECK((ids({{any_ab, b}, {}, 0}) == std::vector<ordinal_t>{1, 6}));
	CHECK((ids({{syn_ab, b}, {}, 0}) == std::vector<ordinal_t>{1, 6}));
	// "q _ r" with a gap: doc 3 (q r b: r follows q) is not it; doc 4 (q b r) is
	CHECK((ids({{q, r}, {0, 2}, 0}) == std::vector<ordinal_t>{4}));
	// NEAR 2: (a|b) and b at distinct positions within 2: docs 1 and 6
	CHECK((ids({{any_ab, b}, {}, 2}) == std::vector<ordinal_t>{1, 6}));
	// NEAR needs two positions: (a|b) and (a|b) in a doc holding a single a or b: doc 0 has a and b; doc 5 has a, a
	CHECK((ids({{any_ab, any_ab}, {}, 3}) == std::vector<ordinal_t>{0, 1, 2, 5, 6}));
	// a slot with a term the segment lacks still matches through its other terms
	CHECK((ids({{slot{mode::any, {"nope", "q"}}, r}, {}, 0}) == std::vector<ordinal_t>{3}));
	// a slot with no term in the segment builds nothing
	CHECK(build(c, {{slot{mode::any, {"nope"}}, b}, {}, 0}) == nullptr);
}

void random_phrases() {
	std::mt19937_64 rng(11);
	const std::vector<std::string> vocab = {"a", "b", "c", "d", "e", "f"};
	std::vector<std::vector<std::string>> docs;
	for (int i = 0; i < 500; ++i) {
		std::vector<std::string> d(1 + rng() % 16);
		for (auto& w : d) { w = vocab[rng() % vocab.size()]; }
		docs.push_back(std::move(d));
	}
	corpus c(std::move(docs));
	int matched = 0, near_matched = 0, gapped = 0, multi = 0;
	for (int i = 0; i < 600; ++i) {
		phrase q;
		const std::size_t n = 2 + rng() % 3;
		for (std::size_t k = 0; k < n; ++k) {
			slot s;
			s.how = rng() % 2 ? mode::any : mode::synonym;
			for (std::size_t m = 0, count = 1 + rng() % 3; m < count; ++m) {
				const auto& t = vocab[rng() % vocab.size()];
				if (std::find(s.terms.begin(), s.terms.end(), t) == s.terms.end()) { s.terms.push_back(t); }
			}
			multi += s.terms.size() > 1;
			q.slots.push_back(std::move(s));
		}
		if (rng() % 3 == 0) {
			q.window = std::uint32_t(n + rng() % 4);
		} else if (rng() % 3 == 0) {
			std::uint32_t at = 0;
			for (std::size_t k = 0; k < n; ++k) {
				q.offsets.push_back(at);
				at += 1 + rng() % 3;
			}
			gapped += 1;
		}
		const auto want = model(c, q);
		matched += !want.empty();
		near_matched += q.window && !want.empty();
		check_against(c, q, want, rng);
	}
	CHECK(matched > 200 && near_matched > 30 && gapped > 20 && multi > 400);
}

}  // namespace

int main() {
	fixed();
	random_phrases();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("multi_phrase_test: ok");
	return 0;
}
