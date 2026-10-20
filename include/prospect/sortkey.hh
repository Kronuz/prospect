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


// Sort keys (Phase 4, D8): a key is an ordered list of components, each a slot, a
// direction, and a selection rule for multi-valued slots. A document's key is a typed
// tuple compared component by component; missing values sort last in either direction
// and ties break by docid ascending. Keys cross shards in a self-describing wire form,
// bytes as bytes, so a coordinator merges them without knowing the slots.

#pragma once

#include "prospect/ordered.hh"
#include "varint.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace prospect::sortkey {

enum class direction : std::uint8_t { ascending, descending };

// A component's type: an ordered key (ordered.hh), a distance to a reference, or bytes.
enum class type : std::uint8_t { u64, distance, bytes };

// Which of a multi-valued slot's values a document sorts by.
enum class selection : std::uint8_t { smallest, largest, closest, farthest };

struct component {
	std::uint32_t slot = 0;
	direction dir = direction::ascending;
	type kind = type::u64;
	selection rule = selection::smallest;
	double reference = 0.0;  // distance components only

	// Ascending takes the smallest value, descending the largest.
	static component by_value(std::uint32_t slot, type kind, direction dir) noexcept {
		return {slot, dir, kind, dir == direction::ascending ? selection::smallest : selection::largest, 0.0};
	}
	// The closest or farthest value's distance to reference, nearest first when ascending.
	static component by_distance(std::uint32_t slot, double reference, selection rule = selection::closest, direction dir = direction::ascending) noexcept {
		return {slot, dir, type::distance, rule, reference};
	}
};

using spec = std::vector<component>;

// A component's value: monostate when missing, else the alternative matching kind.
using value = std::variant<std::monostate, std::uint64_t, double, std::string>;

struct part {
	direction dir = direction::ascending;
	type kind = type::u64;
	value v;
	bool missing() const noexcept { return v.index() == 0; }
	bool operator==(const part&) const = default;
};

using key = std::vector<part>;

struct hit {
	key k;
	std::uint64_t docid = 0;
	bool operator==(const hit&) const = default;
};

// Missing sorts last whatever the direction; parts of mismatched shape order by direction
// and type, so the order stays total.
inline int compare(const part& a, const part& b) noexcept {
	if (a.missing() || b.missing()) { return int(a.missing()) - int(b.missing()); }
	if (a.dir != b.dir) { return a.dir < b.dir ? -1 : 1; }
	if (a.v.index() != b.v.index()) { return a.v.index() < b.v.index() ? -1 : 1; }
	int c = 0;
	if (const auto* x = std::get_if<std::uint64_t>(&a.v)) {
		const auto y = *std::get_if<std::uint64_t>(&b.v);
		c = int(*x > y) - int(*x < y);
	} else if (const auto* d = std::get_if<double>(&a.v)) {
		const auto e = *std::get_if<double>(&b.v);
		c = int(*d > e) - int(*d < e);
	} else {
		const int r = std::string_view(*std::get_if<std::string>(&a.v)).compare(*std::get_if<std::string>(&b.v));
		c = int(r > 0) - int(r < 0);
	}
	return a.dir == direction::descending ? -c : c;
}

inline int compare(const key& a, const key& b) noexcept {
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; ++i) {
		if (const int c = compare(a[i], b[i])) { return c; }
	}
	return int(a.size() > b.size()) - int(a.size() < b.size());
}

// Whether a ranks before b: the smaller key, then the smaller docid.
inline bool before(const hit& a, const hit& b) noexcept {
	const int c = compare(a.k, b.k);
	return c ? c < 0 : a.docid < b.docid;
}

// |reference - v|, with equal infinities at distance 0 rather than NaN.
inline double distance(double reference, double v) noexcept { return reference == v ? 0.0 : std::fabs(reference - v); }

// A document's part for a component from its slot's values (NaN is rejected upstream).
inline part select_u64(const component& c, std::span<const std::uint64_t> values) {
	part p{c.dir, type::u64, {}};
	if (values.empty()) { return p; }
	p.v.emplace<std::uint64_t>(c.rule == selection::largest ? *std::max_element(values.begin(), values.end()) : *std::min_element(values.begin(), values.end()));
	return p;
}
inline part select_bytes(const component& c, std::span<const std::string_view> values) {
	part p{c.dir, type::bytes, {}};
	if (values.empty()) { return p; }
	p.v.emplace<std::string>(c.rule == selection::largest ? *std::max_element(values.begin(), values.end()) : *std::min_element(values.begin(), values.end()));
	return p;
}
inline part select_distance(const component& c, std::span<const double> values) {
	part p{c.dir, type::distance, {}};
	if (values.empty()) { return p; }
	const bool farthest = c.rule == selection::farthest || c.rule == selection::largest;
	double best = distance(c.reference, values[0]);
	for (const double v : values.subspan(1)) {
		const double d = distance(c.reference, v);
		if (farthest ? d > best : d < best) { best = d; }
	}
	p.v.emplace<double>(best);
	return p;
}

