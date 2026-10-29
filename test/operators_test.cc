// SYNONYM, MAX, XOR, SCALE_WEIGHT, ELITE_SET and the host-source scorer (prospect/operators.hh):
//   1. parity with Xapian's recorded vectors (strata-oracle `ops`, test/fixtures/xapian-ops/):
//      the match count and the top 50 (docid, weight) of each query, counted and pruned;
//   2. random operator trees against a brute-force model on larger segments: iteration, advance,
//      top-k counted and pruned, count, fresh copies, and the score bounds pruning relies on;
//   3. a host value source over an in-memory slot, alone and inside trees, and shared by threads.
// OPS_PARITY_MIN: the least number of oracle rows that must be compared (guards a silent skip).
#include "prospect/operators.hh"
#include "prospect/search.hh"
#include "prospect/searcher.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::ordinal_t;
namespace p = prospect;

namespace {

// --- queries ------------------------------------------------------------------------------------

struct node {
	std::string op;            // "" for a term
	std::string term;
	unsigned wqf = 1;
	double arg = 0;            // SCALE factor, ELITE_SET size
	std::vector<node> kids;
};

node term_node(std::string t, unsigned wqf = 1) {
	node n;
	n.term = std::move(t);
	n.wqf = wqf;
	return n;
}
node op_node(std::string op, std::vector<node> kids, double arg = 0) {
	node n;
	n.op = std::move(op);
	n.kids = std::move(kids);
	n.arg = arg;
	return n;
}

// The s-expressions of strata-oracle's gen-ops.py.
class sexp {
public:
	explicit sexp(const std::string& s) : s_(s) {}
	node parse() {
		blank();
		if (s_[i_] != '(') {
			std::string w = word();
			unsigned wqf = 1;
			if (auto at = w.find('*'); at != std::string::npos) { wqf = unsigned(std::stoul(w.substr(at + 1))), w.resize(at); }
			return term_node(w, wqf);
		}
		++i_;
		node n;
		n.op = word();
		if (n.op == "SCALE" || n.op == "SCALEN" || n.op == "ELITE_SET" || n.op == "PHRASE") { n.arg = std::stod(word()); }
		for (blank(); s_[i_] != ')'; blank()) { n.kids.push_back(parse()); }
		++i_;
		return n;
	}

private:
	void blank() { while (i_ < s_.size() && s_[i_] == ' ') { ++i_; } }
	std::string word() {
		blank();
		const std::size_t a = i_;
		while (i_ < s_.size() && s_[i_] != ' ' && s_[i_] != ')' && s_[i_] != '(') { ++i_; }
		return s_.substr(a, i_ - a);
	}
	const std::string& s_;
	std::size_t i_ = 0;
};

// --- the segment --------------------------------------------------------------------------------

struct corpus {
	std::vector<std::vector<std::string>> docs;  // tokens; position i + 1 holds tokens[i]
	std::string file;
	std::unique_ptr<p::segment> seg;
	std::map<std::string, std::vector<std::uint32_t>> df_of;  // term -> wdf per doc (0 where absent)

