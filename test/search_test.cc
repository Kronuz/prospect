// Scorers against a brute-force evaluator: disjunction, conjunction, required-plus-optional,
// exclusion, and phrases; match sets, scores, top-k, and counts.
#include "strata/search.hh"
#include "strata/writer.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::ordinal_t;

struct doc_model {
	std::vector<std::string> tokens;  // position i + 1 holds tokens[i]
};

int main() {
	std::mt19937_64 rng(11);
	const std::vector<std::string> vocab = {"a", "b", "c", "d", "e", "f", "g", "h", "rare1", "rare2"};
	for (std::size_t ndocs : {50u, 400u, 3000u, 10000u, 30000u}) {
		std::vector<doc_model> docs(ndocs);
		strata::segment_writer w;
		for (std::size_t d = 0; d < ndocs; ++d) {
			std::size_t len = 1 + rng() % 30;
			for (std::size_t i = 0; i < len; ++i) {
				std::size_t pick = rng() % 100;
				docs[d].tokens.push_back(pick < 2 ? vocab[8 + rng() % 2] : vocab[std::min<std::size_t>(7, pick / 12)]);
			}
			std::map<std::string, std::vector<std::uint32_t>> positions;
			for (std::size_t i = 0; i < docs[d].tokens.size(); ++i) { positions[docs[d].tokens[i]].push_back(std::uint32_t(i + 1)); }
			std::vector<strata::posting_input> in;
			// "h" is weighted: its wdf is twice its position count (counts differ from wdf).
			for (auto& [t, p] : positions) { in.push_back({t, std::uint32_t(p.size()) * (t == "h" ? 2 : 1), p}); }
			w.add(d + 1, in, std::uint32_t(docs[d].tokens.size()));
		}
		std::string file = w.flush({});
		strata::segment seg(file);
		// Positions in the document, and the wdf scoring reads.
		auto occurrences = [&](std::size_t d, const std::string& t) {
			return std::uint32_t(std::count(docs[d].tokens.begin(), docs[d].tokens.end(), t));
		};
		auto tf = [&](std::size_t d, const std::string& t) { return occurrences(d, t) * (t == "h" ? 2 : 1); };
		bool xapian = false;  // the round's scoring scheme
		auto weight = [&](const std::string& t) {
			auto info = seg.term(t);
			return xapian ? strata::weight::bm25_xapian(seg, info ? info->df : 0) : strata::weight::bm25(seg, info ? info->df : 0);
		};
		auto term = [&](const std::string& t) -> strata::scorer_ptr { return std::make_unique<strata::term_scorer>(seg, *seg.term(t), weight(t)); };

		for (int round = 0; round < 300; ++round) {
			xapian = round % 2 == 1;
			std::vector<std::string> pick;
			std::size_t n = 1 + rng() % 3;
			while (pick.size() < n) {
				std::string t = vocab[rng() % vocab.size()];
				if (std::find(pick.begin(), pick.end(), t) == pick.end() && seg.term(t)) { pick.push_back(t); }
			}
			const int kind = int(rng() % 15);
			const std::size_t need = kind >= 11 ? 4 : kind >= 7 ? 3 : 0;
			while (pick.size() < need) {
				std::string t = vocab[rng() % vocab.size()];
				if (std::find(pick.begin(), pick.end(), t) == pick.end() && seg.term(t)) { pick.push_back(t); }
			}
			// Phrases may repeat a term ("a b a"): one iterator per term, a slot per position.
			if ((kind == 4 || kind == 5) && pick.size() >= 2 && rng() % 3 == 0) { pick.push_back(pick[0]); }
			strata::scorer_ptr q;
			std::map<ordinal_t, double> want;
			for (std::size_t d = 0; d < ndocs; ++d) {
				double s = 0;
				bool match = false;
				switch (kind) {
				case 0: {  // OR
					for (auto& t : pick) { if (tf(d, t)) { match = true; s += weight(t)(tf(d, t), std::uint32_t(docs[d].tokens.size())); } }
					break;
				}
				case 1: {  // AND
					match = true;
					for (auto& t : pick) { if (!tf(d, t)) { match = false; } else { s += weight(t)(tf(d, t), std::uint32_t(docs[d].tokens.size())); } }
					break;
				}
				case 2: {  // pick[0] AND_MAYBE (rest)
					match = tf(d, pick[0]) > 0;
					if (match) { for (auto& t : pick) { if (tf(d, t)) { s += weight(t)(tf(d, t), std::uint32_t(docs[d].tokens.size())); } } }
					break;
				}
				case 3: {  // pick[0] AND_NOT (rest)
					match = tf(d, pick[0]) > 0;
					for (std::size_t i = 1; i < pick.size(); ++i) { if (tf(d, pick[i])) { match = false; } }
					if (match) { s = weight(pick[0])(tf(d, pick[0]), std::uint32_t(docs[d].tokens.size())); }
					break;
				}
				case 9: {  // (+pick[0] +pick[1]) pick[2]: required-plus-optional over a conjunction
					match = tf(d, pick[0]) && tf(d, pick[1]);
					if (match) {
						for (std::size_t i = 0; i < 3; ++i) {
							if (tf(d, pick[i])) { s += weight(pick[i])(tf(d, pick[i]), std::uint32_t(docs[d].tokens.size())); }
						}
					}
					break;
				}
				case 10: {  // pick[0] -pick[1] -pick[2]: an exclusion chain
					match = tf(d, pick[0]) && !tf(d, pick[1]) && !tf(d, pick[2]);
					if (match) { s = weight(pick[0])(tf(d, pick[0]), std::uint32_t(docs[d].tokens.size())); }
					break;
				}
				case 11: {  // (pick[0] OR pick[1]) AND (pick[2] OR pick[3])
					match = (tf(d, pick[0]) || tf(d, pick[1])) && (tf(d, pick[2]) || tf(d, pick[3]));
					if (match) {
						for (std::size_t i = 0; i < 4; ++i) {
							if (tf(d, pick[i])) { s += weight(pick[i])(tf(d, pick[i]), std::uint32_t(docs[d].tokens.size())); }
						}
					}
					break;
				}
				case 12: {  // (pick[0] AND pick[1]) OR (pick[2] AND_NOT pick[3])
					const std::uint32_t len = std::uint32_t(docs[d].tokens.size());
					const bool both = tf(d, pick[0]) && tf(d, pick[1]), only = tf(d, pick[2]) && !tf(d, pick[3]);
					match = both || only;
					if (both) { s += weight(pick[0])(tf(d, pick[0]), len) + weight(pick[1])(tf(d, pick[1]), len); }
					if (only) { s += weight(pick[2])(tf(d, pick[2]), len); }
					break;
				}
				case 13: {  // (pick[0] AND_MAYBE pick[1]) OR pick[2]
					const std::uint32_t len = std::uint32_t(docs[d].tokens.size());
					match = tf(d, pick[0]) || tf(d, pick[2]);
					if (tf(d, pick[0])) { s += weight(pick[0])(tf(d, pick[0]), len) + (tf(d, pick[1]) ? weight(pick[1])(tf(d, pick[1]), len) : 0.0); }
					if (tf(d, pick[2])) { s += weight(pick[2])(tf(d, pick[2]), len); }
					break;
				}
				case 14: {  // ((pick[0] OR pick[1]) AND_NOT pick[2]) AND pick[3]
					const std::uint32_t len = std::uint32_t(docs[d].tokens.size());
					match = (tf(d, pick[0]) || tf(d, pick[1])) && !tf(d, pick[2]) && tf(d, pick[3]);
					if (match) {
						for (std::size_t i : {0u, 1u, 3u}) {
							if (tf(d, pick[i])) { s += weight(pick[i])(tf(d, pick[i]), len); }
						}
					}
					break;
				}
				case 7:    // (pick[0] OR pick[1]) AND_NOT pick[2]
				case 8: {  // (pick[0] OR pick[1]) AND_MAYBE pick[2]
					match = (tf(d, pick[0]) || tf(d, pick[1])) && (kind == 8 || !tf(d, pick[2]));
					if (match) {
						for (std::size_t i = 0; i < (kind == 8 ? 3u : 2u); ++i) {
							if (tf(d, pick[i])) { s += weight(pick[i])(tf(d, pick[i]), std::uint32_t(docs[d].tokens.size())); }
						}
					}
					break;
				}
				case 6: {  // NEAR 3: every picked term (repeats counted) within 3 positions
					const auto& tk = docs[d].tokens;
					for (std::size_t i = 0; i < tk.size() && !match; ++i) {
						std::map<std::string, int> need;
						for (auto& t : pick) { ++need[t]; }
						for (std::size_t j = i; j < tk.size() && j < i + 3; ++j) {
							if (need.count(tk[j]) && --need[tk[j]] == 0) { need.erase(tk[j]); }
						}
						match = need.empty();
					}
					if (match) {
						for (auto& t : pick) { s += weight(t)(tf(d, t), std::uint32_t(tk.size())); }
					}
					break;
				}
				case 4:
				case 5: {  // phrase; 5: phrase AND its first term (two-phase in a conjunction)
					std::uint32_t freq = 0;
					const auto& tk = docs[d].tokens;
					for (std::size_t i = 0; i + pick.size() <= tk.size(); ++i) {
						bool ok = true;
						for (std::size_t j = 0; j < pick.size() && ok; ++j) { ok = tk[i + j] == pick[j]; }
						freq += ok;
					}
					match = freq > 0;
					if (match && xapian) {
						// Xapian sums each phrase term's own weight.
						for (auto& t : pick) { s += weight(t)(tf(d, t), std::uint32_t(tk.size())); }
						if (kind == 5) { s += weight(pick[0])(tf(d, pick[0]), std::uint32_t(tk.size())); }
					} else if (match) {
						double idf = 0;
						for (auto& t : pick) { idf += weight(t).idf; }
						auto wt = strata::weight::bm25(seg, 1);
						wt.idf = idf;
						s = wt(freq, std::uint32_t(tk.size()));
						if (kind == 5) { s += weight(pick[0])(tf(d, pick[0]), std::uint32_t(tk.size())); }
					}
					break;
				}
				}
				if (match) { want[ordinal_t(d)] = s; }
			}
			auto build = [&]() -> strata::scorer_ptr {
				std::vector<strata::scorer_ptr> kids;
				switch (kind) {
				case 0: for (auto& t : pick) { kids.push_back(term(t)); } return std::make_unique<strata::disjunction_scorer>(std::move(kids));
				case 1: for (auto& t : pick) { kids.push_back(term(t)); } return std::make_unique<strata::conjunction_scorer>(std::move(kids));
				case 2: case 3: {
					if (pick.size() == 1) { return term(pick[0]); }
					for (std::size_t i = 1; i < pick.size(); ++i) { kids.push_back(term(pick[i])); }
					auto rest = std::make_unique<strata::disjunction_scorer>(std::move(kids));
					if (kind == 2) { return std::make_unique<strata::required_optional_scorer>(term(pick[0]), std::move(rest)); }
					return std::make_unique<strata::exclusion_scorer>(term(pick[0]), std::move(rest));
				}
				case 7: case 8: {
					kids.push_back(term(pick[0]));
					kids.push_back(term(pick[1]));
					auto either = std::make_unique<strata::disjunction_scorer>(std::move(kids));
					if (kind == 8) { return std::make_unique<strata::required_optional_scorer>(std::move(either), term(pick[2])); }
					return std::make_unique<strata::exclusion_scorer>(std::move(either), term(pick[2]));
				}
				case 9: {
					kids.push_back(term(pick[0]));
					kids.push_back(term(pick[1]));
					return std::make_unique<strata::required_optional_scorer>(std::make_unique<strata::conjunction_scorer>(std::move(kids)), term(pick[2]));
				}
				case 10: {
					auto first = std::make_unique<strata::exclusion_scorer>(term(pick[0]), term(pick[1]));
					return std::make_unique<strata::exclusion_scorer>(std::move(first), term(pick[2]));
				}
				case 11: {
					std::vector<strata::scorer_ptr> left, right;
					left.push_back(term(pick[0]));
					left.push_back(term(pick[1]));
					right.push_back(term(pick[2]));
					right.push_back(term(pick[3]));
					kids.push_back(std::make_unique<strata::disjunction_scorer>(std::move(left)));
					kids.push_back(std::make_unique<strata::disjunction_scorer>(std::move(right)));
					return std::make_unique<strata::conjunction_scorer>(std::move(kids));
				}
				case 12: {
					std::vector<strata::scorer_ptr> both;
					both.push_back(term(pick[0]));
					both.push_back(term(pick[1]));
					kids.push_back(std::make_unique<strata::conjunction_scorer>(std::move(both)));
					kids.push_back(std::make_unique<strata::exclusion_scorer>(term(pick[2]), term(pick[3])));
					return std::make_unique<strata::disjunction_scorer>(std::move(kids));
				}
				case 13: {
					kids.push_back(std::make_unique<strata::required_optional_scorer>(term(pick[0]), term(pick[1])));
					kids.push_back(term(pick[2]));
					return std::make_unique<strata::disjunction_scorer>(std::move(kids));
				}
				case 14: {
					std::vector<strata::scorer_ptr> either;
					either.push_back(term(pick[0]));
					either.push_back(term(pick[1]));
					kids.push_back(std::make_unique<strata::exclusion_scorer>(std::make_unique<strata::disjunction_scorer>(std::move(either)), term(pick[2])));
					kids.push_back(term(pick[3]));
					return std::make_unique<strata::conjunction_scorer>(std::move(kids));
				}
				default: {
					std::vector<strata::format::term_info> infos;
					for (auto& t : pick) { infos.push_back(*seg.term(t)); }
					std::vector<strata::weight> ws;
					for (auto& t : pick) { ws.push_back(weight(t)); }
					if (kind == 6) { return std::make_unique<strata::phrase_scorer>(seg, infos, std::move(ws), 3); }
					auto phrase = xapian ? std::make_unique<strata::phrase_scorer>(seg, infos, std::move(ws)) : std::make_unique<strata::phrase_scorer>(seg, infos);
					if (kind == 4) { return phrase; }
					kids.push_back(std::move(phrase));
					kids.push_back(term(pick[0]));
					return std::make_unique<strata::conjunction_scorer>(std::move(kids));
				}
				}
			};
			q = build();
			std::map<ordinal_t, double> got;
			for (ordinal_t d = q->next(); d != strata::no_more_docs; d = q->next()) { got[d] = q->score(); }
			CHECK(got.size() == want.size());
			for (auto& [d, s] : want) {
				CHECK(got.count(d) && std::abs(got[d] - s) <= 1e-9 * std::max(1.0, s));
			}
			// top-k and count
			auto q2 = build();
			std::uint64_t c = 0;
			auto hits = strata::top_k(*q2, 10, &c);
			CHECK(c == want.size());
			std::vector<std::pair<double, ordinal_t>> sorted;
			for (auto& [d, s] : want) { sorted.push_back({-s, d}); }
			std::sort(sorted.begin(), sorted.end());
			for (std::size_t i = 0; i < hits.size(); ++i) {
				CHECK(std::abs(hits[i].score + sorted[i].first) <= 1e-9 * std::max(1.0, -sorted[i].first));
			}
			// pruned top-k (no count: thresholds feed MaxScore and block skipping) equals the
			// unpruned order, ties broken by ordinal.
			for (std::size_t k : {1u, 3u, 10u}) {
				auto q4 = build();
				auto pruned = strata::top_k(*q4, k);
				std::vector<std::pair<double, ordinal_t>> best;
				for (auto& [d, s] : want) { best.push_back({-s, d}); }
				std::sort(best.begin(), best.end());
				CHECK(pruned.size() == std::min(k, best.size()));
				for (std::size_t i = 0; i < pruned.size() && i < best.size(); ++i) {
					CHECK(std::abs(pruned[i].score + best[i].first) <= 1e-9 * std::max(1.0, -best[i].first));
					CHECK(pruned[i].ord == best[i].second || std::abs(pruned[i].score + best[i].first) <= 1e-9);
				}
			}
			// count(): exact, inclusion-exclusion, or windowed fill
			auto q5 = build();
			CHECK(strata::count(*q5) == want.size());
			// advance skips correctly
			auto q3 = build();
			ordinal_t target = ordinal_t(rng() % ndocs);
			auto it = want.lower_bound(target);
			CHECK(q3->advance(target) == (it == want.end() ? strata::no_more_docs : it->first));
			// fresh(): an unpositioned copy of a scorer already advanced counts every match;
			// every query but phrases supports it.
			auto copy = q3->fresh();
			CHECK(copy || kind == 4 || kind == 5 || kind == 6);
			if (copy) { CHECK(strata::count(*copy) == want.size()); }
		}
	}
	// Paths the random corpus rarely takes, at densities chosen for them: a rare lead with a
	// mid-density term (leapfrog counting), a rare term minus a dense one (stepping instead
	// of windows), a disjunction of six terms (heap mode, advance before next), and k = 0.
	{
		std::mt19937_64 r(5);
		const std::uint32_t n = 20000;
		const std::vector<std::pair<std::string, std::uint32_t>> density = {{"x", 1}, {"y", 20}, {"z", 100}, {"w1", 10}, {"w2", 10}, {"w3", 10}, {"w4", 10}, {"w5", 10}, {"w6", 10}};
		std::map<std::string, std::set<ordinal_t>> holds;
		strata::segment_writer w;
		for (std::uint32_t d = 0; d < n; ++d) {
			std::vector<strata::posting_input> in;
			std::vector<std::uint32_t> none;
			for (const auto& [t, pct] : density) {
				if (r() % 100 < pct) {
					in.push_back({t, 1 + std::uint32_t(r() % 3), none});
					holds[t].insert(d);
				}
			}
			std::sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.term < b.term; });
			w.add(d + 1, in, 10);
		}
		std::string file = w.flush({});
		strata::segment seg(file);
		auto term = [&](const std::string& t) -> strata::scorer_ptr { return std::make_unique<strata::term_scorer>(seg, *seg.term(t)); };
		auto both = [&](const std::string& a, const std::string& b) {
			std::size_t c = 0;
			for (auto d : holds[a]) { c += holds[b].count(d); }
			return c;
		};
		{
			std::vector<strata::scorer_ptr> kids;
			kids.push_back(term("x"));
			kids.push_back(term("y"));
			strata::conjunction_scorer q(std::move(kids));
			CHECK(strata::count(q) == both("x", "y"));
		}
		{
			strata::exclusion_scorer q(term("x"), term("z"));
			CHECK(strata::count(q) == holds["x"].size() - both("x", "z"));
			strata::exclusion_scorer p(term("y"), term("x"));
			CHECK(strata::count(p) == holds["y"].size() - both("y", "x"));
		}
		std::set<ordinal_t> any;
		for (int i = 1; i <= 6; ++i) { any.insert(holds["w" + std::to_string(i)].begin(), holds["w" + std::to_string(i)].end()); }
		auto six = [&] {
			std::vector<strata::scorer_ptr> kids;
			for (int i = 1; i <= 6; ++i) { kids.push_back(term("w" + std::to_string(i))); }
			return std::make_unique<strata::disjunction_scorer>(std::move(kids));
		};
		{
			auto q = six();
			const ordinal_t target = 7777;
			CHECK(q->advance(target) == *any.lower_bound(target));
			std::size_t seen = 1;
			while (q->next() != strata::no_more_docs) { ++seen; }
			CHECK(seen == std::size_t(std::distance(any.lower_bound(target), any.end())));
			auto c = six();
			CHECK(strata::count(*c) == any.size());
			auto all = six();
			std::uint64_t total = 0;
			const auto exhaustive = strata::top_k(*all, 10, &total);
			auto pruned_q = six();
			const auto pruned = strata::top_k(*pruned_q, 10);
			CHECK(total == any.size() && pruned.size() == 10 && exhaustive.size() == 10);
			// The same hits; scores within summation order (the paths add clauses differently).
			for (std::size_t i = 0; i < pruned.size() && i < exhaustive.size(); ++i) {
				CHECK(pruned[i].ord == exhaustive[i].ord && std::abs(pruned[i].score - exhaustive[i].score) <= 1e-9 * exhaustive[i].score);
			}
			auto none = six();
			std::uint64_t k0 = 0;
			CHECK(strata::top_k(*none, 0, &k0).empty() && k0 == any.size());
		}
	}
	// A phrase with a positioned term of wdf 0 (a boolean term): its frequency comes from
	// positions, so pruning must not bound it by wdf.
	{
		strata::segment_writer w;
		for (std::uint32_t d = 0; d < 2000; ++d) {
			std::vector<std::uint32_t> pa{1}, pb{2}, px{3};
			std::vector<strata::posting_input> in{{"a", 1, pa}, {"x", 1 + d % 7, px}};
			if (d % 2 == 0) { in.insert(in.begin() + 1, strata::posting_input{"b", 0, pb}); }  // rarer: it leads
			w.add(d + 1, in, 2 + d % 7);
		}
		std::string file = w.flush({});
		strata::segment seg(file);
		auto phrase = [&] { return std::make_unique<strata::phrase_scorer>(seg, std::vector<strata::format::term_info>{*seg.term("a"), *seg.term("b")}); };
		auto pruned = phrase();
		auto hits = strata::top_k(*pruned, 10);
		auto all = phrase();
		std::uint64_t n = 0;
		auto exhaustive = strata::top_k(*all, 10, &n);
		CHECK(n == 1000);
		CHECK(hits.size() == 10 && exhaustive.size() == 10);
		for (std::size_t i = 0; i < hits.size() && i < exhaustive.size(); ++i) { CHECK(std::abs(hits[i].score - exhaustive[i].score) <= 1e-12); }
	}
	// Identical documents tie: every path keeps the smallest ordinals, best first.
	{
		strata::segment_writer w;
		for (std::uint32_t d = 0; d < 5000; ++d) {
			std::vector<std::uint32_t> p1{1}, p2{2};
			std::vector<strata::posting_input> in{{"a", 1, p1}, {"b", 1, p2}};
			w.add(d + 1, in, 2);
		}
		std::string file = w.flush({});
		strata::segment seg(file);
		auto check = [&](strata::scorer& s, bool counted) {
			std::uint64_t n = 0;
			auto hits = counted ? strata::top_k(s, 10, &n) : strata::top_k(s, 10);
			CHECK(hits.size() == 10);
			for (std::size_t i = 0; i < hits.size(); ++i) { CHECK(hits[i].ord == i); }
		};
		for (bool counted : {false, true}) {
			strata::term_scorer t(seg, *seg.term("a"));
			check(t, counted);
			std::vector<strata::scorer_ptr> v1, v2;
			v1.push_back(std::make_unique<strata::term_scorer>(seg, *seg.term("a")));
			v1.push_back(std::make_unique<strata::term_scorer>(seg, *seg.term("b")));
			strata::disjunction_scorer u(std::move(v1));
			check(u, counted);
			v2.push_back(std::make_unique<strata::term_scorer>(seg, *seg.term("a")));
			v2.push_back(std::make_unique<strata::term_scorer>(seg, *seg.term("b")));
			strata::conjunction_scorer c(std::move(v2));
			check(c, counted);
		}
	}
	// A term indexed without positions in a phrase or NEAR: no matches, no error, in full
	// blocks and in the tail.
	{
		strata::segment_writer w;
		for (std::uint32_t d = 0; d < 300; ++d) {
			std::vector<std::uint32_t> pa{1}, none;
			std::vector<strata::posting_input> in{{"a", 1, pa}, {"p", 2, none}};
			w.add(d + 1, in, 3);
		}
		std::string file = w.flush({});
		strata::segment seg(file);
		const std::vector<strata::format::term_info> infos{*seg.term("a"), *seg.term("p")};
		strata::phrase_scorer phrase(seg, infos);
		CHECK(strata::top_k(phrase, 10).empty());
		strata::phrase_scorer near(seg, infos, {strata::weight::bm25(seg, 300), strata::weight::bm25(seg, 300)}, 5);
		std::uint64_t n = 1;
		CHECK(strata::top_k(near, 10, &n).empty() && n == 0);
	}
	std::printf("search: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
