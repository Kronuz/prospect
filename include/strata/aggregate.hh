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

// Aggregation collectors (Phase 4, D11): metrics and buckets fed one document at a time
// with its values per slot. A partial merges associatively and commutatively, and its
// final result is a function of the exact state alone, so one shard and any split into
// shards give byte-identical results.
//
// Numeric slots are multisets: `count` counts values, sums add duplicates, and a bucket
// counts a document once. Variance is the sample variance (n - 1), as Xapiand reports it.
// Nesting is evaluated with explicit work stacks, never recursion, so depth is bounded by
// memory alone; bytes() reports what a partial holds, for a query budget to charge.
//
// Floating point: no expression here is contractible (edges and bounds use explicit fma),
// and callers compile with -ffp-contract=off so their own arithmetic agrees across
// x86_64 and arm64. A fractional histogram buckets by floor((x - offset) / interval);
// integer values with an integer interval bucket by floor division on integers.

#pragma once

#include "strata/exact.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace strata::aggregate {

using slot_t = std::uint32_t;
using node_id = std::uint32_t;

// A numeric value. NaN never enters (stage 1 rejects it; collectors skip it) and -0.0
// enters as +0.0, so equal values are one key.
using number = std::variant<std::int64_t, std::uint64_t, double>;
// One document's values for a slot.
using values = std::variant<std::span<const std::int64_t>, std::span<const std::uint64_t>, std::span<const double>, std::span<const std::string_view>>;
// A bucket's key: none (filter), a value, or a name (range).
using key = std::variant<std::monostate, std::int64_t, std::uint64_t, double, std::string>;

// A document as collectors see it: its values for any slot, valid until the next call.
struct document {
	virtual ~document() = default;
	virtual values slot(slot_t s) const = 0;
};