	explicit corpus(std::vector<std::vector<std::string>> d) : docs(std::move(d)) {
		p::segment_writer w;
		for (std::size_t i = 0; i < docs.size(); ++i) {
			std::map<std::string, std::vector<std::uint32_t>> positions;
			for (std::size_t k = 0; k < docs[i].size(); ++k) { positions[docs[i][k]].push_back(std::uint32_t(k + 1)); }
			std::vector<p::posting_input> in;
			for (auto& [t, pos] : positions) { in.push_back({t, std::uint32_t(pos.size()), pos}); }
			w.add(i + 1, in, std::uint32_t(docs[i].size()));
			for (auto& [t, pos] : positions) {
				auto& v = df_of[t];
				v.resize(docs.size());
				v[i] = std::uint32_t(pos.size());
			}
		}
		file = w.flush({});
		seg = std::make_unique<p::segment>(file);
	}
	const p::inverted_plugin::reader& rd() const { return seg->get<p::inverted_plugin>(); }
	std::uint64_t df(const std::string& t) const {
		auto it = df_of.find(t);
		if (it == df_of.end()) { return 0; }
		return std::uint64_t(std::count_if(it->second.begin(), it->second.end(), [](auto x) { return x > 0; }));
	}
	std::uint32_t wdf(const std::string& t, std::size_t d) const {
		auto it = df_of.find(t);
		return it == df_of.end() ? 0 : it->second[d];
	}
	std::uint32_t length(std::size_t d) const { return std::uint32_t(docs[d].size()); }
	p::weight term_weight(const std::string& t, unsigned wqf) const { return p::weight::bm25_xapian(p::statistics(rd()), df(t), wqf); }
};

// What Xapian's operators do, by definition and without scorers. A document maps to its score.
using result = std::map<ordinal_t, double>;

double synonym_estimate(const corpus& c, const std::vector<const node*>& terms) {
	double miss = 1;
	for (auto* t : terms) { miss *= 1.0 - double(c.df(t->term)) / double(c.docs.size()); }
	return std::floor(double(c.docs.size()) * (1.0 - miss) + 0.5);
}

void flatten_xor(const node& n, std::vector<const node*>& out) {
	for (const auto& k : n.kids) {
		if (k.op == "XOR") { flatten_xor(k, out); } else { out.push_back(&k); }
	}
}

result model(const corpus& c, const node& n) {
	result out;
	if (n.op.empty()) {
		const auto w = c.term_weight(n.term, n.wqf);
		for (std::size_t d = 0; d < c.docs.size(); ++d) {
			if (const auto f = c.wdf(n.term, d)) { out[ordinal_t(d)] = w(f, c.length(d)); }
		}
		return out;
	}
	if (n.op == "SYNONYM") {
		std::vector<const node*> terms;
		for (const auto& k : n.kids) { terms.push_back(&k); }
		const auto w = p::weight::bm25_xapian(p::statistics(c.rd()), std::uint64_t(synonym_estimate(c, terms)), 1);
		for (std::size_t d = 0; d < c.docs.size(); ++d) {
			std::uint64_t f = 0;
			for (auto* t : terms) { f += c.wdf(t->term, d); }
			if (f) { out[ordinal_t(d)] = w(std::uint32_t(std::min<std::uint64_t>(f, c.length(d))), c.length(d)); }
		}
		return out;
	}
	if (n.op == "ELITE_SET") {
		std::vector<std::pair<double, std::size_t>> keyed;
		for (std::size_t i = 0; i < n.kids.size(); ++i) {
			if (c.df(n.kids[i].term)) { keyed.push_back({-c.term_weight(n.kids[i].term, n.kids[i].wqf).idf, i}); }
		}
		std::sort(keyed.begin(), keyed.end());  // heaviest first, ties by position
		keyed.resize(std::min<std::size_t>(keyed.size(), n.arg > 0 ? std::size_t(n.arg) : p::default_elite_size));
		for (auto& [key, i] : keyed) {
			for (auto& [d, s] : model(c, n.kids[i])) { out[d] += s; }
		}
		return out;
	}
	if (n.op == "XOR") {
		std::vector<const node*> flat;
		flatten_xor(n, flat);
		std::map<ordinal_t, std::pair<int, double>> seen;
		for (auto* k : flat) {
			for (auto& [d, s] : model(c, *k)) { ++seen[d].first, seen[d].second += s; }
		}
		for (auto& [d, cs] : seen) {
			if (cs.first % 2 == 1) { out[d] = cs.second; }
		}
		return out;
	}
	std::vector<result> ks;
	for (const auto& k : n.kids) { ks.push_back(model(c, k)); }
	if (n.op == "OR") {
		for (auto& k : ks) {
			for (auto& [d, s] : k) { out[d] += s; }
		}
	} else if (n.op == "AND") {
		for (auto& [d, s] : ks[0]) {
			double sum = 0;
			bool all = true;
			for (auto& k : ks) {
				auto it = k.find(d);
				all = all && it != k.end();
				if (it != k.end()) { sum += it->second; }
			}
			if (all) { out[d] = sum; }
		}
	} else if (n.op == "AND_MAYBE") {
		for (auto& [d, s] : ks[0]) { out[d] = s + (ks[1].count(d) ? ks[1][d] : 0.0); }
	} else if (n.op == "AND_NOT") {
		for (auto& [d, s] : ks[0]) {
			if (!ks[1].count(d)) { out[d] = s; }
		}
	} else if (n.op == "FILTER") {
		for (auto& [d, s] : ks[0]) {
			if (ks[1].count(d)) { out[d] = s; }
		}
	} else if (n.op == "MAX") {
		for (auto& k : ks) {
			for (auto& [d, s] : k) { out[d] = std::max(out.count(d) ? out[d] : 0.0, s); }
		}
	} else if (n.op == "SCALE") {
		for (auto& [d, s] : ks[0]) { out[d] = s * n.arg; }
	}
	return out;
}

// The scorer for a node, or null when it matches nothing.
p::scorer_ptr build(const corpus& c, const node& n) {
	if (n.op.empty()) {
		auto info = c.rd().term(0, n.term);
		if (!info) { return nullptr; }
		return std::make_unique<p::term_scorer>(c.rd(), *info, c.term_weight(n.term, n.wqf));
	}
	if (n.op == "SYNONYM") {
		std::vector<p::format::term_info> infos;
		std::vector<std::uint64_t> dfs;
		for (const auto& k : n.kids) {
			dfs.push_back(c.df(k.term));
			if (auto info = c.rd().term(0, k.term)) { infos.push_back(*info); }
		}
		if (infos.empty()) { return nullptr; }
		return std::make_unique<p::synonym_scorer>(c.rd(), infos, p::weight::bm25_xapian(p::statistics(c.rd()), p::synonym_df(dfs, c.docs.size()), 1));
	}
	if (n.op == "ELITE_SET") {
		std::vector<p::elite_child> kids;
		for (const auto& k : n.kids) {
			if (auto s = build(c, k)) { kids.push_back({std::move(s), c.term_weight(k.term, k.wqf).idf}); }
		}
		return p::elite_set(std::move(kids), std::size_t(n.arg));
	}
	std::vector<p::scorer_ptr> ks;
	for (const auto& k : n.kids) { ks.push_back(build(c, k)); }
	auto present = [&] {
		std::vector<p::scorer_ptr> out;
		for (auto& k : ks) {
			if (k) { out.push_back(std::move(k)); }
		}
		return out;
	};
	auto one_or = [&](std::vector<p::scorer_ptr> v, auto make) -> p::scorer_ptr {
		if (v.empty()) { return nullptr; }
		if (v.size() == 1) { return std::move(v[0]); }
		return make(std::move(v));
	};
	if (n.op == "OR") { return one_or(present(), [](auto v) { return std::make_unique<p::disjunction_scorer>(std::move(v)); }); }
	if (n.op == "MAX") { return one_or(present(), [](auto v) { return std::make_unique<p::max_scorer>(std::move(v)); }); }
	if (n.op == "XOR") { return one_or(present(), [](auto v) { return std::make_unique<p::xor_scorer>(std::move(v)); }); }
	if (n.op == "AND") {
		for (auto& k : ks) {
			if (!k) { return nullptr; }
		}
		return one_or(present(), [](auto v) { return std::make_unique<p::conjunction_scorer>(std::move(v)); });
	}
	if (n.op == "SCALE") { return ks[0] ? std::make_unique<p::scale_scorer>(std::move(ks[0]), n.arg) : nullptr; }
	if (!ks[0]) { return nullptr; }
	if (n.op == "AND_MAYBE") { return ks[1] ? std::make_unique<p::required_optional_scorer>(std::move(ks[0]), std::move(ks[1])) : std::move(ks[0]); }
	if (n.op == "AND_NOT") { return ks[1] ? std::make_unique<p::exclusion_scorer>(std::move(ks[0]), std::move(ks[1])) : std::move(ks[0]); }
	if (n.op == "FILTER") {
		if (!ks[1]) { return nullptr; }
		std::vector<p::scorer_ptr> both;
		both.push_back(std::move(ks[0]));
		both.push_back(std::make_unique<p::scale_scorer>(std::move(ks[1]), 0.0));
		return std::make_unique<p::conjunction_scorer>(std::move(both));
	}
	std::fprintf(stderr, "unknown operator %s\n", n.op.c_str());
	std::exit(2);
}

bool near_eq(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol * std::max(1.0, std::max(std::abs(a), std::abs(b))); }

// A scorer against the model: every way of iterating, the top k, the count, the bounds.
void check_scorer(const corpus& c, const node& n, const result& want, std::mt19937_64& rng, bool thorough) {
	auto make = [&]() { return build(c, n); };
	if (want.empty()) {
		if (auto s = make()) { CHECK(s->next() == strata::no_more_docs); }
		return;
	}
	auto s = make();
	CHECK(s != nullptr);
	if (!s) { return; }
	// next(): every match in order with its score; the bounds hold for each.
	{
		auto it = want.begin();
		for (ordinal_t d = s->next(); d != strata::no_more_docs; d = s->next(), ++it) {
			CHECK(it != want.end() && d == it->first);
			if (it == want.end() || d != it->first) { break; }
			const double sc = s->score();
			CHECK(near_eq(sc, it->second));
			CHECK(sc <= p::sum_upper(s->max_score_total(), s->score_terms()));
			CHECK(sc <= p::sum_upper(s->max_score(d, d), s->score_terms()));
			CHECK(sc <= p::sum_upper(s->current_bound(), s->score_terms()));
		}
		CHECK(it == want.end());
	}
	// Past the last match, next() and advance() stay there.
	{
		auto e = make();
		while (e->next() != strata::no_more_docs) {}
		CHECK(e->next() == strata::no_more_docs && e->next() == strata::no_more_docs);
		CHECK(e->advance(0) == strata::no_more_docs && e->doc() == strata::no_more_docs);
	}
	// advance(): to random targets, then onward.
	{
		auto a = make();
		ordinal_t at = 0;
		for (int step = 0; step < 6; ++step) {
			at += ordinal_t(rng() % (c.docs.size() / 3 + 1));
			auto it = want.lower_bound(at);
			const ordinal_t got = a->advance(at);
			CHECK(got == (it == want.end() ? strata::no_more_docs : it->first));
			if (got == strata::no_more_docs) { break; }
			CHECK(near_eq(a->score(), it->second));
			CHECK(a->advance(0) == got);  // a target behind the position leaves it
			at = got + 1;
		}
	}
	// A range's bound covers every score inside it.
	{
		auto b = make();
		const ordinal_t from = ordinal_t(rng() % c.docs.size()), upto = from + ordinal_t(rng() % 700);
		const double bound = b->max_score(from, upto);
		for (auto it = want.lower_bound(from); it != want.end() && it->first <= upto; ++it) {
			CHECK(it->second <= p::sum_upper(bound, b->score_terms()));
		}
	}
	// top-k counted, then pruned; ties order by ordinal.
	std::vector<std::pair<double, ordinal_t>> best;
	for (auto& [d, sc] : want) { best.push_back({-sc, d}); }
	std::sort(best.begin(), best.end());
	for (std::size_t k : {1u, 3u, 10u, 50u}) {
		for (bool counted : {true, false}) {
			auto q = make();
			std::uint64_t cnt = 0;
			auto hits = p::top_k(*c.seg, *q, k, counted ? &cnt : nullptr);
			if (counted) { CHECK(cnt == want.size()); }
			CHECK(hits.size() == std::min(k, best.size()));
			for (std::size_t i = 0; i < hits.size() && i < best.size(); ++i) {
				CHECK(near_eq(hits[i].score, -best[i].first));
				const bool tied = (i + 1 < best.size() && near_eq(best[i].first, best[i + 1].first)) || (i > 0 && near_eq(best[i].first, best[i - 1].first));
				CHECK(tied || hits[i].ord == best[i].second);
			}
		}
	}
	if (!thorough) { return; }
	// count(): exact, or windowed over fill.
	{
		auto q = make();
		CHECK(p::count(*q) == want.size());
	}
	// fresh(): an unpositioned copy of an advanced scorer counts every match.
	{
		auto q = make();
		q->advance(ordinal_t(rng() % c.docs.size()));
		if (auto copy = q->fresh()) { CHECK(p::count(*copy) == want.size()); }
	}
}

std::vector<std::string> split_lines(std::ifstream& in) {
	std::vector<std::string> out;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty()) { out.push_back(line); }
	}
	return out;
}

