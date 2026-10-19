// Offline checking (Lucene's CheckIndex): every byte against its checksum and every
// structure decoded, rather than only what queries happen to read.
#pragma once

#include "strata/reader.hh"

#include <cstdint>
#include <vector>

namespace strata {

struct check_report {
	std::uint64_t segments = 0, documents = 0, deleted = 0, terms = 0, postings = 0, positions = 0, stored_bytes = 0, keys = 0;
};

// Checks one segment: verifies every byte, decodes every term's postings (exactly its
// document frequency, by the iterator's construction) and positions, and reads every
// document's docid, version, length, and stored record. Throws corrupt_segment at the
// first problem; adds what it saw to r.
inline void check_segment(const segment& seg, const std::vector<std::uint64_t>* deleted, check_report& r, std::string_view key_prefix = {}) {
	seg.verify_all();
	++r.segments;
	r.documents += seg.core().doc_count();
	for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
		if (deleted && ((*deleted)[o >> 6] >> (o & 63)) & 1) { ++r.deleted; }
		(void)seg.core().docid(o);
		(void)seg.get<keys_plugin>().version(o);
		(void)seg.get<inverted_plugin>().length(o);
		r.stored_bytes += seg.get<stored_plugin>().stored(o).size();
	}
	const auto& inverted = seg.get<inverted_plugin>();
	inverted.scan_terms([&](std::string_view term, const format::term_info& info) {
		// Terms and keys are disjoint under a nonempty prefix (F8).
		if (!key_prefix.empty() && term.starts_with(key_prefix)) { corrupt("a term with the key prefix"); }
		++r.terms;
		postings_iterator it(inverted, info);
		positions_reader pr(inverted, info);
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
			++r.postings;
			r.positions += pr.read(it).size();
		}
		return true;
	});
	// Every key in order, its ordinal in range and its count the index's (keys.hh); each
	// ordinal named once, the range the first and last key, and every key passing the
	// range and filter that lookups consult (F13-R37); a nonzero version belongs to a key
	// (F9, F13-R9).
	const auto& keys = seg.get<keys_plugin>();
	std::vector<bool> keyed(seg.core().doc_count());
	std::string first, last;
	std::uint64_t walked = 0;  // this segment's keys
	keys.each_key([&](std::string_view key, ordinal_t o) {
		keyed[o] = true;
		++r.keys;
		if (!walked++) { first = key; }
		last = key;
		if (!keys.may_hold(key)) { corrupt("a key its range or filter misses"); }
	});
	if (keys.key_count() && (keys.min_key() != first || keys.max_key() != last)) { corrupt("key range"); }
	for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
		if (!keyed[o] && seg.get<keys_plugin>().version(o) != 0) { corrupt("a version without a key"); }
	}
}

}  // namespace strata
