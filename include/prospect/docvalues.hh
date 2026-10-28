/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Doc values (Phase 4, D4): one slot's column in one segment. Presence is a frozen docset,
// omitted when every ordinal has a value; a present ordinal's value index is its rank.
// Values pack in blocks of 128 from the block's minimum, divided by the GCD of the
// deltas. Skip groups (R10) hold consecutive documents that have a value, whole.
//
// Column, little-endian fixed fields and LEB128 counts:
//   u8 version, u8 kind, u8 flags (1 multi-valued, 2 presence stored),
//   leb ordinals, leb documents with values, leb values, u64 min, u64 max,
//   integer kinds: the exact sum as 128-bit two's complement (u64 low, u64 high),
//   presence: leb size, frozen docset,
//   bytes: the dictionary (leb terms, terms + 1 u64 offsets, the sorted terms' bytes),
//   leb documents per group, then per group u32 first ordinal, u32 last ordinal,
//     u64 first value index, u64 value count, u64 min, u64 max,
//   multi-valued: the offsets (present documents + 1 cumulative counts) as packed values,
//   the values as packed values.
// Packed values: leb count, per block u64 min, u64 max, u64 gcd, u64 offset, u8 bits;
// leb packed size, then every block's bitpack form, (value - min) / gcd at `bits`.

#pragma once

#include "bitpack.hh"
#include "docset.hh"
#include "prospect/format.hh"
#include "prospect/ordered.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prospect::docvalues {

// Logical types; `date` encodes as f64 seconds (D3a).
enum class kind : std::uint8_t { u64 = 0, i64 = 1, f64 = 2, bytes = 3 };

inline constexpr std::size_t block_values = ::bitpack::block_size;

struct options {
	std::uint32_t group_docs = 4096;  // documents with a value per skip group: a preset, recorded per column
};

struct statistics {
	std::uint64_t docs = 0;    // ordinals with at least one value, deleted ones included
	std::uint64_t values = 0;
	std::uint64_t min = 0, max = 0;  // mapped keys (dictionary ordinals for bytes); 0 when empty
	// The exact sum of the unmapped values, for integer kinds. f64 stores none yet: the
	// width of D11's persisted exact accumulator is a format field decided later.
	bool has_sum = false;
	__int128 sum = 0;
};

struct block_info { std::uint64_t min = 0, max = 0, gcd = 1; unsigned bits = 0; };

// A skip group: its documents' ordinal span and their values, [first_value, first_value + count).
struct skip_entry {
	ordinal_t first = 0, last = 0;
	std::uint64_t first_value = 0, count = 0, min = 0, max = 0;
};

// Value j of a bitpack block at width 0 to 64, reading only its own bits. Value j lies in
// lane j % 4, row j / 4; a 64-bit width packs the low halves at min(bits, 32), then the
// high halves at bits - 32 after packed_bytes(32).
inline std::uint64_t extract(const std::uint8_t* block, unsigned bits, std::size_t j) noexcept {
	auto lane_value = [j](const std::uint8_t* in, unsigned b) -> std::uint64_t {
		if (b == 0) { return 0; }
		const unsigned lane = unsigned(j & 3), start = unsigned(j >> 2) * b, s = start % 32;
		const std::uint8_t* word = in + 4 * (4 * (start / 32) + lane);
		std::uint32_t low, high;
		std::memcpy(&low, word, 4);
		std::uint64_t v = low >> s;
		if (s + b > 32) { std::memcpy(&high, word + 16, 4); v |= std::uint64_t(high) << (32 - s); }
		return v & ((std::uint64_t(1) << b) - 1);
	};
	if (bits <= 32) { return lane_value(block, bits); }
	return lane_value(block, 32) | lane_value(block + ::bitpack::packed_bytes(32), bits - 32) << 32;
}