namespace detail {

// The sign of x - d, exactly.
inline int compare(__int128 x, double d) noexcept {
	if (std::isnan(d)) { return 0; }
	if (d >= 0x1p64) { return -1; }
	if (d < -0x1p63) { return 1; }
	const double t = std::floor(d);
	const auto i = static_cast<__int128>(t);
	if (x != i) { return x < i ? -1 : 1; }
	return d > t ? -1 : 0;
}

inline bool integral(const number& n, __int128& out) noexcept {
	if (const auto* p = std::get_if<std::int64_t>(&n)) { out = *p; return true; }
	if (const auto* p = std::get_if<std::uint64_t>(&n)) { out = *p; return true; }
	return false;
}

inline unsigned __int128 magnitude(__int128 v) noexcept {
	return v < 0 ? -static_cast<unsigned __int128>(v) : static_cast<unsigned __int128>(v);
}

// Adds ±m exactly, in 40-bit pieces that are each an exact double.
inline void add_magnitude(exact::double_sum& s, unsigned __int128 m, bool negative) noexcept {
	for (int shift = 0; m != 0; shift += 40, m >>= 40) {
		const double part = std::ldexp(double(std::uint64_t(m) & ((1ull << 40) - 1)), shift);
		s.add(negative ? -part : part);
	}
}
inline void add_integer(exact::double_sum& s, __int128 v) noexcept { add_magnitude(s, magnitude(v), v < 0); }

// Floor division for b > 0.
inline __int128 floor_div(__int128 a, __int128 b) noexcept {
	__int128 q = a / b;
	if (a % b != 0 && a < 0) { --q; }
	return q;
}

template <class F>
void for_each_number(const values& v, F&& f) {
	std::visit([&](const auto& span) {
		using T = typename std::remove_cvref_t<decltype(span)>::value_type;
		if constexpr (!std::is_same_v<T, std::string_view>) {
			for (T x : span) {
				if constexpr (std::is_same_v<T, double>) {
					if (std::isnan(x)) { continue; }
					if (x == 0.0) { x = 0.0; }
				}
				f(number(std::in_place_type<T>, x));
			}
		}
	}, v);
}

inline std::uint64_t value_count(const values& v) noexcept {
	return std::visit([](const auto& span) -> std::uint64_t {
		using T = typename std::remove_cvref_t<decltype(span)>::value_type;
		if constexpr (std::is_same_v<T, double>) {
			return std::uint64_t(std::count_if(span.begin(), span.end(), [](double x) { return !std::isnan(x); }));
		} else {
			return span.size();
		}
	}, v);
}

// The distinct keys of one document's values, sorted.
inline void distinct_keys(const values& v, std::vector<key>& out) {
	out.clear();
	std::visit([&](const auto& span) {
		using T = typename std::remove_cvref_t<decltype(span)>::value_type;
		for (T x : span) {
			if constexpr (std::is_same_v<T, double>) {
				if (std::isnan(x)) { continue; }
				if (x == 0.0) { x = 0.0; }
			}
			if constexpr (std::is_same_v<T, std::string_view>) {
				out.emplace_back(std::in_place_type<std::string>, x);
			} else {
				out.emplace_back(std::in_place_type<T>, x);
			}
		}
	}, v);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

inline key integer_key(__int128 v) {
	if (v >= std::numeric_limits<std::int64_t>::min() && v <= std::numeric_limits<std::int64_t>::max()) { return key(std::in_place_type<std::int64_t>, std::int64_t(v)); }
	if (v > 0 && v <= std::numeric_limits<std::uint64_t>::max()) { return key(std::in_place_type<std::uint64_t>, std::uint64_t(v)); }
	return key(std::in_place_type<double>, static_cast<double>(v));
}

// An rb-tree node's bookkeeping beside its value: color and three links.
inline constexpr std::size_t node_overhead = 4 * sizeof(void*);

inline std::size_t heap_bytes(const key& k) noexcept {
	const auto* s = std::get_if<std::string>(&k);
	return s && s->size() > 15 ? s->capacity() + 1 : 0;
}

}  // namespace detail

// The sign of a - b, exactly, across kinds.
inline int compare(const number& a, const number& b) noexcept {
	__int128 x = 0, y = 0;
	const bool xi = detail::integral(a, x), yi = detail::integral(b, y);
	if (xi && yi) { return (x > y) - (x < y); }
	if (xi) { return detail::compare(x, std::get<double>(b)); }
	if (yi) { return -detail::compare(y, std::get<double>(a)); }
	const double p = std::get<double>(a), q = std::get<double>(b);
	return (p > q) - (p < q);
}

// A total order: by value, then equal values by kind.
struct number_less {
	bool operator()(const number& a, const number& b) const noexcept {
		const int c = compare(a, b);
		return c != 0 ? c < 0 : a.index() < b.index();
	}
};

inline double to_double(const number& n) noexcept {
	return std::visit([](auto v) { return static_cast<double>(v); }, n);
}

inline void add_exact(exact::double_sum& s, const number& n) noexcept {
	__int128 v = 0;
	if (detail::integral(n, v)) { detail::add_integer(s, v); } else { s.add(std::get<double>(n)); }
}

// The shortest %g form that reads back as d.
inline std::string format_double(double d) {
	if (std::isnan(d)) { return "nan"; }
	if (std::isinf(d)) { return d > 0 ? "inf" : "-inf"; }
	char buffer[40];
	for (int precision = 1; precision <= 17; ++precision) {
		std::snprintf(buffer, sizeof buffer, "%.*g", precision, d);
		if (std::strtod(buffer, nullptr) == d) { break; }
	}
	return buffer;
}

inline std::string format_number(const number& n) {
	return std::visit([](auto v) -> std::string {
		if constexpr (std::is_same_v<decltype(v), double>) { return format_double(v); } else { return std::to_string(v); }
	}, n);
}

inline std::string format_key(const key& k) {
	return std::visit([](const auto& v) -> std::string {
		using T = std::remove_cvref_t<decltype(v)>;
		if constexpr (std::is_same_v<T, std::monostate>) { return {}; }
		else if constexpr (std::is_same_v<T, std::string>) { return v; }
		else if constexpr (std::is_same_v<T, double>) { return format_double(v); }
		else { return std::to_string(v); }
	}, k);
}

// Count, minimum, and maximum (`min`, `max`).
class bounds {
public:
	void add(const number& x) noexcept {
		++count_;
		if (!min_ || number_less{}(x, *min_)) { min_ = x; }
		if (!max_ || number_less{}(*max_, x)) { max_ = x; }
	}
	void add(const values& v) { detail::for_each_number(v, [&](const number& x) { add(x); }); }
	void merge(const bounds& o) noexcept {
		count_ += o.count_;
		if (o.min_ && (!min_ || number_less{}(*o.min_, *min_))) { min_ = o.min_; }
		if (o.max_ && (!max_ || number_less{}(*max_, *o.max_))) { max_ = o.max_; }
	}
	std::uint64_t count() const noexcept { return count_; }
	const std::optional<number>& min() const noexcept { return min_; }
	const std::optional<number>& max() const noexcept { return max_; }

private:
	std::uint64_t count_ = 0;
	std::optional<number> min_, max_;
};

// Bounds plus the exact sum (`sum`, `avg`, `stats`).
class stats {
public:
	void add(const number& x) noexcept {
		bounds_.add(x);
		if (const auto* i = std::get_if<std::int64_t>(&x)) { integers_.add(*i); }
		else if (const auto* u = std::get_if<std::uint64_t>(&x)) { integers_.value += *u; }
		else { reals_.add(std::get<double>(x)); }
	}
	void add(const values& v) { detail::for_each_number(v, [&](const number& x) { add(x); }); }
	void merge(const stats& o) noexcept { bounds_.merge(o.bounds_); integers_.merge(o.integers_); reals_.merge(o.reals_); }
	std::uint64_t count() const noexcept { return bounds_.count(); }
	const std::optional<number>& min() const noexcept { return bounds_.min(); }
	const std::optional<number>& max() const noexcept { return bounds_.max(); }
	// Every value's sum, rounded once.
	double sum() const noexcept {
		auto s = reals_;
		detail::add_integer(s, integers_.value);
		return s.value();
	}
	std::optional<double> avg() const noexcept {
		if (count() == 0) { return std::nullopt; }
		return sum() / double(count());
	}

private:
	bounds bounds_;
	exact::integer_sum integers_;
	exact::double_sum reals_;
};

// Stats plus the exact sum of squares (`extended_stats`, `variance`, `std_deviation`).
class extended : public stats {
public:
	void add(const number& x) noexcept {
		stats::add(x);
		__int128 v = 0;
		if (detail::integral(x, v)) {
			const unsigned __int128 m = detail::magnitude(v);  // below 2^64, so m * m fits
			detail::add_magnitude(squares_, m * m, false);
		} else {
			squares_.add_square(std::get<double>(x));
		}
	}
	void add(const values& v) { detail::for_each_number(v, [&](const number& x) { add(x); }); }
	void merge(const extended& o) noexcept { stats::merge(o); squares_.merge(o.squares_); }
	double sum_of_squares() const noexcept { return squares_.value(); }
	// The sample variance: (q - s * (s / n)) / (n - 1), with q - s * m rounded once.
	std::optional<double> variance() const noexcept {
		if (count() < 2) { return std::nullopt; }
		const double n = double(count()), s = sum(), m = s / n, p = s * m;
		exact::double_sum t;
		t.add(squares_.value());
		t.add(-p);
		if (std::isfinite(p)) { t.add(-std::fma(s, m, -p)); }
		const double m2 = t.value();
		return (m2 < 0.0 ? 0.0 : m2) / (n - 1);
	}
	std::optional<double> std_deviation() const noexcept {
		const auto v = variance();
		if (!v) { return std::nullopt; }
		return std::sqrt(*v);
	}
	// (lower, upper): avg -/+ sigma standard deviations.
	std::optional<std::pair<double, double>> std_deviation_bounds(double sigma) const noexcept {
		const auto sd = std_deviation();
		if (!sd) { return std::nullopt; }
		const double a = *avg();
		return std::pair{std::fma(-sigma, *sd, a), std::fma(sigma, *sd, a)};
	}

private:
	exact::double_sum squares_;
};

// Exact (value -> count) for `median` and `mode`.
class distribution {
public:
	void add(const number& x) { ++counts_[x]; ++count_; }
	void add(const values& v) { detail::for_each_number(v, [&](const number& x) { add(x); }); }
	void merge(const distribution& o) {
		for (const auto& [x, c] : o.counts_) { counts_[x] += c; }
		count_ += o.count_;
	}
	std::uint64_t count() const noexcept { return count_; }
	std::size_t distinct() const noexcept { return counts_.size(); }
	// The middle value, or the two middle values' mean, rounded once.
	std::optional<double> median() const {
		if (count_ == 0) { return std::nullopt; }
		const std::uint64_t low = (count_ - 1) / 2, high = count_ / 2;
		const number* a = nullptr;
		const number* b = nullptr;
		std::uint64_t seen = 0;
		for (const auto& [x, c] : counts_) {
			if (!a && low < seen + c) { a = &x; }
			if (high < seen + c) { b = &x; break; }
			seen += c;
		}
		if (a == b) { return to_double(*a); }
		exact::double_sum s;
		add_exact(s, *a);
		add_exact(s, *b);
		if (const double whole = s.value(); std::isfinite(whole)) { return whole / 2; }
		exact::double_sum halves;  // two huge doubles: halving each is exact
		halves.add(to_double(*a) / 2);
		halves.add(to_double(*b) / 2);
		return halves.value();
	}
	// The most frequent value; the smallest among ties.
	std::optional<number> mode() const {
		std::optional<number> best;
		std::uint64_t most = 0;
		for (const auto& [x, c] : counts_) {
			if (c > most) { most = c; best = x; }
		}
		return best;
	}
	static constexpr std::size_t node_bytes = detail::node_overhead + sizeof(std::pair<const number, std::uint64_t>);

private:
	std::map<number, std::uint64_t, number_less> counts_;
	std::uint64_t count_ = 0;
};

enum class kind : std::uint8_t {
	count, sum, avg, min, max, stats, extended_stats, variance, std_deviation, median, mode,
	values, terms, histogram, range, filter, root,
};
constexpr bool is_bucket(kind k) noexcept { return k >= kind::values; }
constexpr std::string_view kind_name(kind k) noexcept {
	constexpr std::array<std::string_view, 17> names{
		"count", "sum", "avg", "min", "max", "stats", "extended_stats", "variance", "std_deviation", "median", "mode",
		"values", "terms", "histogram", "range", "filter", "root",
	};
	return names[static_cast<std::size_t>(k)];
}

// [from, to); an empty key becomes "from..to".
struct range_bound {
	std::string key;
	std::optional<number> from, to;
};

struct spec {
	kind type = kind::count;
	std::string name;
	slot_t slot = 0;
	double sigma = 2.0;                                                    // std_deviation bounds
	number interval{std::in_place_type<std::int64_t>, 1};                  // histogram, positive
	number offset{std::in_place_type<std::int64_t>, 0};                    // histogram
	std::vector<range_bound> ranges;                                       // range
	std::function<bool(const document&)> predicate;                        // filter; empty matches all
};

// The aggregation tree, flat: node 0 is the root bucket, every document.
class request {
public:
	static constexpr node_id root = 0;

