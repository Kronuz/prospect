// strata as a search-benchmark-game engine.
//   strata_engine index DIR < corpus.json   one segment from JSON lines ("text": whitespace tokens)
//   strata_engine serve DIR                  "COMMAND\tquery" lines -> one count per line
//   strata_engine terms DIR                  every term and its document frequency
//   strata_engine verify DIR < queries.txt   pruned top-k equals an exhaustive scan, k = 10, 100, 1000
//   strata_engine bench DIR COMMAND RUNS < queries.txt
//                                            per query: fastest of RUNS in-process runs, in µs
// Query syntax: words are optional, +word is required, -word is excluded, "a b" is a phrase.
#include "residency.hh"
#include "strata/search.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string_view text_field(std::string_view line) {
	auto at = line.find("\"text\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 9;
	return line.substr(at, line.find('"', at) - at);
}

// The "id" field, stored like the baseline engines' stored id.
std::string_view id_field(std::string_view line) {
	auto at = line.find("\"id\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 7;
	return line.substr(at, line.find('"', at) - at);
}

int index(const std::string& dir) {
	std::ios::sync_with_stdio(false);
	strata::segment_writer w;
	std::string line;
	std::vector<std::pair<std::string_view, std::uint32_t>> tokens;  // (token, position)
	std::vector<std::uint32_t> positions;
	std::vector<strata::posting_input> in;
	std::uint64_t n = 0;
	while (std::getline(std::cin, line)) {
		if (line.empty()) { continue; }
		std::string_view text = text_field(line);
		tokens.clear();
		std::uint32_t pos = 0;
		for (std::size_t i = 0; i < text.size();) {
			std::size_t j = text.find(' ', i);
			if (j == std::string_view::npos) { j = text.size(); }
			if (j > i) { tokens.push_back({text.substr(i, j - i), ++pos}); }
			i = j + 1;
		}
		// Group a document's tokens by sorting: each term's positions end up contiguous.
		std::sort(tokens.begin(), tokens.end());
		positions.clear();
		for (const auto& t : tokens) { positions.push_back(t.second); }
		in.clear();
		for (std::size_t i = 0; i < tokens.size();) {
			std::size_t j = i;
			while (j < tokens.size() && tokens[j].first == tokens[i].first) { ++j; }
			in.push_back({tokens[i].first, std::uint32_t(j - i), std::span<const std::uint32_t>(positions.data() + i, j - i)});
			i = j;
		}
		w.add(++n, in, pos, id_field(line));
		if (n % 100000 == 0) { std::cerr << n << '\n'; }
	}
	{
		strata::file_output out(dir + "/segment.sx");
		strata::file_spill spill(dir + "/segment.positions.tmp");
		w.flush({}, out, spill);
		out.close();
	}
	std::ifstream written(dir + "/segment.sx", std::ios::binary | std::ios::ate);
	std::cerr << "segment: " << written.tellg() << " bytes, " << n << " documents\n";
	return 0;
}

struct parsed {
	std::vector<std::vector<std::string>> must, should, must_not;  // each clause: a term or a phrase
};

parsed parse(std::string_view q) {
	parsed out;
	for (std::size_t i = 0; i < q.size();) {
		if (q[i] == ' ') { ++i; continue; }
		char op = 0;
		if (q[i] == '+' || q[i] == '-') { op = q[i++]; }
		std::vector<std::string> clause;
		if (i < q.size() && q[i] == '"') {
			std::size_t end = q.find('"', i + 1);
			if (end == std::string_view::npos) { end = q.size(); }
			std::string_view phrase = q.substr(i + 1, end - i - 1);
			for (std::size_t a = 0; a < phrase.size();) {
				std::size_t b = phrase.find(' ', a);
				if (b == std::string_view::npos) { b = phrase.size(); }
				if (b > a) { clause.emplace_back(phrase.substr(a, b - a)); }
				a = b + 1;
			}
			i = end + 1;
		} else {
			std::size_t end = q.find(' ', i);
			if (end == std::string_view::npos) { end = q.size(); }
			clause.emplace_back(q.substr(i, end - i));
			i = end;
		}
		if (clause.empty()) { continue; }
		(op == '+' ? out.must : op == '-' ? out.must_not : out.should).push_back(std::move(clause));
	}
	return out;
}

// A clause's scorer, or null when one of its terms is absent (the clause matches nothing).
// boost is how many times the clause repeats in its group.
strata::scorer_ptr clause_scorer(const strata::segment& seg, const std::vector<std::string>& clause, double boost = 1.0) {
	std::vector<strata::format::term_info> infos;
	for (const auto& t : clause) {
		auto info = seg.term(t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
	}
	if (infos.size() == 1) { return std::make_unique<strata::term_scorer>(seg, infos[0], boost); }
	return std::make_unique<strata::phrase_scorer>(seg, infos);
}

// Repeated clauses in a group merge into one with the repeat count as boost, as Lucene's
// BooleanQuery rewrite does; BM25 is linear in the boost, so scores are unchanged. Phrases
// keep their repeats.
std::vector<std::pair<std::vector<std::string>, double>> merge_repeats(const std::vector<std::vector<std::string>>& clauses) {
	std::vector<std::pair<std::vector<std::string>, double>> out;
	for (const auto& c : clauses) {
		auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.first == c; });
		if (it != out.end() && c.size() == 1) {
			it->second += 1.0;
		} else {
			out.push_back({c, 1.0});
		}
	}
	return out;
}

