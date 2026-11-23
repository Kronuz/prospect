// tuple_buffer's run building: a batch's buckets grouped by term, sorted, and drained into a run, with its scratch memory
#pragma once

#include "prospect/tuple_buffer.hh"

namespace prospect {
namespace detail {

inline std::string_view tuple_buffer::bytes_of(const tuple& t) const noexcept {
	if (t.len() == long_term) { return std::string_view(side_[t.off].bytes, side_[t.off].size); }
	return std::string_view(texts_[t.ord - batch_first_] + t.off, t.len());
}

inline void tuple_buffer::charge_scratch() {
	if (!memory) { return; }
	const std::uint64_t now = groups_.capacity() * sizeof(group) + scratch_.hashes.capacity() * 8 +
		(scratch_.ids.capacity() + scratch_.tids.capacity() + scratch_.counts.capacity() + order_.capacity()) * 4 +
		scratch_.sorted.capacity() * sizeof(tuple) + (keys_scratch_.capacity() + radix_scratch_.capacity()) * sizeof(chunk_key) +
		sort_todo_.capacity() * sizeof(key_run) + entry_at_.capacity() * sizeof(entry_ref);
	if (now > scratch_charged_) {
		memory->grow(now - scratch_charged_);
		scratch_charged_ = now;
	}
}

inline void tuple_buffer::drain() {
	assert(max_run_bytes <= run_cap);
	if (batched_) {
		groups_.clear();
		encoded_.size = 0;
		entry_at_.clear();
		tuple_detail::bytes_arena terms;  // the groups' composite terms, until the entries are encoded
		auto group_bucket = [&](std::vector<tuple>& v, std::uint32_t field) {
			const std::size_t first = groups_.size();
			group_run(v.data(), v.size(), groups_, scratch_, field, terms);
			for (std::size_t i = first; i < groups_.size(); ++i) { encode_entry(std::uint32_t(i)); }  // its tuples still in cache
		};
		for (bucket_set& set : sets_) {
			if (set.buckets) {
				for (auto& v : *set.buckets) { group_bucket(v, set.field); }
			} else {
				group_bucket(set.flat, set.field);
			}
		}
		sort_groups(groups_);
		// The encoded entries into the run in term order, marks placed as they land.
		// Runs of at most max_run_bytes, cut between entries.
		for (std::size_t k = 0, n = order_.size(); k < n;) {
			std::size_t end = n;
			std::uint64_t bytes = encoded_.size;
			if (bytes > max_run_bytes) {
				end = k, bytes = 0;
				while (end < n && bytes + entry_at_[order_[end]].size <= max_run_bytes) { bytes += entry_at_[order_[end++]].size; }
				if (end == k) { bytes = entry_at_[order_[end++]].size; }  // past the cap: a run alone, its one mark 0
			}
			const std::size_t first_mark = marks_.size();
			const std::uint64_t at = log_.append(bytes, memory);
			char* const run = log_.at(at);
			std::size_t offset = 0;
			for (std::size_t j = k; j < end; ++j) {
				const entry_ref& e = entry_at_[order_[j]];
				if ((j - k) % mark_every == 0) { marks_.push_back(std::uint32_t(offset)); }
				std::memcpy(run + offset, encoded_.data.get() + e.offset, e.size);
				offset += e.size;
			}
			runs_.push_back({at, std::uint32_t(end - k), first_mark, marks_.size() - first_mark});
			k = end;
		}
		for (bucket_set& set : sets_) {
			if (set.buckets) {
				for (auto& b : *set.buckets) { b.clear(); }
			}
			set.flat.clear();
		}
	}
	batched_ = 0;
	entry_term_ = 0, entry_positions_ = 0;
	side_.clear();
	positions_.clear();
	arena_at_ = 0, arena_fill_ = 0;
	texts_.clear();
	batch_first_ = ordinal_t(docids_.size());
	charge_columns();
	charge_scratch();
}

inline void tuple_buffer::group_run(tuple* p, std::size_t n, std::vector<group>& groups, group_scratch& sc, std::uint32_t field, tuple_detail::bytes_arena& terms) const {
	if (!n) { return; }
	std::size_t cap = std::bit_ceil(std::max<std::size_t>(64, n / 2));
	auto& hashes = sc.hashes;
	auto& ids = sc.ids;
	hashes.assign(cap, 0);
	ids.resize(cap);
	sc.tids.resize(n);
	std::uint32_t distinct = 0;
	auto find = [&](std::uint64_t h) -> std::uint32_t {
		for (std::size_t k = (h >> 8) & (cap - 1);; k = (k + 1) & (cap - 1)) {
			if (hashes[k] == h) { return ids[k]; }
			if (hashes[k] == 0) {
				hashes[k] = h;
				ids[k] = distinct;
				return distinct++;
			}
		}
	};
	for (std::size_t i = 0; i < n; ++i) {
		std::uint64_t h = p[i].hash();
		h += h == 0;  // 0 marks an empty slot
		sc.tids[i] = find(h);
		if (2 * std::size_t(distinct) > cap) {  // keep the load at most a half
			std::vector<std::uint64_t> old_hashes(std::move(hashes));
			std::vector<std::uint32_t> old_ids(std::move(ids));
			cap *= 2;
			hashes.assign(cap, 0);
			ids.resize(cap);
			for (std::size_t k = 0; k < old_hashes.size(); ++k) {
				if (!old_hashes[k]) { continue; }
				std::size_t j = (old_hashes[k] >> 8) & (cap - 1);
				while (hashes[j]) { j = (j + 1) & (cap - 1); }
				hashes[j] = old_hashes[k];
				ids[j] = old_ids[k];
			}
		}
	}
	auto& counts = sc.counts;  // then each id's end in sorted
	counts.assign(distinct + 1, 0);
	for (std::size_t i = 0; i < n; ++i) { ++counts[sc.tids[i] + 1]; }
	for (std::uint32_t t = 0; t < distinct; ++t) { counts[t + 1] += counts[t]; }
	sc.sorted.resize(n);
	for (std::size_t i = 0; i < n; ++i) { sc.sorted[counts[sc.tids[i]]++] = p[i]; }
	std::memcpy(p, sc.sorted.data(), n * sizeof(tuple));
	// Tokens of at most 8 bytes with equal hashes and lengths are equal (hash's word
	// form is a bijection at each length); the rest compare bytes.
	const bool exact = hash_mask == ~std::uint64_t(0);
	std::size_t begin = 0;
	for (std::uint32_t t = 0; t < distinct; ++t) {
		const std::size_t end = counts[t];
		const tuple& f = p[begin];
		const std::string_view first = bytes_of(f);
		bool mixed = false;
		if (exact && f.len() <= 8) {
			for (std::size_t i = begin + 1; i < end && !mixed; ++i) { mixed = p[i].len() != f.len() || p[i].hash() != f.hash(); }
		} else {
			for (std::size_t i = begin + 1; i < end && !mixed; ++i) { mixed = bytes_of(p[i]) != first; }
		}
		if (mixed) {
			split(p + begin, p + end, groups, field, terms);
		} else {
			groups.push_back({composite(field, first, terms), p + begin, p + end});
		}
		begin = end;
	}
}

inline std::string_view tuple_buffer::composite(std::uint32_t field, std::string_view bytes, tuple_detail::bytes_arena& terms) {
	char head[5];
	const std::size_t n = fielddict::put_field(head, field);
	char* at = terms.take(n + bytes.size());
	std::memcpy(at, head, n);
	if (!bytes.empty()) { std::memcpy(at + n, bytes.data(), bytes.size()); }
	return {at, n + bytes.size()};
}

inline void tuple_buffer::split(tuple* begin, tuple* end, std::vector<group>& groups, std::uint32_t field, tuple_detail::bytes_arena& terms) const {
	std::stable_sort(begin, end, [&](const tuple& x, const tuple& y) { return bytes_of(x) < bytes_of(y); });
	for (tuple* g = begin; g != end;) {
		tuple* e = g + 1;
		while (e != end && bytes_of(*e) == bytes_of(*g)) { ++e; }
		groups.push_back({composite(field, bytes_of(*g), terms), g, e});
		g = e;
	}
}

inline auto tuple_buffer::key_at(std::string_view term, std::size_t depth, std::uint32_t index) noexcept -> chunk_key {
	std::uint64_t c = 0;
	const std::size_t n = std::min<std::size_t>(8, term.size() - depth);
	for (std::size_t i = 0; i < n; ++i) { c |= std::uint64_t(std::uint8_t(term[depth + i])) << (56 - 8 * i); }
	return {c, std::uint32_t(std::min<std::size_t>(term.size() - depth, 9)), index};
}

inline void tuple_buffer::sort_groups(const std::vector<group>& groups) {
	const std::size_t n = groups.size();
	keys_scratch_.resize(n);
	for (std::size_t i = 0; i < n; ++i) { keys_scratch_[i] = key_at(groups[i].term, 0, std::uint32_t(i)); }
	if (n < radix_from) {
		sort_run(groups, keys_scratch_.data(), keys_scratch_.data() + n, 0);
	} else {
		radix_sort(keys_scratch_, radix_scratch_);
		chunk_key* const e = keys_scratch_.data() + n;
		for (chunk_key* r = keys_scratch_.data(); r != e;) {
			chunk_key* t = r + 1;
			while (t != e && t->chunk == r->chunk && t->left == r->left) { ++t; }
			if (t - r > 1 && r->left == 9) {  // tied on the first chunk, and every one continues past it
				for (chunk_key* k = r; k != t; ++k) { *k = key_at(groups[k->index].term, 8, k->index); }
				sort_run(groups, r, t, 8);
			}
			r = t;
		}
	}
	order_.resize(n);
	for (std::size_t i = 0; i < n; ++i) { order_[i] = keys_scratch_[i].index; }
}

inline void tuple_buffer::radix_sort(std::vector<chunk_key>& keys, std::vector<chunk_key>& scratch) {
	const std::size_t n = keys.size();
	scratch.resize(n);
	std::array<std::array<std::uint32_t, 256>, 9> counts{};
	for (const chunk_key& k : keys) {
		++counts[0][k.left];
		for (unsigned b = 0; b < 8; ++b) { ++counts[b + 1][(k.chunk >> (8 * b)) & 255]; }
	}
	auto digit = [](const chunk_key& k, unsigned d) noexcept { return d == 0 ? unsigned(k.left) : unsigned(k.chunk >> (8 * (d - 1))) & 255; };
	chunk_key* from = keys.data();
	chunk_key* to = scratch.data();
	for (unsigned d = 0; d < 9; ++d) {
		std::array<std::uint32_t, 256>& c = counts[d];
		if (c[digit(from[0], d)] == n) { continue; }
		std::uint32_t sum = 0;
		for (std::uint32_t& x : c) { sum += std::exchange(x, sum); }
		for (std::size_t i = 0; i < n; ++i) { to[c[digit(from[i], d)]++] = from[i]; }
		std::swap(from, to);
	}
	if (from != keys.data()) { keys.swap(scratch); }
}

inline void tuple_buffer::sort_run(const std::vector<group>& groups, chunk_key* b, chunk_key* e, std::size_t depth) {
	std::vector<key_run>& todo = sort_todo_;
	todo.push_back({b, e, depth});
	while (!todo.empty()) {
		const key_run k = todo.back();
		todo.pop_back();
		std::sort(k.b, k.e);
		for (chunk_key* r = k.b; r != k.e;) {
			chunk_key* t = r + 1;
			while (t != k.e && t->chunk == r->chunk && t->left == r->left) { ++t; }
			if (t - r > 1 && r->left == 9) {  // tied on this chunk, and every one continues past it
				for (chunk_key* x = r; x != t; ++x) { *x = key_at(groups[x->index].term, k.depth + 8, x->index); }
				todo.push_back({r, t, k.depth + 8});
			}
			r = t;
		}
	}
}

}  // namespace detail
}  // namespace prospect