	request() {
		nodes_.emplace_back();
		nodes_.back().type = kind::root;
		children_.emplace_back();
	}
	node_id add(node_id parent, spec s) {
		if (parent >= nodes_.size() || !is_bucket(nodes_[parent].type)) { throw std::invalid_argument("aggregate: parent is not a bucket aggregation"); }
		if (s.type == kind::root) { throw std::invalid_argument("aggregate: root is implicit"); }
		if (!std::isfinite(s.sigma) || s.sigma < 0) { throw std::invalid_argument("aggregate: sigma must be finite and non-negative"); }
		if (s.type == kind::histogram) {
			const number zero{std::in_place_type<double>, 0.0};
			const bool finite = std::isfinite(to_double(s.interval)) && std::isfinite(to_double(s.offset));
			if (!finite || compare(s.interval, zero) <= 0) { throw std::invalid_argument("aggregate: histogram interval must be positive and finite"); }
			if (auto* d = std::get_if<double>(&s.offset); d && *d == 0.0) { *d = 0.0; }
		}
		if (s.type == kind::range) {
			for (auto& r : s.ranges) {
				const bool nan = (r.from && std::isnan(to_double(*r.from))) || (r.to && std::isnan(to_double(*r.to)));
				if (nan) { throw std::invalid_argument("aggregate: range bound is NaN"); }
				if (r.key.empty()) { r.key = (r.from ? format_number(*r.from) : std::string()) + ".." + (r.to ? format_number(*r.to) : std::string()); }
			}
		}
		const auto id = static_cast<node_id>(nodes_.size());
		nodes_.push_back(std::move(s));
		children_.emplace_back();
		children_[parent].push_back(id);
		return id;
	}
	const spec& at(node_id n) const noexcept { return nodes_[n]; }
	const std::vector<node_id>& children(node_id n) const noexcept { return children_[n]; }
	std::size_t size() const noexcept { return nodes_.size(); }

private:
	std::vector<spec> nodes_;
	std::vector<std::vector<node_id>> children_;
};

// A metric's final values; only those its kind reports are set.
struct metric {
	std::optional<std::uint64_t> count;
	std::optional<number> min, max, mode;
	std::optional<double> avg, sum, sum_of_squares, variance, std_deviation, lower, upper, median;
};

// One line of a final result, in preorder: an aggregation at depth d, its buckets at d + 1,
// and each bucket's sub-aggregations at d + 2. Entry 0 is the root bucket.
struct entry {
	bool bucket = false;
	std::size_t depth = 0;
	node_id node = 0;
	key bucket_key;
	std::uint64_t doc_count = 0;
	metric value;
};

struct bucket_state {
	std::uint64_t doc_count = 0;
	std::size_t children = 0;  // into the partial's children table, one instance per child node
};

// One shard's (or a merge's) state for a request, which must outlive it.
class partial {
public:
	explicit partial(const request& r) : request_(&r) { root_.children = instantiate_children(request::root); }

