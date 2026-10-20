// Compile-time checks that the frozen Phase 1 contracts fit together.
#include "residency.hh"
#include "strata/search/reader.hh"
#include "strata/search/scorer.hh"

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

// The layers' plugins (layers.md L4) and format 13's document profile (L12) against a stub
// keys plugin that depends on nothing, until format 13's keys plugin exists.
static_assert(strata::section_plugin<strata::inverted_plugin> && strata::section_plugin<strata::keys_plugin> && strata::section_plugin<strata::stored_plugin>);
struct stub_keys {
	static constexpr std::string_view id = "keys";
	static constexpr strata::format::owner owner = strata::format::owner::keys;
	static constexpr std::uint64_t structure = strata::format::bit::keys;
	static constexpr std::uint64_t structures = strata::format::bit::keys;
	struct reader {
		explicit reader(const strata::open_context&) {}
		void verify_all() const {}
	};
	static strata::built merge(const strata::merge_context&, std::span<const reader* const>, strata::section_sink&);
};
static_assert(strata::section_plugin<stub_keys>);
// A plugin's own bit must be one of its bits, and exactly one bit.
struct stub_two_bits : stub_keys {
	static constexpr std::uint64_t structure = strata::format::bit::keys | strata::format::bit::stored;
};
static_assert(!strata::section_plugin<stub_two_bits>);
using document_layout = strata::layout<stub_keys, strata::stored_plugin>;
template struct strata::basic_document<document_layout>;
template class strata::basic_segment<document_layout>;
static_assert(sizeof(strata::basic_document<document_layout>) == sizeof(strata::core_part) + sizeof(strata::stored_plugin::part));

int main() {
	docset::sorted_set s(100, {1, 5, 9});
	constant_scorer c{s.begin_iteration(), 2.5};
	double total = 0;
	for (auto d = c.next(); d != strata::no_more_docs; d = c.next()) { total += c.score(); }
	std::printf("concepts: %s\n", total == 7.5 ? "ok" : "FAILED");
	return total == 7.5 ? 0 : 1;
}