// The wire form: a version byte, the part count, then per part a tag (kind in bits 0-1,
// descending in bit 2, missing in bit 3) and, when present, 8 big-endian bytes of the
// ordered key (distances through ordered::from_f64) or a length and the bytes. Counts
// and lengths are minimal LEB128, so every key has exactly one encoding.
struct malformed_key : std::runtime_error {
	using std::runtime_error::runtime_error;
};

inline constexpr unsigned char wire_version = 1;

namespace detail {

inline void put_varint(std::string& out, std::uint64_t v) {
	char buf[varint::leb128::max_bytes];
	out.append(buf, varint::leb128::encode(buf, v));
}

inline std::size_t varint_size(std::uint64_t v) noexcept { return v ? (std::size_t(std::bit_width(v)) + 6) / 7 : 1; }

inline void put_u64(std::string& out, std::uint64_t k) {
	unsigned char buf[8];
	ordered::put_be(k, buf);
	out.append(reinterpret_cast<const char*>(buf), 8);
}

class reader {
public:
	explicit reader(std::string_view in) noexcept : p_(in.data()), end_(in.data() + in.size()) {}
	unsigned char byte() {
		if (p_ == end_) { throw malformed_key("sortkey: truncated"); }
		return static_cast<unsigned char>(*p_++);
	}
	std::uint64_t u64() {
		if (end_ - p_ < 8) { throw malformed_key("sortkey: truncated"); }
		std::uint64_t k = 0;
		for (int i = 0; i < 8; ++i) { k = (k << 8) | static_cast<unsigned char>(p_[i]); }
		p_ += 8;
		return k;
	}
	std::uint64_t number() {
		const char* start = p_;
		std::uint64_t v = 0;
		if (!varint::leb128::decode(&p_, end_, v)) { throw malformed_key("sortkey: truncated varint"); }
		if (std::size_t(p_ - start) != varint_size(v)) { throw malformed_key("sortkey: overlong varint"); }
		return v;
	}
	std::string_view bytes(std::uint64_t n) {
		if (n > std::uint64_t(end_ - p_)) { throw malformed_key("sortkey: truncated bytes"); }
		const std::string_view s(p_, std::size_t(n));
		p_ += n;
		return s;
	}
	std::size_t left() const noexcept { return std::size_t(end_ - p_); }

private:
	const char* p_;
	const char* end_;
};

}  // namespace detail

inline std::string encode(const key& k) {
	std::string out;
	out.push_back(static_cast<char>(wire_version));
	detail::put_varint(out, k.size());
	for (const auto& p : k) {
		const unsigned kind = unsigned(p.kind);
		if (kind > 2) { throw std::invalid_argument("sortkey: unknown component type"); }
		out.push_back(static_cast<char>(kind | (p.dir == direction::descending ? 4u : 0u) | (p.missing() ? 8u : 0u)));
		if (p.missing()) { continue; }
		if (p.v.index() != std::size_t(kind) + 1) { throw std::invalid_argument("sortkey: part value does not match its type"); }
		switch (p.kind) {
		case type::u64: detail::put_u64(out, *std::get_if<std::uint64_t>(&p.v)); break;
		case type::distance: {
			const double d = *std::get_if<double>(&p.v);
			if (!(d >= 0.0)) { throw std::invalid_argument("sortkey: negative or NaN distance"); }
			detail::put_u64(out, ordered::from_f64(d));
			break;
		}
		case type::bytes: {
			const auto& s = *std::get_if<std::string>(&p.v);
			detail::put_varint(out, s.size());
			out.append(s);
			break;
		}
		}
	}
	return out;
}

// Bounds-checked; throws malformed_key on anything encode would not have written.
inline key decode(std::string_view wire) {
	detail::reader in(wire);
	if (in.byte() != wire_version) { throw malformed_key("sortkey: version"); }
	const std::uint64_t n = in.number();
	if (n > in.left()) { throw malformed_key("sortkey: count"); }  // each part takes a byte at least
	key k;
	k.reserve(std::size_t(n));
	for (std::uint64_t i = 0; i < n; ++i) {
		const unsigned tag = in.byte();
		if ((tag & ~15u) || (tag & 3u) > 2) { throw malformed_key("sortkey: tag"); }
		part p{tag & 4u ? direction::descending : direction::ascending, type(tag & 3u), {}};
		if (!(tag & 8u)) {
			switch (p.kind) {
			case type::u64: p.v.emplace<std::uint64_t>(in.u64()); break;
			case type::distance: {
				const double d = ordered::to_f64(in.u64());
				if (std::isnan(d) || std::signbit(d)) { throw malformed_key("sortkey: distance"); }
				p.v.emplace<double>(d);
				break;
			}
			case type::bytes: {
				const std::uint64_t len = in.number();
				p.v.emplace<std::string>(in.bytes(len));
				break;
			}
			}
		}
		k.push_back(std::move(p));
	}
	if (in.left()) { throw malformed_key("sortkey: trailing bytes"); }
	return k;
}