	void collect(const document& doc) {
		++root_.doc_count;
		work_.clear();
		work_.emplace_back(request::root, root_.children);
		while (!work_.empty()) {
			const auto [owner, first] = work_.back();
			work_.pop_back();
			const auto& kids = request_->children(owner);
			for (std::size_t k = 0; k < kids.size(); ++k) {
				const node_id n = kids[k];
				const spec& s = request_->at(n);
				const std::size_t i = children_[first + k];
				switch (s.type) {
				case kind::count: counts_[i] += detail::value_count(doc.slot(s.slot)); break;
				case kind::min: case kind::max: bounds_[i].add(doc.slot(s.slot)); break;
				case kind::sum: case kind::avg: case kind::stats: stats_[i].add(doc.slot(s.slot)); break;
				case kind::extended_stats: case kind::variance: case kind::std_deviation: extended_[i].add(doc.slot(s.slot)); break;
				case kind::median: case kind::mode: {
					const std::size_t before = distributions_[i].distinct();
					distributions_[i].add(doc.slot(s.slot));
					held_ += (distributions_[i].distinct() - before) * distribution::node_bytes;
					break;
				}
				case kind::values: case kind::terms: {
					detail::distinct_keys(doc.slot(s.slot), keys_);
					for (auto& t : keys_) {
						auto it = terms_[i].buckets.find(t);
						if (it == terms_[i].buckets.end()) {
							const std::size_t c = instantiate_children(n);  // may grow terms_
							held_ += term_node_bytes + detail::heap_bytes(t);
							it = terms_[i].buckets.emplace(std::move(t), bucket_state{0, c}).first;
						}
						++it->second.doc_count;
						work_.emplace_back(n, it->second.children);
					}
					break;
				}
				case kind::histogram: {
					integer_keys_.clear();
					real_keys_.clear();
					__int128 iv = 0, off = 0;
					const bool integer_spec = detail::integral(s.interval, iv) && detail::integral(s.offset, off);
					const double div = to_double(s.interval), shift = to_double(s.offset);
					detail::for_each_number(doc.slot(s.slot), [&](const number& x) {
						__int128 v = 0;
						if (integer_spec && detail::integral(x, v)) {
							integer_keys_.push_back(detail::floor_div(v - off, iv));
						} else {
							double q = std::floor((to_double(x) - shift) / div);
							if (q == 0.0) { q = 0.0; }
							real_keys_.push_back(q);
						}
					});
					std::sort(integer_keys_.begin(), integer_keys_.end());
					integer_keys_.erase(std::unique(integer_keys_.begin(), integer_keys_.end()), integer_keys_.end());
					std::sort(real_keys_.begin(), real_keys_.end());
					real_keys_.erase(std::unique(real_keys_.begin(), real_keys_.end()), real_keys_.end());
					for (const __int128 q : integer_keys_) {
						auto it = histograms_[i].integers.find(q);
						if (it == histograms_[i].integers.end()) {
							const std::size_t c = instantiate_children(n);
							held_ += integer_node_bytes;
							it = histograms_[i].integers.emplace(q, bucket_state{0, c}).first;
						}
						++it->second.doc_count;
						work_.emplace_back(n, it->second.children);
					}
					for (const double q : real_keys_) {
						auto it = histograms_[i].reals.find(q);
						if (it == histograms_[i].reals.end()) {
							const std::size_t c = instantiate_children(n);
							held_ += real_node_bytes;
							it = histograms_[i].reals.emplace(q, bucket_state{0, c}).first;
						}
						++it->second.doc_count;
						work_.emplace_back(n, it->second.children);
					}
					break;
				}
				case kind::range: {
					const values v = doc.slot(s.slot);
					for (std::size_t r = 0; r < s.ranges.size(); ++r) {
						const range_bound& bound = s.ranges[r];
						bool hit = false;
						detail::for_each_number(v, [&](const number& x) {
							hit = hit || ((!bound.from || compare(x, *bound.from) >= 0) && (!bound.to || compare(x, *bound.to) < 0));
						});
						if (hit) {
							auto& b = ranges_[i].buckets[r];
							++b.doc_count;
							work_.emplace_back(n, b.children);
						}
					}
					break;
				}
				case kind::filter:
					if (!s.predicate || s.predicate(doc)) {
						++filters_[i].doc_count;
						work_.emplace_back(n, filters_[i].children);
					}
					break;
				case kind::root: break;
				}
			}
		}
	}

