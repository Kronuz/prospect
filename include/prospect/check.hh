// The search layout's check report: the core's counts, and the terms, postings, positions,
// stored bytes, keys, and listed terms its plugins count; and check_segment, which also
// cross-checks the term lists against the postings.
#pragma once

#include "prospect/reader.hh"
#include "prospect/strata.hh"
#include "prospect/term_lists.hh"
#include "strata/core/check.hh"

#include <cstdint>
#include <string_view>
#include <vector>

namespace prospect {

using check_report = basic_check_report<search_layout>;

namespace check_detail {
inline std::uint64_t mix(std::uint64_t ord, std::string_view term, std::uint32_t wdf) noexcept {
	std::uint64_t h = 0xcbf29ce484222325ull;
	for (const char c : term) { h = (h ^ std::uint8_t(c)) * 0x100000001b3ull; }
	h ^= (ord << 32 | wdf) * 0x9E3779B97F4A7C15ull;
	h ^= h >> 30, h *= 0xBF58476D1CE4E5B9ull;
	h ^= h >> 27, h *= 0x94D049BB133111EBull;
	return h ^ (h >> 31);
}
}  // namespace check_detail

// The term lists hold exactly the postings' (ordinal, term, wdf) triples: their counts and an
// order-independent sum of a hash of each triple agree. Throws corrupt_segment otherwise.
inline void cross_check_term_lists(const segment& seg) {
	if (!seg.holds<term_lists_plugin>()) { return; }
	const auto& lists = seg.get<term_lists_plugin>();
	const auto& inverted = seg.get<inverted_plugin>();
	std::uint64_t listed = 0, listed_sum = 0, posted = 0, posted_sum = 0;
	term_lists_plugin::reader::frame frame;
	for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
		for (auto c = lists.list(o, frame).begin(); c.next();) {
			++listed;
			listed_sum += check_detail::mix(o, c.term(), c.wdf());
		}
	}
	inverted.scan_terms([&](std::string_view term, const format::term_info& info) {
		postings_iterator it(inverted, info);
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
			++posted;
			posted_sum += check_detail::mix(o, term, it.wdf());
		}
		return true;
	});
	if (listed != posted || listed_sum != posted_sum) { corrupt("term lists disagree with the postings"); }
}

// strata::check_segment, then the cross-check.
inline void check_segment(const segment& seg, const std::vector<std::uint64_t>* deleted, check_report& r, std::string_view key_prefix = {}) {
	strata::check_segment(seg, deleted, r, key_prefix);
	cross_check_term_lists(seg);
}

}  // namespace prospect