// --- 1. Xapian's vectors ------------------------------------------------------------------------

std::vector<std::string> json_strings(const std::string& line, const std::string& key) {
	std::vector<std::string> out;
	std::size_t at = line.find("\"" + key + "\":[");
	if (at == std::string::npos) { return out; }
	for (at += key.size() + 4; at < line.size() && line[at] != ']';) {
		const std::size_t a = line.find('"', at), b = line.find('"', a + 1);
		out.push_back(line.substr(a + 1, b - a - 1));
		at = b + 1 < line.size() && line[b + 1] == ',' ? b + 2 : b + 1;
	}
	return out;
}

std::string json_field(const std::string& line, const std::string& key) {
	const std::size_t at = line.find("\"" + key + "\":");
	if (at == std::string::npos) { return {}; }
	std::size_t a = at + key.size() + 3, b = a;
	if (line[a] == '"') {
		b = line.find('"', a + 1);
		return line.substr(a + 1, b - a - 1);
	}
	while (b < line.size() && line[b] != ',' && line[b] != '}') { ++b; }
	return line.substr(a, b - a);
}

// Hits as [docid,weight,percent] triples.
std::vector<std::pair<std::uint64_t, double>> json_hits(const std::string& line) {
	std::vector<std::pair<std::uint64_t, double>> out;
	std::size_t at = line.find("\"hits\":[");
	for (at += 8; line[at] == '[';) {
		const std::size_t c1 = line.find(',', at), c2 = line.find(',', c1 + 1), close = line.find(']', c2);
		out.push_back({std::stoull(line.substr(at + 1, c1 - at - 1)), std::stod(line.substr(c1 + 1, c2 - c1 - 1))});
		at = close + 1 < line.size() && line[close + 1] == ',' ? close + 2 : close + 1;
	}
	return out;
}

