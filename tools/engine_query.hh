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
#include <unordered_map>
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

// A multi-segment search's statistics: the snapshot's totals and each term's document
// frequency over every segment (D13), cached per query.
struct snapshot_stats {
	std::span<const strata::segment_snapshot> segs;
	strata::statistics st;
	std::unordered_map<std::string, std::uint64_t> df;
	explicit snapshot_stats(std::span<const strata::segment_snapshot> s) : segs(s), st(strata::snapshot_statistics(s)) {}
	std::uint64_t of(const std::string& term) {
		auto [it, fresh] = df.try_emplace(term, 0);
		if (fresh) { it->second = strata::snapshot_df(segs, term); }
		return it->second;
	}
};

// A clause's scorer, or null when one of its terms is absent (the clause matches nothing).
// boost is how many times the clause repeats in its group (Xapian's wqf). With snapshot
// statistics, weights are the index's, not the segment's.
inline strata::scorer_ptr clause_scorer(const strata::segment& seg, const std::vector<std::string>& clause, double boost, strata::scheme scheme, snapshot_stats* snap = nullptr) {
	std::vector<strata::format::term_info> infos;
	for (const auto& t : clause) {
		auto info = seg.term(t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
	}
	const bool xapian = scheme == strata::scheme::bm25_xapian;
	if (infos.size() == 1) {
		if (snap) {
			const auto df = snap->of(clause[0]);
			return std::make_unique<strata::term_scorer>(seg, infos[0], xapian ? strata::weight::bm25_xapian(snap->st, df, std::uint32_t(boost)) : strata::weight::bm25(snap->st, df, boost));
		}
		if (xapian) { return std::make_unique<strata::term_scorer>(seg, infos[0], strata::weight::bm25_xapian(seg, infos[0].df, std::uint32_t(boost))); }
		return std::make_unique<strata::term_scorer>(seg, infos[0], boost);
	}
	if (!xapian) {
		if (!snap) { return std::make_unique<strata::phrase_scorer>(seg, infos); }
		std::vector<std::uint64_t> dfs;
		for (const auto& t : clause) { dfs.push_back(snap->of(t)); }
		return std::make_unique<strata::phrase_scorer>(seg, infos, std::vector<strata::weight>{}, 0, &snap->st, dfs);
	}
	std::vector<strata::weight> weights;
	for (std::size_t k = 0; k < infos.size(); ++k) {
		weights.push_back(snap ? strata::weight::bm25_xapian(snap->st, snap->of(clause[k])) : strata::weight::bm25_xapian(seg, infos[k].df));
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

inline strata::scorer_ptr build(const strata::segment& seg, const parsed& q, strata::scheme scheme = strata::scheme::bm25, snapshot_stats* snap = nullptr) {
	std::vector<strata::scorer_ptr> must, should, must_not;
	for (const auto& [c, boost] : merge_repeats(q.must)) {
		auto s = clause_scorer(seg, c, boost, scheme, snap);
		if (!s) { return nullptr; }
		must.push_back(std::move(s));
	}
	for (const auto& [c, boost] : merge_repeats(q.should)) {
		if (auto s = clause_scorer(seg, c, boost, scheme, snap)) { should.push_back(std::move(s)); }
	}
	for (const auto& c : q.must_not) {
		if (auto s = clause_scorer(seg, c, 1.0, scheme, snap)) { must_not.push_back(std::move(s)); }
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
	strata::top_k(*s, k, counting ? &n : nullptr);
	return counting ? n : 1;
}

// The k best documents of a query over an index's segments, weighted by the snapshot's
// statistics (D13); with count, also the number of live matches.
inline std::vector<strata::doc_hit> search_snapshot(std::span<const strata::segment_snapshot> segs, const parsed& q, std::size_t k, strata::scheme scheme = strata::scheme::bm25, std::uint64_t* count = nullptr) {
	snapshot_stats snap(segs);
	auto builder = [&](const strata::segment& seg) { return build(seg, q, scheme, &snap); };
	return strata::search(segs, builder, k, count);
}

// The number of live matches of a query over an index's segments.
inline std::uint64_t count_snapshot(std::span<const strata::segment_snapshot> segs, const parsed& q, strata::scheme scheme = strata::scheme::bm25) {
	snapshot_stats snap(segs);
	auto builder = [&](const strata::segment& seg) { return build(seg, q, scheme, &snap); };
	return strata::search_count(segs, builder);
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