	// Adds other's state; other must come from the same request.
	void merge(const partial& other) {
		if (&other == this) {
			const partial copy(other);
			merge(copy);
			return;
		}
		if (other.request_ != request_) { throw std::invalid_argument("aggregate: partials of different requests"); }
		struct step { node_id node; std::size_t into, from; };
		std::vector<step> stack;
		auto children = [&](node_id owner, std::size_t into, std::size_t from) {
			const auto& kids = request_->children(owner);
			for (std::size_t k = 0; k < kids.size(); ++k) { stack.push_back(step{kids[k], children_[into + k], other.children_[from + k]}); }
		};
		root_.doc_count += other.root_.doc_count;
		children(request::root, root_.children, other.root_.children);
		while (!stack.empty()) {
			const step p = stack.back();
			stack.pop_back();
			const spec& s = request_->at(p.node);
			switch (s.type) {
			case kind::count: counts_[p.into] += other.counts_[p.from]; break;
			case kind::min: case kind::max: bounds_[p.into].merge(other.bounds_[p.from]); break;
			case kind::sum: case kind::avg: case kind::stats: stats_[p.into].merge(other.stats_[p.from]); break;
			case kind::extended_stats: case kind::variance: case kind::std_deviation: extended_[p.into].merge(other.extended_[p.from]); break;
			case kind::median: case kind::mode: {
				const std::size_t before = distributions_[p.into].distinct();
				distributions_[p.into].merge(other.distributions_[p.from]);
				held_ += (distributions_[p.into].distinct() - before) * distribution::node_bytes;
				break;
			}
			case kind::values: case kind::terms:
				for (const auto& [t, b] : other.terms_[p.from].buckets) {
					auto it = terms_[p.into].buckets.find(t);
					if (it == terms_[p.into].buckets.end()) {
						const std::size_t c = instantiate_children(p.node);  // merging into empty copies
						held_ += term_node_bytes + detail::heap_bytes(t);
						it = terms_[p.into].buckets.emplace(t, bucket_state{0, c}).first;
					}
					it->second.doc_count += b.doc_count;
					children(p.node, it->second.children, b.children);
				}
				break;
			case kind::histogram:
				for (const auto& [q, b] : other.histograms_[p.from].integers) {
					auto it = histograms_[p.into].integers.find(q);
					if (it == histograms_[p.into].integers.end()) {
						const std::size_t c = instantiate_children(p.node);
						held_ += integer_node_bytes;
						it = histograms_[p.into].integers.emplace(q, bucket_state{0, c}).first;
					}
					it->second.doc_count += b.doc_count;
					children(p.node, it->second.children, b.children);
				}
				for (const auto& [q, b] : other.histograms_[p.from].reals) {
					auto it = histograms_[p.into].reals.find(q);
					if (it == histograms_[p.into].reals.end()) {
						const std::size_t c = instantiate_children(p.node);
						held_ += real_node_bytes;
						it = histograms_[p.into].reals.emplace(q, bucket_state{0, c}).first;
					}
					it->second.doc_count += b.doc_count;
					children(p.node, it->second.children, b.children);
				}
				break;
			case kind::range:
				for (std::size_t r = 0; r < s.ranges.size(); ++r) {
					auto& b = ranges_[p.into].buckets[r];
					const auto& o = other.ranges_[p.from].buckets[r];
					b.doc_count += o.doc_count;
					children(p.node, b.children, o.children);
				}
				break;
			case kind::filter:
				filters_[p.into].doc_count += other.filters_[p.from].doc_count;
				children(p.node, filters_[p.into].children, other.filters_[p.from].children);
				break;
			case kind::root: break;
			}
		}
	}

