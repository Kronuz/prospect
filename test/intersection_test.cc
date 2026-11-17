// The decoded block's all-slots compare (find_in_block) and the bitset block's per-word rank
// (bits_rank_), against a model of each term's postings: full, partial, short and one-posting
// blocks, the last block, and bitset words that are all ones, all zeros, and mixed.
#include "prospect/reader.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace {

namespace f = prospect::format;
using strata::ordinal_t;

std::uint32_t wdf_of(ordinal_t d) { return 1 + d % 7; }

// Documents 0..docs-1, each holding the terms whose model lists it.
std::string build(const std::map<std::string, std::vector<ordinal_t>>& terms, std::uint32_t docs) {
	std::vector<std::vector<prospect::posting_input>> in(docs);
	for (const auto& [t, ords] : terms) {
		for (const ordinal_t d : ords) { in[d].push_back({t, wdf_of(d), {}}); }
	}
	prospect::segment_writer w;
	for (std::uint32_t d = 0; d < docs; ++d) { w.add(1 + d, in[d], 10); }
	return w.flush({});
}

ordinal_t want(const std::vector<ordinal_t>& ps, ordinal_t t) {
	const auto it = std::lower_bound(ps.begin(), ps.end(), t);
	return it == ps.end() ? strata::no_more_docs : *it;
}

// Every target from a fresh iterator (the position inside the block, then next() and the wdf
// that depend on it); one iterator over every target in order; one iterator over the postings.
void check_term(const prospect::segment& s, const std::string& term, const std::vector<ordinal_t>& ps, std::uint32_t docs) {
	const auto& inv = s.get<prospect::inverted_plugin>();
	const auto info = *inv.term(0, term);
	CHECK(info.df == ps.size());
	for (ordinal_t t = 0; t < docs + 3; ++t) {
		prospect::postings_iterator it(inv, info);
		const ordinal_t got = it.advance(t);
		const ordinal_t w = want(ps, t);
		if (got != w) {
			std::fprintf(stderr, "%s: advance(%u) = %u, want %u\n", term.c_str(), t, got, w);
			CHECK(false);
			return;
		}
		if (w == strata::no_more_docs) { continue; }
		const auto at = std::size_t(std::lower_bound(ps.begin(), ps.end(), t) - ps.begin());
		CHECK(it.wdf() == wdf_of(w));
		CHECK(it.next() == (at + 1 < ps.size() ? ps[at + 1] : strata::no_more_docs));
		if (at + 1 < ps.size()) { CHECK(it.wdf() == wdf_of(ps[at + 1])); }
	}
	prospect::postings_iterator seq(inv, info);
	for (ordinal_t t = 0; t < docs + 3; ++t) {
		const ordinal_t got = seq.advance(t);
		CHECK(got == want(ps, t));
		if (got != strata::no_more_docs) { CHECK(seq.wdf() == wdf_of(got)); }
	}
	prospect::postings_iterator by(inv, info);
	for (const ordinal_t d : ps) { CHECK(by.advance(d) == d && by.wdf() == wdf_of(d)); }
	CHECK(by.advance(docs + 1) == strata::no_more_docs);
}

}  // namespace

int main() {
	std::map<std::string, std::vector<ordinal_t>> terms;
	auto every = [&](const char* name, ordinal_t step, std::size_t n, ordinal_t from = 0) {
		for (std::size_t i = 0; i < n; ++i) { terms[name].push_back(from + ordinal_t(i) * step); }
	};
	every("full", 3, 2 * f::block);         // two full decoded blocks, no tail
	every("part", 3, 2 * f::block + 44);    // two full blocks and a 44-posting tail
	every("short", 11, 5);                  // only a tail of 5
	every("edge", 2, f::block - 1);         // a tail one short of a block
	every("exact", 2, f::block);            // exactly one full block
	every("plus1", 2, f::block + 1);        // a full block and a one-posting tail
	every("one", 1, 1, 777);                // a single posting
	// Bitset blocks: word 0 all ones, word 1 all zeros, words 2 and 3 half set at random;
	// a block holds 128 postings over 256 ordinals, so every word boundary is exercised.
	std::mt19937_64 rng(3);
	for (std::uint32_t b = 0; b < 3; ++b) {
		auto& v = terms["bits"];
		const ordinal_t base = b * 256;
		for (ordinal_t i = 0; i < 64; ++i) { v.push_back(base + i); }
		std::vector<ordinal_t> mix;
		for (ordinal_t i = 128; i < 256; ++i) { mix.push_back(base + i); }
		std::shuffle(mix.begin(), mix.end(), rng);
		mix.resize(64);
		mix.back() = base + 255;  // the block's last bit is set
		std::sort(mix.begin(), mix.end());
		mix.erase(std::unique(mix.begin(), mix.end()), mix.end());
		for (const ordinal_t m : mix) { v.push_back(m); }
	}
	// Bitset blocks back to back and a partial tail of them, all words set in the first.
	for (ordinal_t i = 0; i < 128; ++i) { terms["ones"].push_back(i); }
	for (ordinal_t i = 0; i < 128 + 20; i += 1 + (i % 4 == 0)) { terms["dense"].push_back(1000 + i); }
	for (auto& [_, v] : terms) {
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
	}
	const std::uint32_t docs = 1100;
	const std::string bytes = build(terms, docs);
	prospect::segment s(bytes);
	for (const auto& [term, ps] : terms) { check_term(s, term, ps, docs); }
	if (failures) { return 1; }
	std::puts("intersection_test: ok");
	return 0;
}