namespace detail {

inline constexpr std::uint8_t version = 1;
inline constexpr std::uint8_t flag_multi = 1, flag_presence = 2;
inline constexpr std::size_t block_entry = 33, group_entry = 40;
inline constexpr std::uint64_t max_group_docs = 1ull << 32;

constexpr bool has_sum(kind k) noexcept { return k == kind::u64 || k == kind::i64; }

inline void put_values(std::string& out, const std::vector<std::uint64_t>& v) {
	const std::size_t n = v.size(), blocks = (n + block_values - 1) / block_values;
	format::put_leb(out, n);
	std::string directory, packed;
	std::uint64_t q[block_values];
	std::uint8_t buffer[::bitpack::packed_bytes(64)];
	for (std::size_t b = 0; b < blocks; ++b) {
		const std::size_t first = b * block_values, last = std::min(n, first + block_values);
		std::uint64_t lo = v[first], hi = v[first];
		for (std::size_t i = first; i < last; ++i) { lo = std::min(lo, v[i]); hi = std::max(hi, v[i]); }
		std::uint64_t gcd = 0;
		for (std::size_t i = first; i < last && gcd != 1; ++i) { gcd = std::gcd(gcd, v[i] - lo); }
		if (gcd == 0) { gcd = 1; }
		for (std::size_t i = 0; i < block_values; ++i) { q[i] = first + i < last ? (v[first + i] - lo) / gcd : 0; }
		const unsigned bits = ::bitpack::max_bits(q);
		::bitpack::pack(q, bits, buffer);
		format::put<std::uint64_t>(directory, lo);
		format::put<std::uint64_t>(directory, hi);
		format::put<std::uint64_t>(directory, gcd);
		format::put<std::uint64_t>(directory, packed.size());
		format::put<std::uint8_t>(directory, std::uint8_t(bits));
		packed.append(reinterpret_cast<const char*>(buffer), ::bitpack::packed_bytes(bits));
	}
	out += directory;
	format::put_leb(out, packed.size());
	out += packed;
}

// The column of `present` (ascending ordinals out of `doc_count`): `starts` holds present + 1
// cumulative counts into `flat`, each present ordinal's values sorted. A dictionary is a bytes
// column's, already encoded. No work grows with the ordinals absent.
inline std::string encode_present(kind k, bool multi, std::uint64_t doc_count, const std::vector<::docset::doc_t>& present, const std::vector<std::uint64_t>& starts,
	const std::vector<std::uint64_t>& flat, const std::string* dictionary, const options& opts) {
	if (opts.group_docs == 0) { throw std::invalid_argument("strata: doc values: the group size must be positive"); }
	if (doc_count > std::numeric_limits<ordinal_t>::max()) { throw std::invalid_argument("strata: doc values: too many ordinals"); }
	std::vector<std::uint64_t> offsets{0};
	std::string groups;
	skip_entry group;
	auto put_group = [&] {
		format::put<std::uint32_t>(groups, group.first);
		format::put<std::uint32_t>(groups, group.last);
		format::put<std::uint64_t>(groups, group.first_value);
		format::put<std::uint64_t>(groups, group.count);
		format::put<std::uint64_t>(groups, group.min);
		format::put<std::uint64_t>(groups, group.max);
	};
	for (std::size_t i = 0; i < present.size(); ++i) {
		const std::uint64_t n = starts[i + 1] - starts[i];
		if (!n) { throw std::invalid_argument("strata: doc values: a present ordinal holds a value"); }
		if (!multi && n > 1) { throw std::invalid_argument("strata: doc values: a single-valued slot holds one value per document"); }
		const auto lo = *std::min_element(flat.begin() + std::ptrdiff_t(starts[i]), flat.begin() + std::ptrdiff_t(starts[i + 1]));
		const auto hi = *std::max_element(flat.begin() + std::ptrdiff_t(starts[i]), flat.begin() + std::ptrdiff_t(starts[i + 1]));
		if (i % opts.group_docs == 0) { group = {ordinal_t(present[i]), ordinal_t(present[i]), offsets.back(), 0, lo, hi}; }
		group.last = ordinal_t(present[i]);
		group.count += n;
		group.min = std::min(group.min, lo);
		group.max = std::max(group.max, hi);
		offsets.push_back(offsets.back() + n);
		if ((i + 1) % opts.group_docs == 0) { put_group(); }
	}
	if (present.size() % opts.group_docs) { put_group(); }
	statistics s;
	s.docs = present.size();
	s.values = flat.size();
	if (!flat.empty()) { s.min = *std::min_element(flat.begin(), flat.end()); s.max = *std::max_element(flat.begin(), flat.end()); }
	for (const auto v : flat) {
		if (k == kind::u64) { s.sum += static_cast<__int128>(v); } else if (k == kind::i64) { s.sum += ordered::to_i64(v); }
	}
	const bool sparse = present.size() != doc_count;
	std::string out;
	format::put<std::uint8_t>(out, version);
	format::put<std::uint8_t>(out, std::uint8_t(k));
	format::put<std::uint8_t>(out, std::uint8_t((multi ? flag_multi : 0) | (sparse ? flag_presence : 0)));
	format::put_leb(out, doc_count);
	format::put_leb(out, s.docs);
	format::put_leb(out, s.values);
	format::put<std::uint64_t>(out, s.min);
	format::put<std::uint64_t>(out, s.max);
	if (has_sum(k)) {
		const auto u = static_cast<unsigned __int128>(s.sum);
		format::put<std::uint64_t>(out, std::uint64_t(u));
		format::put<std::uint64_t>(out, std::uint64_t(u >> 64));
	}
	if (sparse) {
		const auto frozen = ::docset::freeze(present.data(), present.size());
		format::put_leb(out, frozen.size());
		out.append(reinterpret_cast<const char*>(frozen.data()), frozen.size());
	}
	if (dictionary) { out += *dictionary; }
	format::put_leb(out, opts.group_docs);
	out += groups;
	if (multi) { put_values(out, offsets); }
	put_values(out, flat);
	return out;
}

// `starts` holds ordinals + 1 cumulative counts into `flat`; each ordinal's values sorted.
inline std::string encode_column(kind k, bool multi, const std::vector<std::uint64_t>& starts, const std::vector<std::uint64_t>& flat, const std::string* dictionary, const options& opts) {
	const std::size_t doc_count = starts.size() - 1;
	std::vector<::docset::doc_t> present;
	std::vector<std::uint64_t> compact{0};
	for (std::size_t o = 0; o < doc_count; ++o) {
		if (starts[o + 1] == starts[o]) { continue; }
		present.push_back(::docset::doc_t(o));
		compact.push_back(starts[o + 1]);
	}
	return encode_present(k, multi, doc_count, present, compact, flat, dictionary, opts);
}

// A bytes column's dictionary from its sorted distinct values: leb count, count + 1 u64
// offsets, the values' bytes.
inline std::string put_dictionary(const std::vector<std::string_view>& terms) {
	std::string dictionary;
	format::put_leb(dictionary, terms.size());
	std::uint64_t offset = 0;
	format::put<std::uint64_t>(dictionary, offset);
	for (const auto t : terms) { offset += t.size(); format::put<std::uint64_t>(dictionary, offset); }
	for (const auto t : terms) { dictionary += t; }
	return dictionary;
}

}  // namespace detail