	// The final result, in preorder (see entry); buckets in key order, ranges as declared.
	std::vector<entry> finish() const {
		struct item {
			node_id node;
			std::size_t index, depth;  // a pool index, or a bucket's children
			bool bucket;
			key k;
			std::uint64_t doc_count;
		};
		std::vector<entry> out;
		std::vector<item> stack, buckets;
		auto push_children = [&](node_id owner, std::size_t first, std::size_t depth) {
			const auto& kids = request_->children(owner);
			for (std::size_t k = kids.size(); k-- > 0;) { stack.push_back(item{kids[k], children_[first + k], depth, false, key{}, 0}); }
		};
		entry top;
		top.bucket = true;
		top.node = request::root;
		top.doc_count = root_.doc_count;
		out.push_back(std::move(top));
		push_children(request::root, root_.children, 1);
		while (!stack.empty()) {
			item it = std::move(stack.back());
			stack.pop_back();
			if (it.bucket) {
				entry e;
				e.bucket = true;
				e.depth = it.depth;
				e.node = it.node;
				e.bucket_key = std::move(it.k);
				e.doc_count = it.doc_count;
				out.push_back(std::move(e));
				push_children(it.node, it.index, it.depth + 1);
				continue;
			}
			const spec& s = request_->at(it.node);
			const std::size_t i = it.index;
			entry e;
			e.depth = it.depth;
			e.node = it.node;
			metric& m = e.value;
			auto deviation = [&](const extended& x) {
				m.std_deviation = x.std_deviation();
				if (const auto b = x.std_deviation_bounds(s.sigma)) { m.lower = b->first; m.upper = b->second; }
			};
			buckets.clear();
			auto push_bucket = [&](key k, const bucket_state& b) {
				buckets.push_back(item{it.node, b.children, it.depth + 1, true, std::move(k), b.doc_count});
			};
			switch (s.type) {
			case kind::count: m.count = counts_[i]; break;
			case kind::min: m.min = bounds_[i].min(); break;
			case kind::max: m.max = bounds_[i].max(); break;
			case kind::sum: m.sum = stats_[i].sum(); break;
			case kind::avg: m.avg = stats_[i].avg(); break;
			case kind::stats: {
				const stats& x = stats_[i];
				m.count = x.count(); m.min = x.min(); m.max = x.max(); m.avg = x.avg(); m.sum = x.sum();
				break;
			}
			case kind::extended_stats: {
				const extended& x = extended_[i];
				m.count = x.count(); m.min = x.min(); m.max = x.max(); m.avg = x.avg(); m.sum = x.sum();
				m.sum_of_squares = x.sum_of_squares(); m.variance = x.variance();
				deviation(x);
				break;
			}
			case kind::variance: m.variance = extended_[i].variance(); break;
			case kind::std_deviation: deviation(extended_[i]); break;
			case kind::median: m.median = distributions_[i].median(); break;
			case kind::mode: m.mode = distributions_[i].mode(); break;
			case kind::values: case kind::terms:
				for (const auto& [t, b] : terms_[i].buckets) { push_bucket(t, b); }
				break;
			case kind::histogram: {
				__int128 iv = 0, off = 0;
				detail::integral(s.interval, iv);
				detail::integral(s.offset, off);
				for (const auto& [q, b] : histograms_[i].integers) { push_bucket(detail::integer_key(q * iv + off), b); }
				const double div = to_double(s.interval), shift = to_double(s.offset);
				for (const auto& [q, b] : histograms_[i].reals) {
					double edge = std::fma(q, div, shift);
					if (edge == 0.0) { edge = 0.0; }
					push_bucket(key(std::in_place_type<double>, edge), b);
				}
				break;
			}
			case kind::range:
				for (std::size_t r = 0; r < s.ranges.size(); ++r) { push_bucket(key(std::in_place_type<std::string>, s.ranges[r].key), ranges_[i].buckets[r]); }
				break;
			case kind::filter: push_bucket(key{}, filters_[i]); break;
			case kind::root: break;
			}
			out.push_back(std::move(e));
			for (auto b = buckets.rbegin(); b != buckets.rend(); ++b) { stack.push_back(std::move(*b)); }
		}
		return out;
	}

