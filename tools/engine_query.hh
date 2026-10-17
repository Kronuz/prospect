// The benchmark engine's query layer: the harness's query syntax, scorers built from it,
// and queries over one segment or an index's segments with the snapshot's statistics.
// Shared by tools/strata_engine.cc and its tests.
//   words are optional, +word is required, -word is excluded, "a b" is a phrase.
#pragma once

#include "strata/search.hh"
#include "strata/searcher.hh"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine {

struct parsed {
	std::vector<std::vector<std::string>> must, should, must_not;  // each clause: a term or a phrase
};

inline parsed parse(std::string_view q) {
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
// boost is how many times the clause repeats in its group (Xapian's wqf). With a term
// table, weights are the snapshot's, not the segment's, and each term's dictionary walk
// in each segment happens once per query (P6-3).
inline strata::scorer_ptr clause_scorer(const strata::segment& seg, const std::vector<std::string>& clause, double boost, strata::scheme scheme, strata::term_table* table = nullptr) {
	std::vector<strata::format::term_info> infos;
	for (const auto& t : clause) {
		if (table) {
			const auto* info = table->info(seg, t);
			if (!info) { return nullptr; }
			infos.push_back(*info);
		} else {
			auto info = seg.term(t);
			if (!info) { return nullptr; }
			infos.push_back(*info);
		}
	}
	const bool xapian = scheme == strata::scheme::bm25_xapian;
	if (infos.size() == 1) {
		if (table) {
			const auto df = table->df(clause[0]);
			const auto& st = table->stats();
			return std::make_unique<strata::term_scorer>(seg, infos[0], xapian ? strata::weight::bm25_xapian(st, df, std::uint32_t(boost)) : strata::weight::bm25(st, df, boost));
		}
		if (xapian) { return std::make_unique<strata::term_scorer>(seg, infos[0], strata::weight::bm25_xapian(seg, infos[0].df, std::uint32_t(boost))); }
		return std::make_unique<strata::term_scorer>(seg, infos[0], boost);
	}
	if (!xapian) {
		if (!table) { return std::make_unique<strata::phrase_scorer>(seg, infos); }
		std::vector<std::uint64_t> dfs;
		for (const auto& t : clause) { dfs.push_back(table->df(t)); }
		return std::make_unique<strata::phrase_scorer>(seg, infos, std::vector<strata::weight>{}, 0, &table->stats(), dfs);
	}
	std::vector<strata::weight> weights;
	for (std::size_t k = 0; k < infos.size(); ++k) {
		weights.push_back(table ? strata::weight::bm25_xapian(table->stats(), table->df(clause[k])) : strata::weight::bm25_xapian(seg, infos[k].df));
	}
	return std::make_unique<strata::phrase_scorer>(seg, infos, std::move(weights));
}

// Repeated clauses in a group merge into one with the repeat count as boost, as Lucene's
// BooleanQuery rewrite does; BM25 is linear in the boost, so scores are unchanged. Phrases
// keep their repeats.
inline std::vector<std::pair<std::vector<std::string>, double>> merge_repeats(const std::vector<std::vector<std::string>>& clauses) {
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

inline strata::scorer_ptr build(const strata::segment& seg, const parsed& q, strata::scheme scheme = strata::scheme::bm25, strata::term_table* table = nullptr) {
	std::vector<strata::scorer_ptr> must, should, must_not;
	for (const auto& [c, boost] : merge_repeats(q.must)) {
		auto s = clause_scorer(seg, c, boost, scheme, table);
		if (!s) { return nullptr; }
		must.push_back(std::move(s));
	}
	for (const auto& [c, boost] : merge_repeats(q.should)) {
		if (auto s = clause_scorer(seg, c, boost, scheme, table)) { should.push_back(std::move(s)); }
	}
	for (const auto& c : q.must_not) {
		if (auto s = clause_scorer(seg, c, 1.0, scheme, table)) { must_not.push_back(std::move(s)); }
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

inline std::uint64_t run(const strata::segment& seg, std::string_view command, std::string_view query, strata::scheme scheme = strata::scheme::bm25) {
	const parsed q = parse(query);
	const bool counting = command == "COUNT" || command.ends_with("_COUNT");
	// A single term's count is its document frequency (one segment, no deletes).
	if (command == "COUNT" && q.must.size() + q.should.size() == 1 && q.must_not.empty() && (q.must.empty() ? q.should[0] : q.must[0]).size() == 1) {
		auto info = seg.term((q.must.empty() ? q.should[0] : q.must[0])[0]);
		return info ? info->df : 0;
	}
	auto s = build(seg, q, scheme);
	if (!s) { return counting ? 0 : 1; }
	if (command == "COUNT") { return strata::count(*s); }
	std::size_t k = 0;
	std::from_chars(command.data() + 4, command.data() + command.size(), k);
	std::uint64_t n = 0;
	strata::top_k(seg, *s, k, counting ? &n : nullptr);
	return counting ? n : 1;
}

// The k best documents of a query over an index's segments, weighted by the snapshot's
// statistics (D13); with count, also the number of live matches.
inline std::vector<strata::doc_hit> search_snapshot(std::span<const strata::segment_snapshot> segs, const parsed& q, std::size_t k, strata::scheme scheme = strata::scheme::bm25, std::uint64_t* count = nullptr) {
	strata::term_table table(segs);
	auto builder = [&](const strata::segment& seg) { return build(seg, q, scheme, &table); };
	return strata::search(segs, builder, k, count);
}

// The one word a query reduces to (every clause that word, nothing excluded), or null.
inline const std::string* single_term(const parsed& q) {
	if (!q.must_not.empty()) { return nullptr; }
	const std::string* t = nullptr;
	for (const auto* group : {&q.must, &q.should}) {
		for (const auto& c : *group) {
			if (c.size() != 1 || (t && *t != c[0])) { return nullptr; }
			t = &c[0];
		}
	}
	return t;
}

// The number of live matches of a query over an index's segments. A single word's count
// in a segment without deletes is its document frequency there, from the term table (P6-7).
inline std::uint64_t count_snapshot(std::span<const strata::segment_snapshot> segs, const parsed& q, strata::scheme scheme = strata::scheme::bm25) {
	strata::term_table table(segs);
	auto builder = [&](const strata::segment& seg) { return build(seg, q, scheme, &table); };
	const std::string* term = single_term(q);
	if (!term) { return strata::search_count(segs, builder); }
	std::uint64_t n = 0;
	for (const auto& s : segs) {
		if (!s.any_deleted()) {
			n += table.df(*s.seg, *term);
			continue;
		}
		const strata::segment_snapshot one[] = {s};
		n += strata::search_count(one, builder);
	}
	return n;
}

// A query over an index's segments, as serve and churn answer it: the count for COUNT and
// TOP_*_COUNT, else 1.
inline std::uint64_t run(std::span<const strata::segment_snapshot> segs, std::string_view command, std::string_view query, strata::scheme scheme = strata::scheme::bm25) {
	const parsed q = parse(query);
	if (command == "COUNT") { return count_snapshot(segs, q, scheme); }
	std::size_t k = 0;
	std::from_chars(command.data() + 4, command.data() + command.size(), k);
	std::uint64_t n = 0;
	(void)search_snapshot(segs, q, k, scheme, command.ends_with("_COUNT") ? &n : nullptr);
	return command.ends_with("_COUNT") ? n : 1;
}

}  // namespace engine