// Encodes a numeric slot: per ordinal, its mapped values (none when absent). Multi-valued
// slots keep duplicates, as a sorted multiset.
inline std::string encode(kind k, bool multi, const std::vector<std::vector<std::uint64_t>>& docs, const options& opts = {}) {
	if (k == kind::bytes) { throw std::invalid_argument("strata: doc values: bytes slots encode through encode_bytes"); }
	std::vector<std::uint64_t> starts{0}, flat;
	for (const auto& d : docs) {
		const auto from = flat.size();
		flat.insert(flat.end(), d.begin(), d.end());
		std::sort(flat.begin() + std::ptrdiff_t(from), flat.end());
		starts.push_back(flat.size());
	}
	return detail::encode_column(k, multi, starts, flat, nullptr, opts);
}

// Encodes a bytes slot: a sorted dictionary of the distinct values, and per ordinal its
// values' dictionary ordinals as a sorted set.
inline std::string encode_bytes(bool multi, const std::vector<std::vector<std::string>>& docs, const options& opts = {}) {
	std::vector<std::string> terms;
	for (const auto& d : docs) { terms.insert(terms.end(), d.begin(), d.end()); }
	std::sort(terms.begin(), terms.end());
	terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
	const std::string dictionary = detail::put_dictionary(std::vector<std::string_view>(terms.begin(), terms.end()));
	std::vector<std::uint64_t> starts{0}, flat;
	for (const auto& d : docs) {
		const auto from = flat.size();
		for (const auto& t : d) { flat.push_back(std::uint64_t(std::lower_bound(terms.begin(), terms.end(), t) - terms.begin())); }
		std::sort(flat.begin() + std::ptrdiff_t(from), flat.end());
		flat.erase(std::unique(flat.begin() + std::ptrdiff_t(from), flat.end()), flat.end());
		starts.push_back(flat.size());
	}
	return detail::encode_column(kind::bytes, multi, starts, flat, &dictionary, opts);
}

