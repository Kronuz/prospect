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


// Collapse (Phase 4, D9): keep at most collapse_max documents per collapse key, the best
// ranked, and count the rest. A slot's collapse key is its complete value list: sorted
// for numeric and bytes slots, stored order for binary slots. Documents without a value
// are never collapsed.

#pragma once

#include "prospect/ordered.hh"
#include "varint.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prospect::collapse {

namespace detail {

inline void put_bytes(std::string& out, std::string_view v) {
	char buf[varint::leb128::max_bytes];
	out.append(buf, varint::leb128::encode(buf, v.size()));
	out.append(v);
}

}  // namespace detail

// A numeric slot's key: its sorted multiset as 8-byte big-endian ordered keys.
inline std::optional<std::string> numeric_key(std::span<const std::uint64_t> values) {
	if (values.empty()) { return std::nullopt; }
	std::vector<std::uint64_t> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	std::string out;
	out.reserve(8 * sorted.size());
	unsigned char buf[8];
	for (const auto v : sorted) {
		ordered::put_be(v, buf);
		out.append(reinterpret_cast<const char*>(buf), 8);
	}
	return out;
}

// A bytes slot's key: its sorted set, each value length-prefixed.
inline std::optional<std::string> bytes_key(std::span<const std::string_view> values) {
	if (values.empty()) { return std::nullopt; }
	std::vector<std::string_view> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	std::string out;
	for (const auto v : sorted) { detail::put_bytes(out, v); }
	return out;
}

// A binary slot's key: its values in stored order, duplicates kept.
inline std::optional<std::string> binary_key(std::span<const std::string_view> values) {
	if (values.empty()) { return std::nullopt; }
	std::string out;
	for (const auto v : values) { detail::put_bytes(out, v); }
	return out;
}

// Rank by order of arrival: an earlier document ranks higher.
struct arrival {
	template <typename T>
	bool operator()(const T&, const T&) const noexcept { return false; }
};

template <typename Item>
struct kept {
	Item item;
	std::optional<std::string> key;
	std::uint64_t collapse_count = 0;  // the key's other matches that were not kept
};

template <typename Item>
struct result {
	std::vector<kept<Item>> items;  // best first
	bool counts_exact = true;       // false: collapse counts are lower bounds
};

// Better(a, b) says a ranks above b; ties, and the default, rank by arrival. A key's
// collapse count is reported on each of its kept documents.
template <typename Item, typename Better = arrival>
class collapser {
public:
	explicit collapser(std::size_t collapse_max, Better better = {}) : max_(collapse_max), better_(std::move(better)) {
		if (!collapse_max) { throw std::invalid_argument("collapse: collapse_max must be at least 1"); }
	}

	// Returns whether item is kept for now; a better document with its key may evict it.
	bool add(Item item, std::optional<std::string> key) {
		entry e{std::move(item), seq_++};
		if (!key) { loose_.push_back(std::move(e)); return true; }
		group& g = groups_[std::move(*key)];
		if (g.kept.size() < max_) { g.kept.push_back(std::move(e)); return true; }
		++g.dropped;
		const auto worst = std::max_element(g.kept.begin(), g.kept.end(), [this](const entry& a, const entry& b) { return ranks_before(a, b); });
		if (!ranks_before(e, *worst)) { return false; }
		*worst = std::move(e);
		return true;
	}

	// Counts documents with key dropped elsewhere, as a shard's collapse does.
	void add_dropped(const std::string& key, std::uint64_t n) { groups_[key].dropped += n; }

	// The kept documents, best first; pruned says some matches were never offered.
	result<Item> take(bool pruned = false) && {
		std::vector<std::pair<std::uint64_t, kept<Item>>> out;
		for (auto& e : loose_) { out.push_back({e.seq, kept<Item>{std::move(e.item), std::nullopt, 0}}); }
		for (auto& [k, g] : groups_) {
			for (auto& e : g.kept) { out.push_back({e.seq, kept<Item>{std::move(e.item), k, g.dropped}}); }
		}
		std::sort(out.begin(), out.end(), [this](const auto& a, const auto& b) {
			if (better_(a.second.item, b.second.item)) { return true; }
			if (better_(b.second.item, a.second.item)) { return false; }
			return a.first < b.first;
		});
		result<Item> r;
		r.counts_exact = !pruned;
		r.items.reserve(out.size());
		for (auto& p : out) { r.items.push_back(std::move(p.second)); }
		return r;
	}

private:
	struct entry {
		Item item;
		std::uint64_t seq;
	};
	struct group {
		std::vector<entry> kept;
		std::uint64_t dropped = 0;
	};
	bool ranks_before(const entry& a, const entry& b) const {
		if (better_(a.item, b.item)) { return true; }
		if (better_(b.item, a.item)) { return false; }
		return a.seq < b.seq;
	}

	std::size_t max_;
	Better better_;
	std::uint64_t seq_ = 0;
	std::unordered_map<std::string, group> groups_;
	std::vector<entry> loose_;  // documents without a key
};

// The coordinator's collapse: each shard collapsed to collapse_max per key; collapsing
// their union again keeps the global best per key, and each key's count is the shards'
// counts plus the copies dropped here. Exact when every shard's counts are.
template <typename Item, typename Better = arrival>
result<Item> merge(const std::vector<result<Item>>& shards, std::size_t collapse_max, Better better = {}) {
	collapser<Item, Better> c(collapse_max, std::move(better));
	bool exact = true;
	for (const auto& s : shards) {
		exact = exact && s.counts_exact;
		std::unordered_set<std::string> counted;
		for (const auto& k : s.items) {
			if (k.key && counted.insert(*k.key).second) { c.add_dropped(*k.key, k.collapse_count); }
			c.add(k.item, k.key);
		}
	}
	return std::move(c).take(!exact);
}

// Match-count bounds with an estimate between them.
struct triple {
	std::uint64_t lower = 0, estimate = 0, upper = 0;
	bool operator==(const triple&) const = default;
};

// Uncollapsed counts are disjoint across shards, so every bound sums.
inline triple merge_uncollapsed(std::span<const triple> shards) noexcept {
	triple t;
	for (const auto& s : shards) { t.lower += s.lower; t.estimate += s.estimate; t.upper += s.upper; }
	return t;
}

// A collapse key can span shards: the lower bound is the largest shard's, the upper the
// sum, and the summed estimate is held between them.
inline triple merge_collapsed(std::span<const triple> shards) noexcept {
	triple t;
	for (const auto& s : shards) { t.lower = std::max(t.lower, s.lower); t.estimate += s.estimate; t.upper += s.upper; }
	t.estimate = std::min(std::max(t.estimate, t.lower), t.upper);
	return t;
}

}  // namespace prospect::collapse