bool terms_only(const node& n) {
	return std::all_of(n.kids.begin(), n.kids.end(), [](const node& k) { return k.op.empty(); });
}

// Whether the oracle's choice among ties could differ from ours: ELITE_SET with equal term
// weights at the edge of the set (Xapian's pick among them follows no order of the query).
bool elite_tie_at_edge(const corpus& c, const node& n) {
	std::vector<double> keys;
	for (const auto& k : n.kids) {
		if (c.df(k.term)) { keys.push_back(c.term_weight(k.term, k.wqf).idf); }
	}
	std::sort(keys.rbegin(), keys.rend());
	const std::size_t size = n.arg > 0 ? std::size_t(n.arg) : p::default_elite_size;
	return size < keys.size() && keys[size - 1] == keys[size];
}

void parity(const std::string& dir) {
	std::ifstream cin_(dir + "/corpus.jsonl"), vin(dir + "/ops.jsonl");
	CHECK(cin_.good() && vin.good());
	std::vector<std::vector<std::string>> docs;
	for (const auto& line : split_lines(cin_)) { docs.push_back(json_strings(line, "terms")); }
	corpus c(std::move(docs));
	std::map<std::string, std::array<int, 3>> tally;  // operator -> compared, skipped for ties, not asserted
	int compared = 0;
	for (const auto& line : split_lines(vin)) {
		if (line.starts_with("{\"index\"")) {
			CHECK(std::stoul(json_field(line, "docs")) == c.docs.size());
			continue;
		}
		const std::string q = json_field(line, "query");
		node n = sexp(q).parse();
		const std::string name = n.op.empty() ? "term" : n.op;
		auto& t = tally[name];
		if (!json_field(line, "error").empty()) {
			// Xapian refuses a negative factor; the n-ary SCALE_WEIGHT is its list constructor's error.
			if (n.op == "SCALE") {
				CHECK(n.arg < 0);
				bool threw = false;
				try { p::scale_scorer(build(c, term_node("w1")), n.arg); } catch (const std::invalid_argument&) { threw = true; }
				CHECK(threw);
			}
			continue;
		}
		// Rows the vectors record but do not explain: SYNONYM over a non-term child, ELITE_SET
		// over composite children.
		if ((n.op == "SYNONYM" || n.op == "ELITE_SET") && !terms_only(n)) { ++t[2]; continue; }
		if (n.op == "ELITE_SET" && elite_tie_at_edge(c, n)) { ++t[1]; continue; }
		const auto want = json_hits(line);
		const std::uint64_t matches = std::stoull(json_field(line, "lower"));
		for (bool counted : {true, false}) {
			auto s = build(c, n);
			std::uint64_t cnt = 0;
			std::vector<p::hit> hits;
			if (s) { hits = p::top_k(*c.seg, *s, 50, counted ? &cnt : nullptr); }
			if (counted) { CHECK(cnt == matches); }
			CHECK(hits.size() == want.size());
			for (std::size_t i = 0; i < hits.size() && i < want.size(); ++i) {
				CHECK(near_eq(hits[i].score, want[i].second, 1e-9));
				const bool tied = (i > 0 && near_eq(want[i - 1].second, want[i].second)) || (i + 1 < want.size() && near_eq(want[i + 1].second, want[i].second));
				CHECK(tied || hits[i].ord + 1 == want[i].first);
			}
		}
		++t[0];
		++compared;
	}
	for (auto& [name, t] : tally) { std::printf("  %-10s compared %3d  tied %d  recorded only %d\n", name.c_str(), t[0], t[1], t[2]); }
	const int least = std::getenv("OPS_PARITY_MIN") ? std::atoi(std::getenv("OPS_PARITY_MIN")) : 660;
	CHECK(compared >= least);
	for (const char* op : {"SYNONYM", "MAX", "XOR", "SCALE", "ELITE_SET", "FILTER"}) { CHECK(tally[op][0] > 0); }
}