	std::uint64_t doc_count() const noexcept { return root_.doc_count; }

	// Approximate bytes held, for a query budget to charge.
	std::size_t bytes() const noexcept {
		return sizeof(*this) + held_
			+ counts_.capacity() * sizeof(std::uint64_t) + bounds_.capacity() * sizeof(bounds)
			+ stats_.capacity() * sizeof(stats) + extended_.capacity() * sizeof(extended)
			+ distributions_.capacity() * sizeof(distribution) + terms_.capacity() * sizeof(terms_state)
			+ histograms_.capacity() * sizeof(histogram_state) + ranges_.capacity() * sizeof(range_state)
			+ filters_.capacity() * sizeof(bucket_state) + children_.capacity() * sizeof(std::size_t)
			+ (work_.capacity() * sizeof(std::pair<node_id, std::size_t>)) + keys_.capacity() * sizeof(key)
			+ integer_keys_.capacity() * sizeof(__int128) + real_keys_.capacity() * sizeof(double);
	}

private:
	struct terms_state { std::map<key, bucket_state> buckets; };
	struct histogram_state {
		std::map<__int128, bucket_state> integers;  // bucket numbers in the integer domain
		std::map<double, bucket_state> reals;       // floor((x - offset) / interval)
	};
	struct range_state { std::vector<bucket_state> buckets; };

	static constexpr std::size_t term_node_bytes = detail::node_overhead + sizeof(std::pair<const key, bucket_state>);
	static constexpr std::size_t integer_node_bytes = detail::node_overhead + sizeof(std::pair<const __int128, bucket_state>);
	static constexpr std::size_t real_node_bytes = detail::node_overhead + sizeof(std::pair<const double, bucket_state>);

