// tuple_buffer's run entry codec: a term's postings encoded as an entry, decoded, and relabeled for a flush's numbering
#pragma once

#include "prospect/tuple_buffer.hh"

namespace prospect {
namespace detail {

inline auto tuple_buffer::view(const char* p) noexcept -> entry_view {
	entry_view v;
	const std::uint32_t size = std::uint32_t(tuple_detail::get_leb(p));
	v.term = std::string_view(p, size);
	p += size;
	v.postings = std::uint32_t(tuple_detail::get_leb(p));
	v.positions = std::uint32_t(tuple_detail::get_leb(p));
	v.cf = tuple_detail::get_leb(p);
	v.flags = std::uint8_t(*p++);
	for (auto& l : v.lengths) { l = tuple_detail::get_leb(p); }
	v.data = p;
	v.end = v.data + std::size_t(v.lengths[0]) + v.lengths[1] + v.lengths[2] + v.lengths[3];
	return v;
}

inline void tuple_buffer::encode_entry(std::uint32_t i) {
	const group& g = groups_[i];
	std::size_t tuples = 0, side_positions = 0;
	for (const tuple* p = g.begin; p != g.end; ++p) {
		++tuples;
		if (p->len() == long_term) { side_positions += side_[p->off].count; }
	}
	char* ords = sec_[0].room(5 * tuples, memory);
	char* wdfs = sec_[1].room(5 * tuples, memory);
	char* counts = sec_[2].room(5 * tuples, memory);
	char* positions = sec_[3].room(5 * (tuples + side_positions), memory);
	char* const starts[4] = {ords, wdfs, counts, positions};
	std::uint32_t postings = 0, npositions = 0, flags = 1 | 4;
	std::uint64_t cf = 0;
	bool counted = false;
	ordinal_t last = ~ordinal_t(0);
	for (const tuple* p = g.begin; p != g.end;) {
		const ordinal_t ord = p->ord;
		std::uint32_t wdf = 0, count = 0, prev = 0;
		for (; p != g.end && p->ord == ord; ++p) {
			if (p->len() == long_term) {
				const side_entry& se = side_[p->off];
				wdf += se.wdf, count += se.count;
				for (std::uint32_t k = 0; k < se.count; ++k) {
					const std::uint32_t pos = positions_[se.positions + k];
					tuple_detail::put_leb(positions, pos - prev);
					prev = pos;
				}
			} else {
				++wdf, ++count;
				tuple_detail::put_leb(positions, p->pos() - prev);
				prev = p->pos();
			}
		}
		tuple_detail::put_leb(ords, ord - last);  // from -1: the first is the ordinal + 1
		tuple_detail::put_leb(wdfs, wdf);
		tuple_detail::put_leb(counts, count);
		last = ord;
		counted = counted || count != wdf;
		++postings;
		npositions += count;
		cf += wdf;
		if (wdf) { flags &= ~1u; }
		if (count) { flags |= 2; }
		if (count != wdf) { flags &= ~4u; }
	}
	const std::uint64_t lengths[4] = {std::uint64_t(ords - starts[0]), std::uint64_t(wdfs - starts[1]), counted ? std::uint64_t(counts - starts[2]) : 0u,
		std::uint64_t(positions - starts[3])};
	char* const begin = encoded_.room(66 + g.term.size() + lengths[0] + lengths[1] + lengths[2] + lengths[3], memory);
	char* e = begin;
	tuple_detail::put_leb(e, g.term.size());
	std::memcpy(e, g.term.data(), g.term.size());
	e += g.term.size();
	for (std::uint64_t v : {std::uint64_t(postings), std::uint64_t(npositions), cf}) { tuple_detail::put_leb(e, v); }
	*e++ = char(flags);
	for (std::uint64_t l : lengths) { tuple_detail::put_leb(e, l); }
	for (int k = 0; k < 4; ++k) {
		std::memcpy(e, starts[k], lengths[k]);
		e += lengths[k];
	}
	entry_at_.push_back({encoded_.size, std::uint64_t(e - begin)});
	encoded_.size += std::size_t(e - begin);
}

inline void tuple_buffer::decode_entry(const entry_view& v, term_postings& tp, std::uint32_t ord) {
	const char* p = v.data;
	const char* wdfs = p + v.lengths[0];
	const char* counts = wdfs + v.lengths[1];
	const char* positions = counts + v.lengths[2];
	const char* wdfs_again = wdfs;  // the counts when the entry has none apart
	for (std::uint32_t i = 0; i < v.postings; ++i) {
		ord += std::uint32_t(tuple_detail::get_leb(p));
		tp.ords.push_back(ord);
		tp.wdfs.push_back(std::uint32_t(tuple_detail::get_leb(wdfs)));
		const std::uint32_t count = std::uint32_t(tuple_detail::get_leb(v.lengths[2] ? counts : wdfs_again));
		tp.counts.push_back(count);
		for (std::uint32_t k = 0; k < count; ++k) { tp.positions.push_back(std::uint32_t(tuple_detail::get_leb(positions))); }
	}
}

inline void tuple_buffer::decode(const std::vector<entry_view>& entries, term_postings& tp) {
	tp.clear();
	std::size_t postings = 0, positions = 0;
	for (const auto& v : entries) { postings += v.postings, positions += v.positions; }
	tp.ords.reserve(postings), tp.wdfs.reserve(postings), tp.counts.reserve(postings), tp.positions.reserve(positions);
	tp.all_zero_wdf = true, tp.any_positions = false, tp.counts_are_wdf = true;
	for (const auto& v : entries) {
		decode_entry(v, tp, ~std::uint32_t(0));
		tp.cf += v.cf;
		if (!(v.flags & 1)) { tp.all_zero_wdf = false; }
		if (v.flags & 2) { tp.any_positions = true; }
		if (!(v.flags & 4)) { tp.counts_are_wdf = false; }
	}
}

inline void tuple_buffer::relabel(const term_postings& old, const std::vector<ordinal_t>& renumber, bool monotone, std::vector<live_posting>& live, term_postings& tp) {
	live.clear();
	std::uint32_t first = 0;
	for (std::size_t i = 0; i < old.ords.size(); ++i) {
		if (renumber[old.ords[i]] != no_more_docs) { live.push_back({renumber[old.ords[i]], old.wdfs[i], old.counts[i], first}); }
		first += old.counts[i];
	}
	if (!monotone) { std::sort(live.begin(), live.end()); }
	tp.clear();
	for (const auto& l : live) {
		tp.ords.push_back(l.ord);
		tp.wdfs.push_back(l.wdf);
		tp.counts.push_back(l.count);
		tp.positions.insert(tp.positions.end(), old.positions.begin() + l.first, old.positions.begin() + l.first + l.count);
		tp.cf += l.wdf;
		if (l.wdf) { tp.all_zero_wdf = false; }
		if (l.count) { tp.any_positions = true; }
		if (l.count != l.wdf) { tp.counts_are_wdf = false; }
	}
}

}  // namespace detail
}  // namespace prospect
