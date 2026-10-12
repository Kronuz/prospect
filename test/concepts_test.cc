// Compile-time checks that the frozen Phase 1 contracts fit together.
#include "residency.hh"
#include "strata/scorer.hh"

#include <cstdio>

// A minimal scorer over a sorted_set with a constant score, as Phase 2 scorers will be shaped.
struct constant_scorer {
	docset::sorted_set::iterator it;
	double weight;
	strata::doc_t doc() const noexcept { return it.doc(); }
	strata::doc_t next() noexcept { return it.next(); }
	strata::doc_t advance(strata::doc_t t) noexcept { return it.advance(t); }
	std::uint64_t cost() const noexcept { return it.cost(); }
	double score() noexcept { return weight; }
	strata::doc_t block_end(strata::doc_t) noexcept { return strata::no_more_docs; }
	double max_score(strata::doc_t, strata::doc_t) noexcept { return weight; }
};

static_assert(strata::scorer<constant_scorer>);
static_assert(!strata::scorer<docset::sorted_set::iterator>);
static_assert(residency::backend<residency::sync_backend>);

int main() {
	docset::sorted_set s(100, {1, 5, 9});
	constant_scorer c{s.begin_iteration(), 2.5};
	double total = 0;
	for (auto d = c.next(); d != strata::no_more_docs; d = c.next()) { total += c.score(); }
	std::printf("concepts: %s\n", total == 7.5 ? "ok" : "FAILED");
	return total == 7.5 ? 0 : 1;
}
