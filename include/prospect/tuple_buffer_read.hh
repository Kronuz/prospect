// tuple_buffer's read side: the k-way merger over the runs' terms, a flush's terms with their postings, and the scan of every posting
#pragma once

#include "prospect/tuple_buffer.hh"

namespace prospect {
namespace detail {

template <class F>
void tuple_buffer::each_term(std::string_view lo, std::string_view hi, const flush_plan& p, F&& f) const {
	merger m(*this, lo, hi);
	term_postings tp, old;
	std::vector<live_posting> live;
	std::string_view term;
	while (m.next(term)) {
		if (postings_of(m.entries(), p, tp, old, live)) { f(term, tp); }
	}
}

template <class F>
void tuple_buffer::scan(F&& f) const {
	std::vector<std::uint32_t> absolute;
	term_postings tp;
	for (const run_ref& r : runs_) {
		const char* p = log_.at(r.offset);
		for (std::uint32_t e = 0; e < r.entries; ++e) {
			const entry_view v = view(p);
			tp.clear();
			decode_entry(v, tp, ~std::uint32_t(0));
			std::size_t at = 0;
			for (std::size_t i = 0; i < tp.ords.size(); ++i) {
				absolute.clear();
				std::uint32_t pos = 0;
				for (std::uint32_t c = 0; c < tp.counts[i]; ++c) { absolute.push_back(pos += tp.positions[at++]); }
				f(v.term, tp.ords[i], tp.wdfs[i], std::span<const std::uint32_t>(absolute));
			}
			p = v.end;
		}
	}
	// The batch not drained yet: each bucket grouped on a copy, the buffer left as it is.
	std::vector<tuple> copy;
	std::vector<group> groups;
	group_scratch sc;
	tuple_detail::bytes_arena terms;
	auto scan_bucket = [&](const std::vector<tuple>& bucket, std::uint32_t field) {
		copy.assign(bucket.begin(), bucket.end());
		groups.clear();
		group_run(copy.data(), copy.size(), groups, sc, field, terms);
		for (const group& g : groups) {
			for (const tuple* t = g.begin; t != g.end;) {
				const ordinal_t ord = t->ord;
				std::uint32_t wdf = 0;
				absolute.clear();
				for (; t != g.end && t->ord == ord; ++t) {
					if (t->len() == long_term) {
						const side_entry& se = side_[t->off];
						wdf += se.wdf;
						for (std::uint32_t i = 0; i < se.count; ++i) { absolute.push_back(positions_[se.positions + i]); }
					} else {
						++wdf;
						absolute.push_back(t->pos());
					}
				}
				f(g.term, ord, wdf, std::span<const std::uint32_t>(absolute));
			}
		}
	};
	for (const bucket_set& set : sets_) {
		if (set.buckets) {
			for (const auto& v : *set.buckets) { scan_bucket(v, set.field); }
		} else {
			scan_bucket(set.flat, set.field);
		}
	}
}

inline tuple_buffer::merger::merger(const tuple_buffer& b, std::string_view lo, std::string_view hi) : hi_(hi) {
	for (std::uint32_t r = 0; r < b.runs_.size(); ++r) {
		const run_ref& run = b.runs_[r];
		const char* base = b.log_.at(run.offset);
		// The last mark before lo, then entries up to lo.
		std::size_t m = 0;
		if (!lo.empty()) {
			std::size_t first = 0, count = run.marks;
			while (count > 0) {
				const std::size_t half = count / 2;
				if (view(base + b.marks_[run.first_mark + first + half]).term < lo) {
					first += half + 1, count -= half + 1;
				} else {
					count = half;
				}
			}
			m = first ? first - 1 : 0;
		}
		cursor c{base + (run.marks ? b.marks_[run.first_mark + m] : 0), std::uint32_t(run.entries - m * mark_every), {}};
		if (!c.left) { continue; }
		c.v = view(c.at);
		while (!lo.empty() && c.v.term < lo && --c.left) {
			c.at = c.v.end;
			c.v = view(c.at);
		}
		if (c.left && (hi_.empty() || c.v.term < hi_)) {
			heap_.push_back({prefix(c.v.term), std::uint32_t(cursors_.size())});
			cursors_.push_back(c);
		}
	}
	for (std::size_t i = heap_.size() / 2; i-- > 0;) { sift(i); }
}

inline bool tuple_buffer::merger::next(std::string_view& term) {
	if (heap_.empty() || (!hi_.empty() && cursors_[heap_.front().c].v.term >= hi_)) { return false; }
	entries_.clear();
	const slot top = heap_.front();
	term = cursors_[top.c].v.term;
	do {
		cursor& c = cursors_[heap_.front().c];
		entries_.push_back(c.v);
		if (--c.left) {
			c.at = c.v.end;
			c.v = view(c.at);
			heap_.front().prefix = prefix(c.v.term);
		} else {
			heap_.front() = heap_.back();
			heap_.pop_back();
			if (heap_.empty()) { break; }
		}
		sift(0);
	} while (heap_.front().prefix == top.prefix && cursors_[heap_.front().c].v.term == term);
	return true;
}

inline std::uint64_t tuple_buffer::merger::prefix(std::string_view term) noexcept {
	std::uint64_t w;
	std::memcpy(&w, term.data(), 8);
	if constexpr (std::endian::native == std::endian::little) {
		if (term.size() < 8) { w &= (std::uint64_t(1) << (8 * term.size())) - 1; }
		return __builtin_bswap64(w);
	} else {
		return term.size() < 8 ? w & ~(~std::uint64_t(0) >> (8 * term.size())) : w;
	}
}

inline bool tuple_buffer::merger::before(const slot& x, const slot& y) const noexcept {
	if (x.prefix != y.prefix) { return x.prefix < y.prefix; }
	const int c = cursors_[x.c].v.term.compare(cursors_[y.c].v.term);
	return c != 0 ? c < 0 : x.c < y.c;
}

inline void tuple_buffer::merger::sift(std::size_t i) noexcept {
	const std::size_t n = heap_.size();
	const slot s = heap_[i];
	for (;;) {
		std::size_t child = 2 * i + 1;
		if (child >= n) { break; }
		if (child + 1 < n && before(heap_[child + 1], heap_[child])) { ++child; }
		if (!before(heap_[child], s)) { break; }
		heap_[i] = heap_[child];
		i = child;
	}
	heap_[i] = s;
}

inline bool tuple_buffer::postings_of(const std::vector<entry_view>& entries, const flush_plan& p, term_postings& tp, term_postings& old, std::vector<live_posting>& live) {
	if (p.all) {
		decode(entries, tp);
		return true;
	}
	decode(entries, old);
	relabel(old, p.renumber, p.monotone, live, tp);
	return !tp.ords.empty();
}

}  // namespace detail
}  // namespace prospect