// --- 2. random trees ----------------------------------------------------------------------------

node random_node(std::mt19937_64& rng, const std::vector<std::string>& vocab, int depth) {
	auto leaf = [&] { return term_node(vocab[rng() % vocab.size()], 1 + unsigned(rng() % 4 == 0)); };
	auto leaves = [&](std::size_t lo, std::size_t hi) {
		std::vector<node> out;
		for (std::size_t i = 0, n = lo + rng() % (hi - lo + 1); i < n; ++i) { out.push_back(leaf()); }
		return out;
	};
	auto child = [&] { return depth > 0 && rng() % 3 ? random_node(rng, vocab, depth - 1) : leaf(); };
	auto kids = [&](std::size_t lo, std::size_t hi) {
		std::vector<node> out;
		for (std::size_t i = 0, n = lo + rng() % (hi - lo + 1); i < n; ++i) { out.push_back(child()); }
		return out;
	};
	switch (rng() % 11) {
	case 0: return op_node("SYNONYM", leaves(1, 5));
	case 1: return op_node("MAX", kids(1, 4));
	case 2: return op_node("XOR", kids(2, 4));
	case 3: return op_node("SCALE", kids(1, 1), double(rng() % 5) * 0.75);
	case 4: return op_node("ELITE_SET", leaves(2, 9), double(rng() % 5));
	case 5: return op_node("FILTER", kids(2, 2));
	case 6: return op_node("OR", kids(2, 3));
	case 7: return op_node("AND", kids(2, 3));
	case 8: return op_node("AND_MAYBE", kids(2, 2));
	case 9: return op_node("AND_NOT", kids(2, 2));
	default: return leaf();
	}
}

void random_trees() {
	std::mt19937_64 rng(31);
	const std::vector<std::string> vocab = {"a", "b", "c", "d", "e", "f", "g", "h", "rare1", "rare2", "absent"};
	for (std::size_t ndocs : {300u, 5000u, 20000u}) {
		std::vector<std::vector<std::string>> docs(ndocs);
		for (auto& d : docs) {
			for (std::size_t i = 0, len = 1 + rng() % 30; i < len; ++i) {
				const std::size_t pick = rng() % 100;
				d.push_back(pick < 2 ? vocab[8 + rng() % 2] : vocab[std::min<std::size_t>(7, pick / 12)]);
			}
		}
		corpus c(std::move(docs));
		const int rounds = ndocs >= 20000 ? 60 : 250;
		for (int round = 0; round < rounds; ++round) {
			const node n = random_node(rng, vocab, 3);
			check_scorer(c, n, model(c, n), rng, round % 2 == 0);
		}
	}
}

// --- 3. a host value source ---------------------------------------------------------------------

// Ordinals holding a decimal string, as a values column would.
class decimal_slot {
public:
	decimal_slot(std::shared_ptr<const std::vector<std::optional<std::string>>> values, std::shared_ptr<const std::vector<ordinal_t>> present)
		: values_(std::move(values)), present_(std::move(present)) {}
	ordinal_t next_present(ordinal_t from) const {
		auto it = std::lower_bound(present_->begin(), present_->end(), from);
		return it == present_->end() ? strata::no_more_docs : *it;
	}
	const std::string& value(ordinal_t o) const { return *(*values_)[o]; }
	std::uint64_t count() const { return present_->size(); }

private:
	std::shared_ptr<const std::vector<std::optional<std::string>>> values_;
	std::shared_ptr<const std::vector<ordinal_t>> present_;
};

// Accepts values in [lo, hi] and weights them by closeness to hi; rejects what is not a number.
struct range_source {
	long lo, hi;
	double bound;  // max_weight(): 0 for "not said"
	static std::optional<long> decode(std::string_view v) {
		if (v.empty() || v.size() > 9) { return std::nullopt; }
		long x = 0;
		for (char ch : v) {
			if (ch < '0' || ch > '9') { return std::nullopt; }
			x = x * 10 + (ch - '0');
		}
		return x;
	}
	bool accept(std::string_view v) const {
		const auto x = decode(v);
		return x && *x >= lo && *x <= hi;
	}
	double weight(std::string_view v) const {
		const auto x = decode(v);
		return x ? 1.0 + double(*x - lo) / double(hi - lo + 1) : 0.0;
	}
	double max_weight() const { return bound; }
};
static_assert(p::slot_values<decimal_slot>);
static_assert(p::value_source<range_source>);