	// A fresh, empty instance of n.
	std::size_t create(node_id n) {
		const spec& s = request_->at(n);
		switch (s.type) {
		case kind::count: counts_.push_back(0); return counts_.size() - 1;
		case kind::min: case kind::max: bounds_.emplace_back(); return bounds_.size() - 1;
		case kind::sum: case kind::avg: case kind::stats: stats_.emplace_back(); return stats_.size() - 1;
		case kind::extended_stats: case kind::variance: case kind::std_deviation: extended_.emplace_back(); return extended_.size() - 1;
		case kind::median: case kind::mode: distributions_.emplace_back(); return distributions_.size() - 1;
		case kind::values: case kind::terms: terms_.emplace_back(); return terms_.size() - 1;
		case kind::histogram: histograms_.emplace_back(); return histograms_.size() - 1;
		case kind::range:
			ranges_.push_back(range_state{std::vector<bucket_state>(s.ranges.size())});
			held_ += s.ranges.size() * sizeof(bucket_state);
			return ranges_.size() - 1;
		case kind::filter: filters_.emplace_back(); return filters_.size() - 1;
		case kind::root: break;
		}
		return 0;
	}

	// One fresh instance per child of owner, contiguous in children_; returns the first.
	std::size_t add_children(node_id owner, std::vector<std::pair<node_id, std::size_t>>& pending) {
		const auto& kids = request_->children(owner);
		const std::size_t first = children_.size();
		children_.resize(first + kids.size());
		for (std::size_t k = 0; k < kids.size(); ++k) {
			const std::size_t i = create(kids[k]);
			children_[first + k] = i;
			pending.emplace_back(kids[k], i);
		}
		return first;
	}

	// A bucket's children for owner; filters' and ranges' fixed buckets below get theirs too.
	std::size_t instantiate_children(node_id owner) {
		std::vector<std::pair<node_id, std::size_t>> pending;
		const std::size_t first = add_children(owner, pending);
		while (!pending.empty()) {
			const auto [n, i] = pending.back();
			pending.pop_back();
			const spec& s = request_->at(n);
			if (s.type == kind::filter) {
				const std::size_t c = add_children(n, pending);
				filters_[i].children = c;
			} else if (s.type == kind::range) {
				for (std::size_t r = 0; r < s.ranges.size(); ++r) {
					const std::size_t c = add_children(n, pending);
					ranges_[i].buckets[r].children = c;
				}
			}
		}
		return first;
	}

	const request* request_;
	bucket_state root_;
	std::vector<std::uint64_t> counts_;
	std::vector<bounds> bounds_;
	std::vector<stats> stats_;
	std::vector<extended> extended_;
	std::vector<distribution> distributions_;
	std::vector<terms_state> terms_;
	std::vector<histogram_state> histograms_;
	std::vector<range_state> ranges_;
	std::vector<bucket_state> filters_;
	std::vector<std::size_t> children_;
	std::size_t held_ = 0;
	// Scratch for collect.
	std::vector<std::pair<node_id, std::size_t>> work_;
	std::vector<key> keys_;
	std::vector<__int128> integer_keys_;
	std::vector<double> real_keys_;
};

// A canonical text form of a result: one line per entry, its depth first; doubles in the
// shortest form that reads back exactly.
inline std::string render(const request& r, const std::vector<entry>& entries) {
	std::string out;
	for (const entry& e : entries) {
		out += std::to_string(e.depth);
		if (e.bucket) {
			if (e.depth != 0) { out += " ["; out += format_key(e.bucket_key); out += ']'; }
			out += " _doc_count=";
			out += std::to_string(e.doc_count);
		} else {
			const spec& s = r.at(e.node);
			out += ' '; out += s.name; out += " ("; out += kind_name(s.type); out += ')';
			auto field = [&](std::string_view name, const std::string& v) { out += ' '; out += name; out += '='; out += v; };
			const metric& m = e.value;
			if (m.count) { field("_count", std::to_string(*m.count)); }
			if (m.min) { field("_min", format_number(*m.min)); }
			if (m.max) { field("_max", format_number(*m.max)); }
			if (m.avg) { field("_avg", format_double(*m.avg)); }
			if (m.sum) { field("_sum", format_double(*m.sum)); }
			if (m.sum_of_squares) { field("_sum_of_squares", format_double(*m.sum_of_squares)); }
			if (m.variance) { field("_variance", format_double(*m.variance)); }
			if (m.std_deviation) { field("_std_deviation", format_double(*m.std_deviation)); }
			if (m.upper) { field("_upper", format_double(*m.upper)); }
			if (m.lower) { field("_lower", format_double(*m.lower)); }
			if (m.median) { field("_median", format_double(*m.median)); }
			if (m.mode) { field("_mode", format_number(*m.mode)); }
		}
		out += '\n';
	}
	return out;
}

}  // namespace strata::aggregate
