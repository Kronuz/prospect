// Relevance parity with Xapian (docs/design/phase-2.md, "Parity harness"): builds the
// oracle's index (the first N corpus documents, analyzed by analysis::term_generator with
// TermGenerator's defaults), rebuilds each recorded query from its
// Query(...) description, and compares match counts and top-10 (docid, weight) against
// the oracle's rank vectors at relative error <= 1e-9, ordering near-ties by docid, both
// counted and pruned.
// With SEGMENTS > 1, the same documents are split into that many contiguous segments,
// weighted with the snapshot's statistics and searched across segments (Phase 3, T8).
// With PARITY_REORDER=MIN_DF, those segments are merged into one reordered segment
// (reordering.md D4, graph terms of at least MIN_DF documents) and searched as one.
//   parity CORPUS_JSON N RANK_JSONL [SEGMENTS]
#include "analysis.hh"
#include "strata/reorder.hh"
#include "strata/search.hh"
#include "strata/searcher.hh"
#include "strata/writer.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

std::string_view text_field(std::string_view line) {
	auto at = line.find("\"text\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 9;
	return line.substr(at, line.find('"', at) - at);
}

// A parsed Xapian query description node.
struct node {
	std::string op;      // "" for a term leaf
	std::string term;
	unsigned window = 0; // PHRASE / NEAR / ELITE_SET parameter
	std::vector<node> children;
};

class description_parser {
public:
	explicit description_parser(std::string_view s) : s_(s) {}

	node parse_query() {
		expect("Query(");
		node n = parse_expr();
		expect(")");
		return n;
	}

private:
	node parse_expr() {
		if (peek() == '(') {
			++i_;
			node n;
			n.children.push_back(parse_expr());
			while (peek() != ')') {
				std::string op = word();
				unsigned window = 0;
				if (op == "PHRASE" || op == "NEAR" || op == "ELITE_SET") { window = unsigned(std::stoul(word())); }
				if (n.op.empty()) { n.op = op, n.window = window; }
				n.children.push_back(parse_expr());
			}
			++i_;
			return n;
		}
		node leaf;
		std::string w = word();
		leaf.term = w.substr(0, w.rfind('@'));
		return leaf;
	}

	std::string word() {
		while (i_ < s_.size() && s_[i_] == ' ') { ++i_; }
		std::size_t start = i_;
		while (i_ < s_.size() && s_[i_] != ' ' && s_[i_] != ')') { ++i_; }
		return std::string(s_.substr(start, i_ - start));
	}
	char peek() {
		while (i_ < s_.size() && s_[i_] == ' ') { ++i_; }
		return i_ < s_.size() ? s_[i_] : '\0';
	}
	void expect(std::string_view t) {
		if (s_.substr(i_, t.size()) != t) { throw std::runtime_error("unexpected description: " + std::string(s_)); }
		i_ += t.size();
	}

	std::string_view s_;
	std::size_t i_ = 0;
};

void count_terms(const node& n, std::map<std::string, std::uint32_t>& wqf) {
	if (n.op.empty()) { ++wqf[n.term]; }
	for (const auto& c : n.children) { count_terms(c, wqf); }
}

// The index's statistics when searching several segments; null for one segment.
struct snapshot_weights {
	std::span<const strata::segment_snapshot> segs;
	strata::statistics st;
	strata::weight of(const std::string& term, std::uint32_t wqf) const { return strata::weight::bm25_xapian(st, strata::snapshot_df(segs, term), wqf); }
};

strata::scorer_ptr build(const strata::segment& seg, const node& n, const std::map<std::string, std::uint32_t>& wqf, const snapshot_weights* snap = nullptr) {
	if (n.op.empty()) {
		auto info = seg.get<strata::inverted_plugin>().term(n.term);
		if (!info) { return nullptr; }
		return std::make_unique<strata::term_scorer>(seg.get<strata::inverted_plugin>(), *info, snap ? snap->of(n.term, wqf.at(n.term)) : strata::weight::bm25_xapian(seg.get<strata::inverted_plugin>(), info->df, wqf.at(n.term)));
	}
	if (n.op == "PHRASE" || n.op == "NEAR") {
		std::vector<strata::format::term_info> infos;
		std::vector<strata::weight> weights;
		for (const auto& c : n.children) {
			auto info = seg.get<strata::inverted_plugin>().term(c.term);
			if (!info) { return nullptr; }
			infos.push_back(*info);
			weights.push_back(snap ? snap->of(c.term, wqf.at(c.term)) : strata::weight::bm25_xapian(seg.get<strata::inverted_plugin>(), info->df, wqf.at(c.term)));
		}
		return std::make_unique<strata::phrase_scorer>(seg.get<strata::inverted_plugin>(), infos, std::move(weights), n.op == "NEAR" ? n.window : 0);
	}
	std::vector<strata::scorer_ptr> kids;
	for (const auto& c : n.children) { kids.push_back(build(seg, c, wqf, snap)); }
	if (n.op == "OR") {
		std::erase(kids, nullptr);
		if (kids.empty()) { return nullptr; }
		if (kids.size() == 1) { return std::move(kids[0]); }
		return std::make_unique<strata::disjunction_scorer>(std::move(kids));
	}
	if (n.op == "AND") {
		for (const auto& k : kids) {
			if (!k) { return nullptr; }
		}
		return std::make_unique<strata::conjunction_scorer>(std::move(kids));
	}
	if (n.op == "AND_MAYBE") {
		if (!kids[0]) { return nullptr; }
		std::vector<strata::scorer_ptr> rest;
		for (std::size_t i = 1; i < kids.size(); ++i) {
			if (kids[i]) { rest.push_back(std::move(kids[i])); }
		}
		if (rest.empty()) { return std::move(kids[0]); }
		strata::scorer_ptr opt = rest.size() == 1 ? std::move(rest[0]) : std::make_unique<strata::disjunction_scorer>(std::move(rest));
		return std::make_unique<strata::required_optional_scorer>(std::move(kids[0]), std::move(opt));
	}
	if (n.op == "AND_NOT") {
		if (!kids[0]) { return nullptr; }
		std::vector<strata::scorer_ptr> rest;
		for (std::size_t i = 1; i < kids.size(); ++i) {
			if (kids[i]) { rest.push_back(std::move(kids[i])); }
		}
		if (rest.empty()) { return std::move(kids[0]); }
		strata::scorer_ptr ex = rest.size() == 1 ? std::move(rest[0]) : std::make_unique<strata::disjunction_scorer>(std::move(rest));
		return std::make_unique<strata::exclusion_scorer>(std::move(kids[0]), std::move(ex));
	}
	throw std::runtime_error("unsupported operator " + n.op);
}

// Minimal extraction of the rank vector fields.
std::string field(const std::string& line, const std::string& key) {
	auto at = line.find("\"" + key + "\":");
	if (at == std::string::npos) { return {}; }
	at += key.size() + 3;
	if (line[at] == '"') {
		std::string out;
		for (std::size_t i = at + 1; i < line.size() && line[i] != '"'; ++i) {
			if (line[i] == '\\') { ++i; }
			out.push_back(line[i]);
		}
		return out;
	}
	return line.substr(at, line.find_first_of(",}", at) - at);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 4 && argc != 5) {
		std::cerr << "usage: parity CORPUS_JSON N RANK_JSONL [SEGMENTS]\n";
		return 2;
	}
	const std::size_t n = std::stoul(argv[2]);
	std::size_t parts = argc == 5 ? std::max<std::size_t>(1, std::stoul(argv[4])) : 1;
	std::vector<strata::segment_writer> writers(parts);
	{
		std::ifstream in(argv[1]);
		std::string line;
		analysis::term_generator g;
		std::map<std::string, std::pair<std::uint32_t, std::vector<std::uint32_t>>> terms;
		std::vector<strata::posting_input> postings;
		for (std::size_t d = 0; d < n && std::getline(in, line); ++d) {
			terms.clear();
			std::uint32_t length = 0;
			g.set_termpos(0);
			g.index_text(text_field(line), [&](std::string_view term, std::uint32_t wdf, std::uint32_t pos) {
				auto& t = terms[std::string(term)];
				t.first += wdf;
				length += wdf;
				if (pos) { t.second.push_back(pos); }
			});
			postings.clear();
			for (const auto& [term, t] : terms) { postings.push_back({term, t.first, t.second}); }
			writers[d * parts / n].add(d + 1, postings, length);
		}
	}
	std::vector<std::string> files;
	for (auto& w : writers) { files.push_back(w.flush({})); }
	std::vector<strata::segment> segments;
	for (const auto& f : files) { segments.emplace_back(f); }
	if (const char* r = std::getenv("PARITY_REORDER")) {
		std::vector<strata::merge_source> sources;
		for (const auto& sg : segments) { sources.push_back({&sg, nullptr}); }
		strata::reorder::settings set;
		set.min_df = std::uint32_t(std::stoul(r));
		strata::string_scratch forward;
		const auto order = strata::reorder::order(sources, forward, set);
		std::string merged;
		strata::string_output out(merged);
		strata::string_spill spill;
		(void)strata::merge_segments(sources, {}, out, spill, {}, strata::format::default_region_shift, nullptr, nullptr, order);
		segments.clear();
		files = {std::move(merged)};
		segments.emplace_back(files[0]);
		parts = 1;
		std::cout << "reordered: " << (segments[0].core().docid_ordered() ? "no" : "yes") << "\n";
	}
	std::vector<strata::segment_snapshot> snaps;
	for (const auto& s : segments) { snaps.push_back({&s, nullptr}); }
	const snapshot_weights weights{snaps, strata::snapshot_statistics(snaps)};
	const snapshot_weights* snap = parts > 1 ? &weights : nullptr;
	const strata::segment& seg = segments[0];
	std::uint64_t total_docs = 0;
	for (const auto& s : segments) { total_docs += s.core().doc_count(); }
	// The k best across segments as hits by docid - 1 (the ordinal a docid-ordered segment gives).
	auto search = [&](const node& q, const std::map<std::string, std::uint32_t>& wqf, std::uint64_t* count) {
		std::vector<strata::hit> out;
		if (parts == 1) {
			if (auto s = build(seg, q, wqf)) { out = strata::top_k(seg, *s, 10, count); } else if (count) { *count = 0; }
			for (auto& h : out) { h.ord = strata::ordinal_t(seg.core().docid(h.ord) - 1); }
			return out;
		}
		auto hits = strata::search(snaps, [&](const strata::segment& sg) { return build(sg, q, wqf, snap); }, 10, count);
		for (const auto& h : hits) { out.push_back({strata::ordinal_t(h.docid - 1), h.score}); }
		return out;
	};

	std::ifstream rank(argv[3]);
	std::string line;
	std::size_t queries = 0, count_ok = 0, top_ok = 0, max_rel_shown = 0;
	double worst = 0;
	while (std::getline(rank, line)) {
		if (line.find("\"index\"") != std::string::npos) {
			std::cout << "oracle index: " << line << "  strata docs " << total_docs << " in " << parts << " segment(s)\n";
			continue;
		}
		if (field(line, "check_at_least") != std::to_string(total_docs)) { continue; }  // the exact-count rows
		++queries;
		const std::string desc = field(line, "description");
		node q = description_parser(desc).parse_query();
		std::map<std::string, std::uint32_t> wqf;
		count_terms(q, wqf);
		const std::uint64_t want_count = std::stoull(field(line, "upper"));
		std::uint64_t got_count = 0;
		std::vector<strata::hit> hits = search(q, wqf, &got_count);
		count_ok += got_count == want_count;

		// Oracle hits: [[docid, weight, percent], ...]
		std::vector<std::pair<std::uint64_t, double>> want;
		auto at = line.find("\"hits\":[");
		for (std::size_t p = line.find('[', at + 8); p != std::string::npos && p < line.size(); p = line.find('[', p + 1)) {
			std::size_t c1 = line.find(',', p), c2 = line.find(',', c1 + 1);
			want.push_back({std::stoull(line.substr(p + 1, c1 - p - 1)), std::stod(line.substr(c1 + 1, c2 - c1 - 1))});
		}
		auto matches = [&](const std::vector<strata::hit>& got) {
			bool ok = got.size() == want.size();
			for (std::size_t i = 0; ok && i < got.size(); ++i) {
				const double rel = std::abs(got[i].score - want[i].second) / std::max(1e-300, std::abs(want[i].second));
				worst = std::max(worst, rel);
				const bool same_doc = got[i].ord + 1 == want[i].first;
				// Near-ties (within 1e-9) may order differently; accept a docid swap among them.
				bool tie = false;
				for (const auto& w2 : want) { tie = tie || (w2.first == got[i].ord + 1 && std::abs(w2.second - got[i].score) <= 1e-9 * std::abs(w2.second)); }
				ok = rel <= 1e-9 && (same_doc || tie);
			}
			return ok;
		};
		// Counted (every match visited) and pruned (no count: thresholds skip blocks).
		std::vector<strata::hit> pruned = search(q, wqf, nullptr);
		const bool ok = matches(hits) && matches(pruned);
		top_ok += ok;
		if ((!ok || got_count != want_count) && max_rel_shown++ < 8) {
			std::cout << "MISMATCH " << desc << " count " << got_count << " vs " << want_count;
			if (!hits.empty() && !want.empty()) {
				std::printf(" top1 %llu %.17g vs %llu %.17g", (unsigned long long)(hits[0].ord + 1), hits[0].score, (unsigned long long)want[0].first, want[0].second);
			}
			std::cout << '\n';
		}
	}
	std::printf("queries %zu: counts equal %zu, top-10 equal %zu, worst relative error %.3g\n", queries, count_ok, top_ok, worst);
	return count_ok == queries && top_ok == queries ? 0 : 1;
}