strata::scorer_ptr build(const strata::segment& seg, const parsed& q) {
	std::vector<strata::scorer_ptr> must, should, must_not;
	for (const auto& [c, boost] : merge_repeats(q.must)) {
		auto s = clause_scorer(seg, c, boost);
		if (!s) { return nullptr; }
		must.push_back(std::move(s));
	}
	for (const auto& [c, boost] : merge_repeats(q.should)) {
		if (auto s = clause_scorer(seg, c, boost)) { should.push_back(std::move(s)); }
	}
	for (const auto& c : q.must_not) {
		if (auto s = clause_scorer(seg, c)) { must_not.push_back(std::move(s)); }
	}
	auto combine = [](std::vector<strata::scorer_ptr> v, bool all) -> strata::scorer_ptr {
		if (v.empty()) { return nullptr; }
		if (v.size() == 1) { return std::move(v[0]); }
		if (all) { return std::make_unique<strata::conjunction_scorer>(std::move(v)); }
		return std::make_unique<strata::disjunction_scorer>(std::move(v));
	};
	strata::scorer_ptr req = combine(std::move(must), true), opt = combine(std::move(should), false);
	strata::scorer_ptr result;
	if (req && opt) {
		result = std::make_unique<strata::required_optional_scorer>(std::move(req), std::move(opt));
	} else {
		result = req ? std::move(req) : std::move(opt);
	}
	// A - (B or C) as (A - B) - C: each exclusion checks one clause, without a union.
	for (auto& ex : must_not) {
		if (result) { result = std::make_unique<strata::exclusion_scorer>(std::move(result), std::move(ex)); }
	}
	return result;
}

std::uint64_t run(const strata::segment& seg, std::string_view command, std::string_view query) {
	const parsed q = parse(query);
	const bool counting = command == "COUNT" || command.ends_with("_COUNT");
	// A single term's count is its document frequency (one segment, no deletes).
	if (command == "COUNT" && q.must.size() + q.should.size() == 1 && q.must_not.empty() && (q.must.empty() ? q.should[0] : q.must[0]).size() == 1) {
		auto info = seg.term((q.must.empty() ? q.should[0] : q.must[0])[0]);
		return info ? info->df : 0;
	}
	auto s = build(seg, q);
	if (!s) { return counting ? 0 : 1; }
	if (command == "COUNT") { return strata::count(*s); }
	std::size_t k = 0;
	std::from_chars(command.data() + 4, command.data() + command.size(), k);
	std::uint64_t n = 0;
	strata::top_k(*s, k, counting ? &n : nullptr);
	return counting ? n : 1;
}