// Phrases with explicit relative offsets (gaps where words were removed, repeated terms)
// against a brute-force oracle, in both the any-match and the frequency paths.
void phrase_offsets() {
	std::mt19937_64 rng(5);
	const std::vector<std::string> vocab = {"a", "b", "c", "d"};
	std::vector<std::vector<std::string>> docs;
	// fixed: a _ b matches "a x b" only; "a b" consecutive does not
	docs.push_back({"a", "x", "b"});
	docs.push_back({"a", "b"});
	docs.push_back({"b", "x", "a"});
	for (int i = 0; i < 300; ++i) {
		std::vector<std::string> d(1 + rng() % 14);
		for (auto& w : d) { w = vocab[rng() % vocab.size()]; }
		docs.push_back(std::move(d));
	}
	corpus c(std::move(docs));
	auto run = [&](const std::vector<std::string>& terms, const std::vector<std::uint32_t>& offs, bool counted) {
		std::vector<p::format::term_info> infos;
		for (const auto& t : terms) {
			auto ti = c.rd().term(0, t);
			CHECK(ti.has_value());
			infos.push_back(*ti);
		}
		p::phrase_scorer ph(c.rd(), infos, {}, 0, nullptr, {}, offs);
		std::uint64_t n = 0;
		std::vector<std::size_t> got;
		for (auto h : p::top_k(*c.seg, ph, 1000, counted ? &n : nullptr)) { got.push_back(h.ord); }
		std::sort(got.begin(), got.end());
		return got;
	};
	auto oracle = [&](const std::vector<std::string>& terms, const std::vector<std::uint32_t>& offs) {
		std::vector<std::size_t> want;
		for (std::size_t d = 0; d < c.docs.size(); ++d) {
			const auto& w = c.docs[d];
			bool hit = false;
			for (std::size_t s = 0; s < w.size() && !hit; ++s) {
				bool ok = true;
				for (std::size_t k = 0; k < terms.size() && ok; ++k) { ok = s + offs[k] - offs[0] < w.size() && w[s + offs[k] - offs[0]] == terms[k]; }
				hit = ok;
			}
			if (hit) { want.push_back(d); }
		}
		return want;
	};
	auto has = [](const std::vector<std::size_t>& v, std::size_t d) { return std::find(v.begin(), v.end(), d) != v.end(); };
	const auto gap = run({"a", "b"}, {0, 2}, false), adjacent = run({"a", "b"}, {0, 1}, false);
	CHECK(has(gap, 0) && !has(gap, 1) && !has(gap, 2));
	CHECK(has(adjacent, 1) && !has(adjacent, 0) && !has(adjacent, 2));
	CHECK(run({"a", "b"}, {0, 2}, true) == gap);
	int matched = 0;
	for (int i = 0; i < 400; ++i) {
		const std::size_t n = 2 + rng() % 3;
		std::vector<std::string> terms;
		std::vector<std::uint32_t> offs;
		std::uint32_t at = 0;
		for (std::size_t k = 0; k < n; ++k) {
			terms.push_back(vocab[rng() % vocab.size()]);
			offs.push_back(at);
			at += 1 + rng() % 3;
		}
		const auto want = oracle(terms, offs);
		matched += !want.empty();
		CHECK(run(terms, offs, false) == want);
		CHECK(run(terms, offs, true) == want);
	}
	CHECK(matched > 100);
}

