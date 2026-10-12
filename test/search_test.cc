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
			for (auto& [t, p] : positions) { in.push_back({t, std::uint32_t(p.size()), p}); }
			w.add(d + 1, in, std::uint32_t(docs[d].tokens.size()));
		}
		std::string file = w.flush({});
		strata::segment seg(file);
		auto tf = [&](std::size_t d, const std::string& t) {
			return std::uint32_t(std::count(docs[d].tokens.begin(), docs[d].tokens.end(), t));
		};
		auto weight = [&](const std::string& t) {
			auto info = seg.term(t);
			return strata::weight::lucene(seg, info ? info->df : 0);
		};
		auto term = [&](const std::string& t) -> strata::scorer_ptr { return std::make_unique<strata::term_scorer>(seg, *seg.term(t)); };

		for (int round = 0; round < 300; ++round) {
			std::vector<std::string> pick;
			std::size_t n = 1 + rng() % 3;
			while (pick.size() < n) {
				std::string t = vocab[rng() % vocab.size()];
				if (std::find(pick.begin(), pick.end(), t) == pick.end() && seg.term(t)) { pick.push_back(t); }
			}
			const int kind = int(rng() % 6);
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
					if (match) {
						double idf = 0;
						for (auto& t : pick) { idf += weight(t).idf; }
						auto wt = strata::weight::lucene(seg, 1);
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
				default: {
					std::vector<strata::format::term_info> infos;
					for (auto& t : pick) { infos.push_back(*seg.term(t)); }
					auto phrase = std::make_unique<strata::phrase_scorer>(seg, infos);
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
		}
	}
	std::printf("search: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