// The k best hits, kept in a heap whose top is the worst kept.
class top_k {
public:
	explicit top_k(std::size_t k) : k_(k) { heap_.reserve(k); }

	bool full() const noexcept { return heap_.size() >= k_; }
	// The worst kept hit once full, the bar a newcomer must beat; null before then.
	const hit* worst() const noexcept { return full() && !heap_.empty() ? &heap_.front() : nullptr; }
	// Whether h would enter.
	bool competes(const hit& h) const noexcept { return !full() || (!heap_.empty() && before(h, heap_.front())); }

	bool push(hit h) {
		if (!competes(h)) { return false; }
		if (full()) {
			std::pop_heap(heap_.begin(), heap_.end(), before);
			heap_.back() = std::move(h);
		} else {
			heap_.push_back(std::move(h));
		}
		std::push_heap(heap_.begin(), heap_.end(), before);
		return true;
	}

	// The kept hits, best first.
	std::vector<hit> take() && {
		std::sort_heap(heap_.begin(), heap_.end(), before);
		return std::move(heap_);
	}

private:
	std::size_t k_;
	std::vector<hit> heap_;
};

// The top k of the union of several top-k lists, which equals the top k of the union of
// their documents when each list is its shard's top k and docids are unique.
inline std::vector<hit> merge(std::span<const std::vector<hit>> lists, std::size_t k) {
	std::vector<hit> all;
	for (const auto& l : lists) { all.insert(all.end(), l.begin(), l.end()); }
	if (all.size() > k) {
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), before);
		all.resize(k);
	} else {
		std::sort(all.begin(), all.end(), before);
	}
	return all;
}

// A block's bounds over the first component's slot, present values only: u64 or bytes
// for value components, the slot's doubles for distance components.
struct block_bounds {
	value min, max;
};

// Whether a block may hold a document that beats worst, from the first component alone.
// Value components bound the selected value by [min, max]; distance components bound the
// distance over [min, max], which is monotone on each side of the reference. A bound of
// the wrong type, a missing worst part, or no worst never skips. An equal first part
// skips only for one-component keys over docid-ordered blocks, whose docids all exceed
// worst's.
inline bool may_compete(const spec& s, const block_bounds& block, const hit* worst, bool docid_ordered) noexcept {
	if (!worst || s.empty() || worst->k.empty()) { return true; }
	const part& w = worst->k.front();
	const component& c = s.front();
	if (w.missing() || w.dir != c.dir || w.kind != c.kind) { return true; }
	const bool ascending = c.dir == direction::ascending;
	int cmp = 0;  // the block's best value against worst's, ascending
	switch (c.kind) {
	case type::u64: {
		const auto* lo = std::get_if<std::uint64_t>(&block.min);
		const auto* hi = std::get_if<std::uint64_t>(&block.max);
		const auto* x = std::get_if<std::uint64_t>(&w.v);
		if (!lo || !hi || !x) { return true; }
		const std::uint64_t v = ascending ? *lo : *hi;
		cmp = int(v > *x) - int(v < *x);
		break;
	}
	case type::bytes: {
		const auto* lo = std::get_if<std::string>(&block.min);
		const auto* hi = std::get_if<std::string>(&block.max);
		const auto* x = std::get_if<std::string>(&w.v);
		if (!lo || !hi || !x) { return true; }
		const int r = std::string_view(ascending ? *lo : *hi).compare(*x);
		cmp = int(r > 0) - int(r < 0);
		break;
	}
	case type::distance: {
		const auto* lo = std::get_if<double>(&block.min);
		const auto* hi = std::get_if<double>(&block.max);
		const auto* x = std::get_if<double>(&w.v);
		if (!lo || !hi || !x || std::isnan(*lo) || std::isnan(*hi) || std::isnan(c.reference)) { return true; }
		const double to_lo = distance(c.reference, *lo), to_hi = distance(c.reference, *hi);
		const double nearest = *lo <= c.reference && c.reference <= *hi ? 0.0 : std::min(to_lo, to_hi);
		const double v = ascending ? nearest : std::max(to_lo, to_hi);
		cmp = int(v > *x) - int(v < *x);
		break;
	}
	default: return true;
	}
	if (!ascending) { cmp = -cmp; }
	if (cmp != 0) { return cmp < 0; }
	return s.size() > 1 || !docid_ordered;
}

}  // namespace prospect::sortkey