void host_source() {
	std::mt19937_64 rng(77);
	const std::vector<std::string> vocab = {"a", "b", "c", "d", "e", "f"};
	const std::size_t ndocs = 6000;
	std::vector<std::vector<std::string>> docs(ndocs);
	auto values = std::make_shared<std::vector<std::optional<std::string>>>(ndocs);
	auto present = std::make_shared<std::vector<ordinal_t>>();
	for (std::size_t d = 0; d < ndocs; ++d) {
		for (std::size_t i = 0, len = 1 + rng() % 12; i < len; ++i) { docs[d].push_back(vocab[rng() % vocab.size()]); }
		if (rng() % 4) {  // a quarter of the documents hold no value
			(*values)[d] = rng() % 20 == 0 ? std::string("not-a-number") : std::to_string(rng() % 1000);
			present->push_back(ordinal_t(d));
		}
	}
	corpus c(std::move(docs));
	for (double bound : {0.0, 3.0}) {
		for (int round = 0; round < 12; ++round) {
			const long lo = long(rng() % 800), hi = lo + long(rng() % 300);
			range_source src{lo, hi, bound};
			decimal_slot slot(values, present);
			result want;
			for (std::size_t d = 0; d < ndocs; ++d) {
				if ((*values)[d] && src.accept(*(*values)[d])) { want[ordinal_t(d)] = src.weight(*(*values)[d]); }
			}
			auto source = [&] { return std::make_unique<p::source_scorer<decimal_slot, range_source>>(slot, src); };
			// alone
			{
				auto s = source();
				CHECK(s->cost() == present->size());
				CHECK(bound == 0.0 ? s->max_score_total() == std::numeric_limits<double>::infinity() : s->max_score_total() == bound);
				auto it = want.begin();
				for (ordinal_t d = s->next(); d != strata::no_more_docs; d = s->next(), ++it) {
					CHECK(it != want.end() && d == it->first);
					if (it == want.end()) { break; }
					CHECK(s->score() == it->second);
				}
				CHECK(it == want.end());
				CHECK(s->next() == strata::no_more_docs && s->advance(0) == strata::no_more_docs);  // exhausted stays so
				auto a = source();
				const ordinal_t target = ordinal_t(rng() % ndocs);
				auto at = want.lower_bound(target);
				CHECK(a->advance(target) == (at == want.end() ? strata::no_more_docs : at->first));
				const ordinal_t here = a->doc();
				CHECK(a->advance(0) == here);  // a target behind the position leaves it
				CHECK(p::count(*source()) == want.size());
				auto copy = a->fresh();
				CHECK(copy && p::count(*copy) == want.size());
			}
			// beside terms: a filter by AND, a weight by OR and by AND_MAYBE, a veto by AND_NOT
			const std::string t1 = vocab[rng() % vocab.size()], t2 = vocab[rng() % vocab.size()];
			auto term = [&](const std::string& t) { return build(c, term_node(t)); };
			for (int shape = 0; shape < 4; ++shape) {
				auto make = [&]() -> p::scorer_ptr {
					std::vector<p::scorer_ptr> v;
					switch (shape) {
					case 0: v.push_back(term(t1)), v.push_back(source()); return std::make_unique<p::conjunction_scorer>(std::move(v));
					case 1: v.push_back(term(t1)), v.push_back(source()); return std::make_unique<p::disjunction_scorer>(std::move(v));
					case 2: return std::make_unique<p::required_optional_scorer>(term(t1), source());
					default: return std::make_unique<p::exclusion_scorer>(term(t2), source());
					}
				};
				result a = model(c, term_node(t1)), b = model(c, term_node(t2)), expect;
				switch (shape) {
				case 0: for (auto& [d, s] : a) { if (want.count(d)) { expect[d] = s + want[d]; } } break;
				case 1: expect = a; for (auto& [d, s] : want) { expect[d] += s; } break;
				case 2: for (auto& [d, s] : a) { expect[d] = s + (want.count(d) ? want[d] : 0.0); } break;
				default: for (auto& [d, s] : b) { if (!want.count(d)) { expect[d] = s; } } break;
				}
				std::vector<std::pair<double, ordinal_t>> best;
				for (auto& [d, s] : expect) { best.push_back({-s, d}); }
				std::sort(best.begin(), best.end());
				for (std::size_t k : {1u, 10u, 100u}) {
					for (bool counted : {true, false}) {
						auto q = make();
						std::uint64_t cnt = 0;
						auto hits = p::top_k(*c.seg, *q, k, counted ? &cnt : nullptr);
						if (counted) { CHECK(cnt == expect.size()); }
						CHECK(hits.size() == std::min(k, best.size()));
						for (std::size_t i = 0; i < hits.size() && i < best.size(); ++i) { CHECK(near_eq(hits[i].score, -best[i].first)); }
					}
				}
				auto q = make();
				CHECK(p::count(*q) == expect.size());
			}
		}
	}
	// One source and one slot shared by threads, each with its own scorers (TSan's job).
	{
		range_source src{100, 600, 0.0};
		decimal_slot slot(values, present);
		std::size_t expect = 0;
		for (std::size_t d = 0; d < ndocs; ++d) { expect += (*values)[d] && src.accept(*(*values)[d]); }
		std::vector<std::size_t> counts(4, 0);
		std::vector<std::thread> threads;
		for (std::size_t t = 0; t < counts.size(); ++t) {
			threads.emplace_back([&, t] {
				for (int rep = 0; rep < 20; ++rep) {
					p::source_scorer<decimal_slot, range_source> s(slot, src);
					std::size_t n = 0;
					for (ordinal_t d = s.next(); d != strata::no_more_docs; d = s.next()) { n += s.score() > 0; }
					counts[t] = n;
				}
			});
		}
		for (auto& th : threads) { th.join(); }
		for (auto n : counts) { CHECK(n == expect); }
	}
}

// --- the small pieces ---------------------------------------------------------------------------