// A validated view over an encoded column; the bytes must outlive it. Malformed input
// throws corrupt_segment, at open or, for offsets checked lazily, at read.
class column_view {
public:
	static column_view open(const char* data, std::size_t size) {
		const char* p = data;
		const char* const end = data + size;
		column_view c;
		if (size < 3) { corrupt("truncated doc-value column"); }
		if (std::uint8_t(p[0]) != detail::version) { corrupt("doc-value column version"); }
		const auto k = std::uint8_t(p[1]), flags = std::uint8_t(p[2]);
		p += 3;
		if (k > std::uint8_t(kind::bytes) || (flags & ~(detail::flag_multi | detail::flag_presence))) { corrupt("doc-value column kind"); }
		c.kind_ = kind(k);
		c.multi_ = flags & detail::flag_multi;
		c.doc_count_ = format::get_leb(&p, end);
		if (c.doc_count_ > std::numeric_limits<ordinal_t>::max()) { corrupt("doc-value ordinals"); }
		c.stats_.docs = format::get_leb(&p, end);
		c.stats_.values = format::get_leb(&p, end);
		need(p, end, 16);
		c.stats_.min = format::get<std::uint64_t>(p);
		c.stats_.max = format::get<std::uint64_t>(p + 8);
		p += 16;
		if (detail::has_sum(c.kind_)) {
			need(p, end, 16);
			const auto low = format::get<std::uint64_t>(p), high = format::get<std::uint64_t>(p + 8);
			p += 16;
			c.stats_.has_sum = true;
			c.stats_.sum = static_cast<__int128>((static_cast<unsigned __int128>(high) << 64) | low);
		}
		if (c.stats_.docs > c.doc_count_) { corrupt("doc-value documents"); }
		if (flags & detail::flag_presence) {
			const auto n = format::get_leb(&p, end);
			need(p, end, n);
			c.presence_ = ::docset::frozen_view::open(reinterpret_cast<const std::uint8_t*>(p), std::size_t(n));
			// Every present ordinal lies below the column's ordinals.
			if (!c.presence_ || c.presence_->cardinality() != c.stats_.docs || c.presence_->rank(::docset::doc_t(c.doc_count_)) != c.stats_.docs) {
				corrupt("doc-value presence");
			}
			p += n;
		} else if (c.stats_.docs != c.doc_count_) {
			corrupt("doc-value presence");
		}
		if (c.kind_ == kind::bytes) { c.parse_dictionary(&p, end); }
		c.group_docs_ = format::get_leb(&p, end);
		if (!c.group_docs_ || c.group_docs_ > detail::max_group_docs) { corrupt("doc-value group size"); }
		c.groups_ = std::size_t((c.stats_.docs + c.group_docs_ - 1) / c.group_docs_);
		need(p, end, std::uint64_t(c.groups_) * detail::group_entry);
		c.group_directory_ = p;
		p += c.groups_ * detail::group_entry;
		if (c.multi_) {
			c.offsets_.parse(&p, end);
			if (c.offsets_.count != c.stats_.docs + 1) { corrupt("doc-value offsets"); }
		} else if (c.stats_.values != c.stats_.docs) {
			corrupt("doc-value count");
		}
		c.values_.parse(&p, end);
		if (c.values_.count != c.stats_.values) { corrupt("doc-value count"); }
		if (p != end) { corrupt("doc-value trailing bytes"); }
		c.check_groups();
		// The statistics' bounds match the blocks'.
		std::uint64_t lo = c.stats_.values ? std::numeric_limits<std::uint64_t>::max() : 0, hi = 0;
		for (std::size_t b = 0; b < c.values_.blocks; ++b) {
			const auto e = c.values_.block(b);
			lo = std::min(lo, e.min);
			hi = std::max(hi, e.max);
		}
		if (lo != c.stats_.min || hi != c.stats_.max) { corrupt("doc-value bounds"); }
		if (c.kind_ == kind::bytes && c.stats_.values && c.stats_.max >= c.terms_) { corrupt("doc-value ordinal"); }
		return c;
	}