strata::segment open(const std::string& dir, residency::file& file) {
	file = residency::file::open(dir + "/segment.sx");
	auto bytes = file.view({0, file.size()});
	return strata::segment(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

int serve(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::string line;
	std::cout.setf(std::ios::unitbuf);
	while (std::getline(std::cin, line)) {
		const std::size_t tab = line.find('\t');
		std::cout << run(seg, std::string_view(line.data(), tab), std::string_view(line).substr(tab + 1)) << '\n';
	}
	return 0;
}

int terms(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::ios::sync_with_stdio(false);
	seg.dictionary().scan("", [](std::string_view key, std::string_view value) {
		std::cout << key << '\t' << strata::format::decode_term_info(value).df << '\n';
		return true;
	});
	return 0;
}

std::string query_of(const std::string& line) {
	auto at = line.find("\"query\": \"");
	if (at == std::string::npos) { return {}; }
	std::string q;
	for (std::size_t i = at + 10; i < line.size() && line[i] != '"'; ++i) { q.push_back(line[i] == '\\' ? line[++i] : line[i]); }
	return q;
}

// For every query: the pruned top-k against a scan that scores every match, rank by rank
// (scores within 1e-9 relative; equal scores may order differently), and the counts.
int verify(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::size_t queries = 0, mismatches = 0;
	for (std::string line; std::getline(std::cin, line);) {
		const std::string q = query_of(line);
		if (q.empty()) { continue; }
		++queries;
		const parsed p = parse(q);
		for (std::size_t k : {10u, 100u, 1000u}) {
			auto pruned = build(seg, p), all = build(seg, p);
			if (!pruned) { break; }
			const auto hits = strata::top_k(*pruned, k);
			std::vector<strata::hit> want;
			std::uint64_t n = 0;
			for (auto d = all->next(); d != strata::no_more_docs; d = all->next(), ++n) { want.push_back({d, all->score()}); }
			std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.score != b.score ? a.score > b.score : a.ord < b.ord; });
			want.resize(std::min<std::size_t>(k, want.size()));
			bool ok = hits.size() == want.size();
			for (std::size_t i = 0; ok && i < hits.size(); ++i) { ok = std::abs(hits[i].score - want[i].score) <= 1e-9 * std::max(1.0, want[i].score); }
			auto counted = build(seg, p);
			ok = ok && strata::count(*counted) == n;
			if (!ok) {
				++mismatches;
				std::cout << "MISMATCH k=" << k << '\t' << q << '\n';
			}
		}
	}
	std::cout << queries << " queries, " << mismatches << " mismatches\n";
	return mismatches ? 1 : 0;
}

// queries.txt is the harness's JSON lines: {"query": "...", "tags": [...]}.
int bench(const std::string& dir, std::string_view command, int runs) {
	residency::file file;
	strata::segment seg = open(dir, file);
	for (std::string line; std::getline(std::cin, line);) {
		auto at = line.find("\"query\": \"");
		if (at == std::string::npos) { continue; }
		std::string unescaped;
		for (std::size_t i = at + 10; i < line.size() && line[i] != '"'; ++i) { unescaped.push_back(line[i] == '\\' ? line[++i] : line[i]); }
		double best = 1e300;
		std::uint64_t result = 0;
		for (int r = 0; r < runs; ++r) {
			auto t0 = std::chrono::steady_clock::now();
			result = run(seg, command, unescaped);
			best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
		std::cout << std::fixed << std::setprecision(1) << best << '\t' << result << '\t' << unescaped << '\n';
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "index" && argc == 3) { return index(argv[2]); }
	if (mode == "serve" && argc == 3) { return serve(argv[2]); }
	if (mode == "terms" && argc == 3) { return terms(argv[2]); }
	if (mode == "verify" && argc == 3) { return verify(argv[2]); }
	if (mode == "bench" && argc == 5) { return bench(argv[2], argv[3], std::atoi(argv[4])); }
	std::cerr << "usage: strata_engine index|serve|terms|verify DIR | bench DIR COMMAND RUNS < queries\n";
	return 2;
}