void pieces() {
	// A nested XOR is the one n-ary XOR: the constructor splices it, and the model flattens it.
	{
		std::mt19937_64 rng(5);
		std::vector<std::vector<std::string>> docs(2000);
		for (auto& d : docs) {
			for (std::size_t i = 0, n = 1 + rng() % 12; i < n; ++i) { d.push_back(std::string(1, char('a' + rng() % 5))); }
		}
		corpus c(std::move(docs));
		const node left = op_node("XOR", {op_node("XOR", {term_node("a"), term_node("b")}), term_node("c")});
		const node deep = op_node("XOR", {term_node("a"), op_node("XOR", {term_node("b"), op_node("XOR", {term_node("c"), term_node("d")})}), term_node("e")});
		for (const node* n : {&left, &deep}) {
			const result want = model(c, *n);
			CHECK(!want.empty());
			check_scorer(c, *n, want, rng, true);
		}
	}
	// SYNONYM's df: one term keeps its own; a union of independent sets, rounded to nearest.
	for (std::uint64_t one : {0u, 1u, 7u, 600u}) {
		const std::uint64_t dfs[] = {one};
		CHECK(p::synonym_df(dfs, 600) == one);
	}
	{
		const std::uint64_t dfs[] = {150, 109};  // 600 * (1 - 450/600 * 491/600) = 231.8
		CHECK(p::synonym_df(dfs, 600) == 232);
		const std::uint64_t same[] = {295, 295};  // 600 * (1 - (305/600)^2) = 445.1
		CHECK(p::synonym_df(same, 600) == 445);
		const std::uint64_t none[] = {0, 0};
		CHECK(p::synonym_df(none, 600) == 0);
		const std::uint64_t all[] = {600, 3};
		CHECK(p::synonym_df(all, 600) == 600);
	}
	// SCALE_WEIGHT refuses a factor below zero or not a number, and zero keeps matches at weight 0.
	corpus c({{"a", "b"}, {"a"}, {"b", "b"}});
	for (double bad : {-1.0, -1e-300, std::nan("")}) {
		bool threw = false;
		try { p::scale_scorer(build(c, term_node("a")), bad); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
	}
	{
		p::scale_scorer zero(build(c, term_node("a")), 0.0);
		CHECK(zero.next() == 0 && zero.score() == 0.0 && zero.max_score_total() == 0.0);
		CHECK(zero.next() == 1 && zero.score() == 0.0);
		CHECK(zero.next() == strata::no_more_docs);
		// A zero factor over an unbounded child stays zero, not a number.
		struct unbounded final : p::scorer {
			ordinal_t doc() const noexcept override { return 0; }
			ordinal_t next() override { return strata::no_more_docs; }
			ordinal_t advance(ordinal_t) override { return strata::no_more_docs; }
			std::uint64_t cost() const noexcept override { return 0; }
			double score() override { return 1; }
			double max_score(ordinal_t, ordinal_t) override { return std::numeric_limits<double>::infinity(); }
			ordinal_t block_end(ordinal_t) override { return strata::no_more_docs; }
			double max_score_total() const noexcept override { return std::numeric_limits<double>::infinity(); }
			std::size_t score_terms() const noexcept override { return 1; }
		};
		p::scale_scorer z(std::make_unique<unbounded>(), 0.0), two(std::make_unique<unbounded>(), 2.0);
		CHECK(z.max_score_total() == 0.0 && z.max_score(0, 5) == 0.0);
		CHECK(two.max_score_total() == std::numeric_limits<double>::infinity());
	}
	// SCALE_WEIGHT's minimum for its child never skips a document whose scaled score beats it: the
	// child skips scores at or below the minimum it is given, so one rounding the wrong way loses a hit.
	{
		struct fixed final : p::scorer {
			explicit fixed(double v) : value(v) {}
			double value, min = 0;
			ordinal_t at = strata::no_more_docs;
			ordinal_t doc() const noexcept override { return at; }
			ordinal_t next() override {
				if (started) { return at = strata::no_more_docs; }
				started = true;
				return at = value > min ? 0 : strata::no_more_docs;
			}
			ordinal_t advance(ordinal_t) override { return next(); }
			std::uint64_t cost() const noexcept override { return 1; }
			double score() override { return value; }
			double max_score(ordinal_t, ordinal_t) override { return value; }
			ordinal_t block_end(ordinal_t) override { return strata::no_more_docs; }
			double max_score_total() const noexcept override { return value; }
			std::size_t score_terms() const noexcept override { return 1; }
			void set_min_competitive_score(double s) override { min = s; }
			bool started = false;
		};
		std::mt19937_64 rng(9);
		std::uniform_real_distribution<double> unit(0.01, 30.0);
		int lost = 0;
		for (int i = 0; i < 200000; ++i) {
			const double c = unit(rng), f = 0.1 + unit(rng);
			const double scaled = c * f, just_below = std::nextafter(scaled, 0.0);
			p::scale_scorer sc(std::make_unique<fixed>(c), f);
			sc.set_min_competitive_score(just_below);  // the document's score is above this
			lost += sc.next() == strata::no_more_docs;
		}
		CHECK(lost == 0);
	}
	// ELITE_SET: the size defaults to ten, a size at or above the count keeps all, the heaviest win
	// and equal weights keep query order; no children is no scorer.
	{
		std::vector<std::vector<std::string>> docs;
		for (int d = 0; d < 40; ++d) {
			docs.push_back({"t" + std::to_string(d % 13)});
			if (d % 3 == 0) { docs.back().push_back("common"); }
		}
		corpus e(std::move(docs));
		std::vector<std::string> names;
		for (int i = 0; i < 13; ++i) { names.push_back("t" + std::to_string(i)); }
		std::mt19937_64 rng(3);
		for (double size : {0.0, 1.0, 3.0, 12.0, 13.0, 20.0}) {
			std::vector<node> kids;
			for (const auto& t : names) { kids.push_back(term_node(t)); }
			const node n = op_node("ELITE_SET", std::move(kids), size);
			check_scorer(e, n, model(e, n), rng, true);
			const std::size_t kept = size == 0 ? p::default_elite_size : std::size_t(size);
			// t0 holds the most documents, so it weighs least and is the first to be left out.
			std::vector<node> top;
			for (std::size_t i = 1; i <= std::min<std::size_t>(kept, 12); ++i) { top.push_back(term_node(names[i])); }
			if (kept >= 13) { top.push_back(term_node(names[0])); }
			CHECK(model(e, n).size() == model(e, op_node("OR", std::move(top))).size());
		}
		CHECK(p::elite_set(std::vector<p::elite_child>{}, 3) == nullptr);
		std::vector<p::elite_child> one;
		one.push_back({build(e, term_node("t1")), 1.0});
		auto only = p::elite_set(std::move(one), 5);
		CHECK(only && only->next() == 1);
	}
}

}  // namespace

int main(int argc, char** argv) {
	const std::string fixtures = argc > 1 ? argv[1] : std::string(PROSPECT_FIXTURES) + "/xapian-ops";
	pieces();
	parity(fixtures);
	random_trees();
	phrase_offsets();
	host_source();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("operators_test: ok");
	return 0;
}