	kind type() const noexcept { return kind_; }
	bool multi_valued() const noexcept { return multi_; }
	std::uint64_t doc_count() const noexcept { return doc_count_; }
	const statistics& stats() const noexcept { return stats_; }

	bool has(ordinal_t o) const noexcept {
		if (o >= doc_count_) { return false; }
		return !presence_ || presence_->contains(::docset::doc_t(o));
	}
	// A present ordinal's index among the present ones.
	std::uint64_t doc_index(ordinal_t o) const noexcept { return presence_ ? presence_->rank(::docset::doc_t(o)) : o; }

	// Replaces `out` with the ordinal's values, sorted; returns their count.
	std::size_t values(ordinal_t o, std::vector<std::uint64_t>& out) const {
		out.clear();
		if (!has(o)) { return 0; }
		const std::uint64_t i = doc_index(o);
		if (!multi_) { out.push_back(values_.at(i)); return 1; }
		const std::uint64_t start = offsets_.at(i), stop = offsets_.at(i + 1);
		if (start > stop || stop > values_.count) { corrupt("doc-value offsets"); }
		out.resize(std::size_t(stop - start));
		values_.copy(start, stop, out.data());
		return out.size();
	}
	// A single-valued column's value, reading only its bits.
	std::optional<std::uint64_t> value(ordinal_t o) const {
		if (!has(o)) { return std::nullopt; }
		if (!multi_) { return values_.at(doc_index(o)); }
		const std::uint64_t start = offsets_.at(doc_index(o));
		if (start >= values_.count) { corrupt("doc-value offsets"); }
		return values_.at(start);  // a multi-valued document's smallest
	}
	// Values by value index: one, or the range [start, stop) into `out`.
	std::uint64_t value_at(std::uint64_t index) const {
		if (index >= values_.count) { throw std::out_of_range("strata: doc values: value index"); }
		return values_.at(index);
	}
	void values_at(std::uint64_t start, std::uint64_t stop, std::vector<std::uint64_t>& out) const {
		if (start > stop || stop > values_.count) { throw std::out_of_range("strata: doc values: value range"); }
		out.resize(std::size_t(stop - start));
		values_.copy(start, stop, out.data());
	}

	// Packing blocks of block_values, in value-index order.
	std::size_t block_count() const noexcept { return values_.blocks; }
	block_info block(std::size_t b) const {
		if (b >= values_.blocks) { throw std::out_of_range("strata: doc values: block"); }
		return values_.block(b);
	}

