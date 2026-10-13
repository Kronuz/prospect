// Offline checking (Lucene's CheckIndex): every byte against its checksum and every
// structure decoded, rather than only what queries happen to read.
#pragma once

#include "strata/reader.hh"

#include <cstdint>
#include <vector>

namespace strata {

struct check_report {
	std::uint64_t segments = 0, documents = 0, deleted = 0, terms = 0, postings = 0, positions = 0, stored_bytes = 0;
};

// Checks one segment: verifies every byte, decodes every term's postings (exactly its
// document frequency, by the iterator's construction) and positions, and reads every
// document's docid, version, length, and stored record. Throws corrupt_segment (or
// termdict::corrupt) at the first problem; adds what it saw to r.
inline void check_segment(const segment& seg, const std::vector<std::uint64_t>* deleted, check_report& r) {
	seg.verify_all();
	++r.segments;
	r.documents += seg.doc_count();
	for (ordinal_t o = 0; o < seg.doc_count(); ++o) {
		if (deleted && ((*deleted)[o >> 6] >> (o & 63)) & 1) { ++r.deleted; }
		(void)seg.docid(o);
		(void)seg.version(o);
		(void)seg.length(o);
		r.stored_bytes += seg.stored(o).size();
	}
	seg.scan_terms([&](std::string_view, const format::term_info& info) {
		++r.terms;
		postings_iterator it(seg, info);
		positions_reader pr(seg, info);
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
			++r.postings;
			r.positions += pr.read(it).size();
		}
		return true;
	});
}

}  // namespace strata
