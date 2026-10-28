// cost() estimates checked directly (testing/parity.md, Lucene's cost tests): a term costs
// its df, a conjunction its cheapest child, a disjunction the sum of its children, the
// required-plus-optional and exclusion scorers their required side, a phrase its rarest
// term; and every cost bounds the number of matches from above, which the planners rely on.
#include "prospect/search.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace {

std::uint64_t matches(prospect::scorer& s) {
	std::uint64_t n = 0;
	for (auto d = s.next(); d != strata::no_more_docs; d = s.next()) { ++n; }
	return n;
}

}  // namespace

int main() {
	std::mt19937_64 rng(23);
	const std::vector<std::string> vocab = {"a", "b", "c", "d", "e", "rare"};
	const unsigned percent[] = {70, 40, 20, 8, 3, 1};  // documents holding each term
	for (std::size_t ndocs : {1u, 200u, 5000u, 40000u}) {
		prospect::segment_writer w;
		for (std::size_t d = 0; d < ndocs; ++d) {
			std::map<std::string, std::vector<std::uint32_t>> positions;
			std::uint32_t at = 0;
			for (std::size_t t = 0; t < vocab.size(); ++t) {
				if (rng() % 100 >= percent[t] && !(d == 0 && t == 0)) { continue; }
				const std::uint32_t n = 1 + std::uint32_t(rng() % 3);
				for (std::uint32_t k = 0; k < n; ++k) { positions[vocab[t]].push_back(at += 1 + std::uint32_t(rng() % 2)); }
			}
			std::vector<prospect::posting_input> in;
			std::uint32_t length = 0;
			for (auto& [t, p] : positions) {
				in.push_back({t, std::uint32_t(p.size()), p});
				length += std::uint32_t(p.size());
			}
			w.add(d + 1, in, length);
		}
		const std::string file = w.flush({});
		const prospect::segment seg(file);
		std::vector<std::string> present;
		for (const auto& t : vocab) {
			if (seg.get<prospect::inverted_plugin>().term(0, t)) { present.push_back(t); }
		}
		auto term = [&](const std::string& t) -> prospect::scorer_ptr { return std::make_unique<prospect::term_scorer>(seg.get<prospect::inverted_plugin>(), *seg.get<prospect::inverted_plugin>().term(0, t)); };
		auto df = [&](const std::string& t) { return seg.get<prospect::inverted_plugin>().term(0, t)->df; };

		for (const auto& t : present) {
			auto s = term(t);
			CHECK(s->cost() == df(t));
			CHECK(matches(*s) == df(t));
		}
		for (int round = 0; round < 200; ++round) {
			std::vector<std::string> pick;
			for (const auto& t : present) {
				if (rng() % 2) { pick.push_back(t); }
			}
			if (pick.size() < 2) { continue; }
			std::uint64_t lo = ~std::uint64_t(0), sum = 0;
			for (const auto& t : pick) {
				lo = std::min<std::uint64_t>(lo, df(t));
				sum += df(t);
			}
			auto children = [&] {
				std::vector<prospect::scorer_ptr> c;
				for (const auto& t : pick) { c.push_back(term(t)); }
				return c;
			};
			prospect::conjunction_scorer all(children());
			CHECK(all.cost() == lo);
			CHECK(matches(all) <= all.cost());
			prospect::disjunction_scorer any(children());
			CHECK(any.cost() == sum);
			CHECK(matches(any) <= any.cost());
			prospect::required_optional_scorer maybe(term(pick[0]), term(pick[1]));
			CHECK(maybe.cost() == df(pick[0]));
			CHECK(matches(maybe) <= maybe.cost());
			prospect::exclusion_scorer but(term(pick[0]), term(pick[1]));
			CHECK(but.cost() == df(pick[0]));
			CHECK(matches(but) <= but.cost());
			std::vector<prospect::format::term_info> infos;
			for (const auto& t : pick) { infos.push_back(*seg.get<prospect::inverted_plugin>().term(0, t)); }
			prospect::phrase_scorer phrase(seg.get<prospect::inverted_plugin>(), infos);
			CHECK(phrase.cost() == lo);
			CHECK(matches(phrase) <= phrase.cost());
		}
	}
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("cost: ok");
	return 0;
}