	// Skip groups of group_docs() documents with a value each (the last may hold fewer).
	std::uint64_t group_docs() const noexcept { return group_docs_; }
	std::size_t group_count() const noexcept { return groups_; }
	skip_entry group(std::size_t g) const {
		if (g >= groups_) { throw std::out_of_range("strata: doc values: group"); }
		return entry(g);
	}
	// The first group whose span ends at or after `o`, or group_count().
	std::size_t find_group(ordinal_t o) const noexcept {
		std::size_t lo = 0, hi = groups_;
		while (lo < hi) {
			const auto mid = lo + (hi - lo) / 2;
			if (entry(mid).last < o) { lo = mid + 1; } else { hi = mid; }
		}
		return lo;
	}

	// Bytes slots: the dictionary.
	std::uint64_t terms() const noexcept { return terms_; }
	std::string_view term(std::uint64_t ord) const {
		if (ord >= terms_) { corrupt("doc-value ordinal"); }
		const auto from = term_offset(ord), to = term_offset(ord + 1);
		return {term_data_ + from, std::size_t(to - from)};
	}
	std::optional<std::uint64_t> find(std::string_view t) const {
		std::uint64_t lo = 0, hi = terms_;
		while (lo < hi) {
			const auto mid = lo + (hi - lo) / 2;
			if (term(mid) < t) { lo = mid + 1; } else { hi = mid; }
		}
		if (lo < terms_ && term(lo) == t) { return lo; }
		return std::nullopt;
	}

private:
	struct packed_values {
		std::uint64_t count = 0;
		std::size_t blocks = 0, packed_size = 0;
		const char* directory = nullptr;
		const char* packed = nullptr;

		void parse(const char** p, const char* end) {
			count = format::get_leb(p, end);
			const std::uint64_t b = count / block_values + (count % block_values != 0);
			if (b > std::size_t(end - *p) / detail::block_entry) { corrupt("doc-value blocks"); }
			blocks = std::size_t(b);
			directory = *p;
			*p += blocks * detail::block_entry;
			const auto n = format::get_leb(p, end);
			need(*p, end, n);
			packed_size = std::size_t(n);
			packed = *p;
			*p += packed_size;
			std::size_t at = 0;
			for (std::size_t i = 0; i < blocks; ++i) {
				const auto e = block(i);
				if (e.bits > 64 || !e.gcd || e.min > e.max || offset(i) != at) { corrupt("doc-value block"); }
				at += ::bitpack::packed_bytes(e.bits);
			}
			if (at != packed_size) { corrupt("doc-value packed size"); }
		}
		block_info block(std::size_t b) const noexcept {
			const char* e = directory + b * detail::block_entry;
			return {format::get<std::uint64_t>(e), format::get<std::uint64_t>(e + 8), format::get<std::uint64_t>(e + 16), unsigned(std::uint8_t(e[32]))};
		}
		std::uint64_t offset(std::size_t b) const noexcept { return format::get<std::uint64_t>(directory + b * detail::block_entry + 24); }
		// Every block's offset and width were checked at parse, so its bytes are in range.
		const std::uint8_t* bytes(std::size_t b) const noexcept { return reinterpret_cast<const std::uint8_t*>(packed + std::size_t(offset(b))); }
		void decode(std::size_t b, std::uint64_t* out) const noexcept {
			const auto e = block(b);
			::bitpack::unpack(bytes(b), e.bits, out);
			for (std::size_t i = 0; i < block_values; ++i) { out[i] = e.min + out[i] * e.gcd; }
		}
		std::uint64_t at(std::uint64_t i) const {
			if (i >= count) { corrupt("doc-value index"); }
			const auto b = std::size_t(i / block_values);
			const auto e = block(b);
			return e.min + extract(bytes(b), e.bits, std::size_t(i % block_values)) * e.gcd;
		}
		void copy(std::uint64_t start, std::uint64_t stop, std::uint64_t* out) const noexcept {
			std::uint64_t buffer[block_values];
			for (std::uint64_t i = start; i < stop;) {
				const auto b = i / block_values;
				decode(std::size_t(b), buffer);
				const std::uint64_t block_end = std::min(stop, (b + 1) * block_values);
				for (; i < block_end; ++i) { *out++ = buffer[i % block_values]; }
			}
		}
	};

	column_view() = default;

	static void need(const char* p, const char* end, std::uint64_t n) {
		if (n > std::uint64_t(end - p)) { corrupt("truncated doc-value column"); }
	}
	std::uint64_t term_offset(std::uint64_t i) const noexcept { return format::get<std::uint64_t>(term_offsets_ + i * 8); }
	skip_entry entry(std::size_t g) const noexcept {
		const char* e = group_directory_ + g * detail::group_entry;
		return {format::get<std::uint32_t>(e), format::get<std::uint32_t>(e + 4), format::get<std::uint64_t>(e + 8),
			format::get<std::uint64_t>(e + 16), format::get<std::uint64_t>(e + 24), format::get<std::uint64_t>(e + 32)};
	}

	// Each group holds the next group_docs present documents, whole, in ordinal order.
	void check_groups() const {
		std::uint64_t next_value = 0;
		for (std::size_t g = 0; g < groups_; ++g) {
			const auto e = entry(g);
			const std::uint64_t begin = g * group_docs_, end = std::min<std::uint64_t>(stats_.docs, begin + group_docs_);
			if (e.first > e.last || e.last >= doc_count_ || e.min > e.max || e.first_value != next_value || e.count < end - begin || e.count > stats_.values - next_value) {
				corrupt("doc-value group");
			}
			if (presence_) {
				if (!presence_->contains(e.first) || !presence_->contains(e.last) || presence_->rank(e.first) != begin || presence_->rank(e.last) != end - 1) {
					corrupt("doc-value group");
				}
			} else if (e.first != begin || e.last != end - 1) {
				corrupt("doc-value group");
			}
			if (multi_ ? (offsets_.at(begin) != e.first_value || offsets_.at(end) != e.first_value + e.count) : e.count != end - begin) {
				corrupt("doc-value group");
			}
			if (e.count && (e.min < stats_.min || e.max > stats_.max)) { corrupt("doc-value group"); }
			next_value += e.count;
		}
		if (next_value != stats_.values) { corrupt("doc-value group"); }
	}

	void parse_dictionary(const char** p, const char* end) {
		const auto n = format::get_leb(p, end);
		if (n >= std::uint64_t(end - *p) / 8) { corrupt("doc-value dictionary"); }  // n + 1 offsets
		terms_ = n;
		term_offsets_ = *p;
		*p += std::size_t(n + 1) * 8;
		const auto bytes = term_offset(n);
		need(*p, end, bytes);
		term_data_ = *p;
		*p += bytes;
		if (term_offset(0) != 0) { corrupt("doc-value dictionary"); }
		for (std::uint64_t i = 1; i <= n; ++i) {
			if (term_offset(i) < term_offset(i - 1)) { corrupt("doc-value dictionary"); }
		}
		for (std::uint64_t i = 1; i < n; ++i) {
			if (!(term(i - 1) < term(i))) { corrupt("doc-value dictionary order"); }
		}
	}

	kind kind_ = kind::u64;
	bool multi_ = false;
	std::uint64_t doc_count_ = 0;
	statistics stats_;
	std::optional<::docset::frozen_view> presence_;
	std::uint64_t group_docs_ = 0;
	std::size_t groups_ = 0;
	const char* group_directory_ = nullptr;
	packed_values offsets_, values_;
	std::uint64_t terms_ = 0;
	const char* term_offsets_ = nullptr;
	const char* term_data_ = nullptr;
};

}  // namespace prospect::docvalues
