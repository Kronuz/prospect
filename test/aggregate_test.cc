// Aggregation collectors (Phase 4, D11): every metric and bucket collector against brute
// force over random multi-valued documents (negatives, duplicates, extremes, and doubles
// with heavy cancellation); one partial and any split into 1 to 64 partials, merged in any
// order or tree, render byte for byte alike; nesting 2,000 deep runs on work stacks; empty
// inputs report empty results.
#include "strata/search/aggregate.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agg = strata::aggregate;

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

// Shewchuk's exact partials with a correctly rounded final sum, for finite inputs.
static double fsum(const std::vector<double>& xs) {
	std::vector<double> partials;
	for (double x : xs) {
		std::size_t i = 0;
		for (double y : partials) {
			if (std::fabs(x) < std::fabs(y)) { std::swap(x, y); }
			const double high = x + y, low = y - (high - x);
			if (low != 0.0) { partials[i++] = low; }
			x = high;
		}
		partials.resize(i); partials.push_back(x);
	}
	double high = 0.0;
	if (!partials.empty()) {
		std::size_t n = partials.size(); high = partials[--n]; double low = 0.0;
		while (n > 0) {
			const double x = high; const double y = partials[--n];
			high = x + y; const double yr = high - x; low = y - yr;
			if (low != 0.0) { break; }
		}
		if (n > 0 && ((low < 0 && partials[n - 1] < 0) || (low > 0 && partials[n - 1] > 0))) {
			const double y = low * 2; const double x = high + y; const double yr = x - high;
			if (y == yr) { high = x; }
		}
	}
	return high;
}

static bool same(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b) || (a == 0 && b == 0); }

enum : agg::slot_t { I64 = 0, U64 = 1, F64 = 2, BYTES = 3 };

static const std::array<std::string, 6> vocabulary{"a", "b", "c", "", "a term longer than any small-string buffer", "z"};

struct doc final : agg::document {
	std::vector<std::int64_t> i;
	std::vector<std::uint64_t> u;
	std::vector<double> d;
	std::vector<std::string_view> b;  // into vocabulary
	agg::values slot(agg::slot_t s) const override {
		switch (s) {
		case I64: return std::span<const std::int64_t>(i);
		case U64: return std::span<const std::uint64_t>(u);
		case F64: return std::span<const double>(d);
		case BYTES: return std::span<const std::string_view>(b);
		default: return std::span<const std::int64_t>();
		}
	}
	bool has(std::string_view t) const { return std::find(b.begin(), b.end(), t) != b.end(); }
};

static std::vector<doc> make_docs(std::mt19937_64& rng, std::size_t n) {
	using i64 = std::numeric_limits<std::int64_t>;
	auto random_double = [&](int spread) {
		const double mantissa = std::ldexp(double(rng() >> 11), -53) + 0.5;
		return (rng() & 1 ? -1.0 : 1.0) * std::ldexp(mantissa, int(rng() % (2 * spread + 1)) - spread);
	};
	static constexpr double small[] = {0.1, -0.1, 0.25, -0.0, 0.0, 1.5, -1.0, 100.0};
	std::vector<double> seen;
	std::vector<doc> docs(n);
	for (auto& x : docs) {
		for (auto k = rng() % 5; k > 0; --k) {
			const auto r = rng() % 16;
			x.i.push_back(r == 0 ? i64::min() : r == 1 ? i64::max() : std::int64_t(rng() % 101) - 50);
		}
		for (auto k = rng() % 5; k > 0; --k) {
			const auto r = rng() % 16;
			x.u.push_back(r == 0 ? std::numeric_limits<std::uint64_t>::max() : r == 1 ? std::uint64_t(1) << 63 : std::uint64_t(rng() % 60));
		}
		for (auto k = rng() % 5; k > 0; --k) {
			const auto r = rng() % 4;
			// Negating an earlier value makes sums cancel heavily.
			const double v = r == 0 ? small[rng() % 8] : r == 1 && !seen.empty() ? -seen[rng() % seen.size()] : random_double(60);
			x.d.push_back(v);
			seen.push_back(v);
		}
		for (auto k = rng() % 4; k > 0; --k) { x.b.push_back(vocabulary[rng() % vocabulary.size()]); }
		// Duplicates within a document.
		if (rng() % 6 == 0 && !x.i.empty()) { x.i.push_back(x.i.front()); }
		if (rng() % 6 == 0 && !x.u.empty()) { x.u.push_back(x.u.front()); }
		if (rng() % 6 == 0 && !x.d.empty()) { x.d.push_back(x.d.front()); }
	}
	return docs;
}

static std::vector<agg::number> numbers_of(const doc& x, agg::slot_t s) {
	std::vector<agg::number> out;
	if (s == I64) { for (auto v : x.i) { out.emplace_back(std::in_place_type<std::int64_t>, v); } }
	if (s == U64) { for (auto v : x.u) { out.emplace_back(std::in_place_type<std::uint64_t>, v); } }
	if (s == F64) { for (double v : x.d) { out.emplace_back(std::in_place_type<double>, v == 0.0 ? 0.0 : v); } }
	return out;
}

static __int128 as_int128(const agg::number& x) {
	return std::holds_alternative<std::int64_t>(x) ? __int128(std::get<std::int64_t>(x)) : __int128(std::get<std::uint64_t>(x));
}

struct oracle {
	std::uint64_t count = 0;
	double sum = 0.0, sum_of_squares = 0.0, variance = 0.0, scale = 0.0;
	std::optional<agg::number> min, max, mode;
	std::optional<double> median;
};

// Metrics the slow way: Shewchuk sums, a sort, and a two-pass variance in long double.
static oracle brute(std::vector<agg::number> xs) {
	oracle o;
	o.count = xs.size();
	__int128 integer = 0;
	bool reals_seen = false;
	std::vector<double> reals, squares;
	long double total = 0;
	auto wide = [](const agg::number& x) { return std::visit([](auto v) { return static_cast<long double>(v); }, x); };
	for (const auto& x : xs) {
		if (const auto* p = std::get_if<double>(&x)) {
			reals_seen = true;
			reals.push_back(*p);
			const double h = *p * *p;
			squares.push_back(h);
			squares.push_back(std::fma(*p, *p, -h));
		} else {
			const __int128 v = as_int128(x);
			integer += v;
			unsigned __int128 m = v < 0 ? -static_cast<unsigned __int128>(v) : static_cast<unsigned __int128>(v);
			m *= m;
			for (int j = 0; j < 4; ++j) { squares.push_back(std::ldexp(double(std::uint32_t(m >> (32 * j))), 32 * j)); }
		}
		total += wide(x);
	}
	o.sum = reals_seen ? fsum(reals) : static_cast<double>(integer);
	o.sum_of_squares = fsum(squares);
	if (o.count >= 2) {
		const long double mean = total / static_cast<long double>(o.count);
		long double m2 = 0;
		for (const auto& x : xs) { const long double dv = wide(x) - mean; m2 += dv * dv; }
		o.variance = static_cast<double>(m2 / static_cast<long double>(o.count - 1));
		o.scale = o.sum_of_squares / double(o.count);
	}
	std::sort(xs.begin(), xs.end(), agg::number_less{});
	if (!xs.empty()) {
		o.min = xs.front();
		o.max = xs.back();
		const auto& a = xs[(xs.size() - 1) / 2];
		const auto& b = xs[xs.size() / 2];
		if (a == b) {
			o.median = agg::to_double(a);
		} else if (std::holds_alternative<double>(a) && std::holds_alternative<double>(b)) {
			o.median = (std::get<double>(a) + std::get<double>(b)) / 2;
		} else {
			o.median = static_cast<double>(as_int128(a) + as_int128(b)) / 2;
		}
		std::size_t best = 0;
		for (std::size_t j = 0; j < xs.size();) {
			std::size_t k = j;
			while (k < xs.size() && xs[k] == xs[j]) { ++k; }
			if (k - j > best) { best = k - j; o.mode = xs[j]; }
			j = k;
		}
	}
	return o;
}

static std::size_t top_of(const std::vector<agg::entry>& es, agg::node_id n) {
	for (std::size_t j = 0; j < es.size(); ++j) {
		if (!es[j].bucket && es[j].node == n && es[j].depth == 1) { return j; }
	}
	return es.size();
}
// The buckets of aggregation entry a.
static std::vector<std::size_t> buckets_of(const std::vector<agg::entry>& es, std::size_t a) {
	std::vector<std::size_t> out;
	if (a >= es.size()) { return out; }
	for (std::size_t j = a + 1; j < es.size() && es[j].depth > es[a].depth; ++j) {
		if (es[j].depth == es[a].depth + 1) { out.push_back(j); }
	}
	return out;
}
// Bucket b's sub-aggregation for node n.
static std::size_t sub_of(const std::vector<agg::entry>& es, std::size_t b, agg::node_id n) {
	if (b >= es.size()) { return es.size(); }
	for (std::size_t j = b + 1; j < es.size() && es[j].depth > es[b].depth; ++j) {
		if (es[j].depth == es[b].depth + 1 && es[j].node == n) { return j; }
	}
	return es.size();
}

static const agg::metric& metric_at(const std::vector<agg::entry>& es, std::size_t j) {
	static const agg::metric none;
	CHECK(j < es.size());
	return j < es.size() ? es[j].value : none;
}

static constexpr std::array<agg::kind, 11> metric_kinds{
	agg::kind::count, agg::kind::sum, agg::kind::avg, agg::kind::min, agg::kind::max, agg::kind::stats,
	agg::kind::extended_stats, agg::kind::variance, agg::kind::std_deviation, agg::kind::median, agg::kind::mode,
};

static void check_metric(const agg::metric& m, agg::kind k, const oracle& o, double sigma) {
	const bool has = o.count > 0, spread = o.count > 1;
	auto near = [](double got, double want, double slack) { return std::fabs(got - want) <= 1e-6 * std::fabs(want) + slack; };
	const double var_slack = 1e-12 * o.scale, sd = std::sqrt(o.variance), sd_slack = 1e-5 * std::sqrt(o.scale);
	const double avg = has ? o.sum / double(o.count) : 0.0;
	const bool counted = k == agg::kind::count || k == agg::kind::stats || k == agg::kind::extended_stats;
	const bool summed = k == agg::kind::sum || k == agg::kind::stats || k == agg::kind::extended_stats;
	const bool averaged = k == agg::kind::avg || k == agg::kind::stats || k == agg::kind::extended_stats;
	const bool bounded = k == agg::kind::min || k == agg::kind::max || k == agg::kind::stats || k == agg::kind::extended_stats;
	const bool varied = k == agg::kind::variance || k == agg::kind::extended_stats;
	const bool deviated = k == agg::kind::std_deviation || k == agg::kind::extended_stats;
	if (counted) { CHECK(m.count == o.count); } else { CHECK(!m.count); }
	if (summed) { CHECK(m.sum && same(*m.sum, o.sum)); } else { CHECK(!m.sum); }
	if (averaged && has) { CHECK(m.avg && same(*m.avg, avg)); } else { CHECK(!m.avg); }
	if (bounded && k != agg::kind::max) { CHECK(m.min == o.min); } else { CHECK(!m.min); }
	if (bounded && k != agg::kind::min) { CHECK(m.max == o.max); } else { CHECK(!m.max); }
	if (k == agg::kind::extended_stats) { CHECK(m.sum_of_squares && same(*m.sum_of_squares, o.sum_of_squares)); } else { CHECK(!m.sum_of_squares); }
	if (varied && spread) { CHECK(m.variance && near(*m.variance, o.variance, var_slack)); } else { CHECK(!m.variance); }
	if (deviated && spread) {
		CHECK(m.std_deviation && near(*m.std_deviation, sd, sd_slack));
		const double slack = sigma * (1e-6 * sd + sd_slack) + 1e-12 * std::fabs(avg);
		CHECK(m.upper && near(*m.upper, avg + sigma * sd, slack));
		CHECK(m.lower && near(*m.lower, avg - sigma * sd, slack));
	} else {
		CHECK(!m.std_deviation && !m.upper && !m.lower);
	}
	if (k == agg::kind::median && has) { CHECK(m.median && same(*m.median, *o.median)); } else { CHECK(!m.median); }
	if (k == agg::kind::mode) { CHECK(m.mode == o.mode); } else { CHECK(!m.mode); }
}

static agg::spec make(agg::kind k, std::string name, agg::slot_t slot) {
	agg::spec s;
	s.type = k;
	s.name = std::move(name);
	s.slot = slot;
	return s;
}

// Every split into 1 to 64 partials, merged in a line or a random tree, renders as want.
static void check_splits(const agg::request& req, const std::vector<doc>& docs, std::mt19937_64& rng, const std::string& want, int rounds) {
	for (int round = 0; round < rounds; ++round) {
		const std::size_t shards = 1 + rng() % 64;
		std::vector<agg::partial> parts(shards, agg::partial(req));
		for (const auto& x : docs) { parts[rng() % shards].collect(x); }
		std::shuffle(parts.begin(), parts.end(), rng);
		if (round % 2 == 0) {
			agg::partial merged(req);
			for (const auto& p : parts) { merged.merge(p); }
			CHECK(agg::render(req, merged.finish()) == want);
		} else {
			while (parts.size() > 1) {
				const std::size_t a = rng() % parts.size();
				std::size_t b = rng() % (parts.size() - 1);
				if (b >= a) { ++b; }
				parts[a].merge(parts[b]);
				parts.erase(parts.begin() + std::ptrdiff_t(b));
			}
			CHECK(agg::render(req, parts.front().finish()) == want);
		}
	}
}

// Every metric over each numeric slot, and a value count over bytes.
static void metrics(std::uint64_t seed, std::size_t n) {
	std::mt19937_64 rng(seed);
	const auto docs = make_docs(rng, n);
	agg::request req;
	std::array<std::array<agg::node_id, metric_kinds.size()>, 3> ids{};
	for (agg::slot_t s = I64; s <= F64; ++s) {
		for (std::size_t k = 0; k < metric_kinds.size(); ++k) {
			ids[s][k] = req.add(agg::request::root, make(metric_kinds[k], std::string(agg::kind_name(metric_kinds[k])) + std::to_string(s), s));
		}
	}
	const auto bytes_count = req.add(agg::request::root, make(agg::kind::count, "bytes", BYTES));
	agg::partial whole(req);
	for (const auto& x : docs) { whole.collect(x); }
	const auto es = whole.finish();
	CHECK(es[0].bucket && es[0].doc_count == docs.size());
	for (agg::slot_t s = I64; s <= F64; ++s) {
		std::vector<agg::number> all;
		for (const auto& x : docs) { const auto v = numbers_of(x, s); all.insert(all.end(), v.begin(), v.end()); }
		const oracle o = brute(all);
		for (std::size_t k = 0; k < metric_kinds.size(); ++k) { check_metric(metric_at(es, top_of(es, ids[s][k])), metric_kinds[k], o, 2.0); }
	}
	std::uint64_t terms = 0;
	for (const auto& x : docs) { terms += x.b.size(); }
	CHECK(metric_at(es, top_of(es, bytes_count)).count == terms);
	check_splits(req, docs, rng, agg::render(req, es), 6);
}

// Buckets with sub-aggregations, against brute force.
static void buckets(std::uint64_t seed, std::size_t n) {
	std::mt19937_64 rng(seed);
	const auto docs = make_docs(rng, n);
	agg::request req;
	const auto t = req.add(agg::request::root, make(agg::kind::terms, "t", BYTES));
	const auto t_stats = req.add(t, make(agg::kind::stats, "t_stats", F64));
	const auto v = req.add(agg::request::root, make(agg::kind::values, "v", I64));
	auto h_spec = make(agg::kind::histogram, "h", I64);
	h_spec.interval = std::int64_t(7);
	h_spec.offset = std::int64_t(3);
	const auto h = req.add(agg::request::root, std::move(h_spec));
	const auto h_sum = req.add(h, make(agg::kind::sum, "h_sum", U64));
	auto hd_spec = make(agg::kind::histogram, "hd", F64);
	hd_spec.interval = 0.25;
	hd_spec.offset = 0.1;
	const auto hd = req.add(agg::request::root, std::move(hd_spec));
	auto r_spec = make(agg::kind::range, "r", F64);
	r_spec.ranges.push_back({"", std::nullopt, agg::number(-1.0)});
	r_spec.ranges.push_back({"", agg::number(-1.0), agg::number(1.5)});
	r_spec.ranges.push_back({"", agg::number(1.5), std::nullopt});
	r_spec.ranges.push_back({"", agg::number(std::int64_t(0)), agg::number(std::int64_t(100))});
	const auto r = req.add(agg::request::root, std::move(r_spec));
	const auto r_terms = req.add(r, make(agg::kind::terms, "r_terms", I64));
	req.add(r_terms, make(agg::kind::extended_stats, "r_ext", F64));
	auto f_spec = make(agg::kind::filter, "f", 0);
	f_spec.predicate = [](const agg::document& d) { return static_cast<const doc&>(d).has("a"); };
	const auto f = req.add(agg::request::root, std::move(f_spec));
	const auto f_stats = req.add(f, make(agg::kind::stats, "f_stats", I64));
	req.add(f, make(agg::kind::values, "f_values", U64));
	const auto f_median = req.add(f, make(agg::kind::median, "f_median", F64));

	agg::partial whole(req);
	for (const auto& x : docs) { whole.collect(x); }
	const auto es = whole.finish();
	CHECK(es[0].doc_count == docs.size());

	// Terms over bytes, each bucket's stats over the documents' doubles.
	{
		std::map<std::string, std::pair<std::uint64_t, std::vector<agg::number>>> want;
		for (const auto& x : docs) {
			const std::set<std::string_view> distinct(x.b.begin(), x.b.end());
			for (const auto term : distinct) {
				auto& w = want[std::string(term)];
				++w.first;
				const auto ds = numbers_of(x, F64);
				w.second.insert(w.second.end(), ds.begin(), ds.end());
			}
		}
		const auto bs = buckets_of(es, top_of(es, t));
		CHECK(bs.size() == want.size());
		std::size_t j = 0;
		for (const auto& [term, w] : want) {
			if (j >= bs.size()) { break; }
			const std::size_t b = bs[j++];
			CHECK(es[b].bucket_key == agg::key(std::in_place_type<std::string>, term));
			CHECK(es[b].doc_count == w.first);
			check_metric(metric_at(es, sub_of(es, b, t_stats)), agg::kind::stats, brute(w.second), 2.0);
		}
	}
	// Values over integers: a document counts once per distinct value.
	{
		std::map<std::int64_t, std::uint64_t> want;
		for (const auto& x : docs) {
			for (const auto value : std::set<std::int64_t>(x.i.begin(), x.i.end())) { ++want[value]; }
		}
		const auto bs = buckets_of(es, top_of(es, v));
		CHECK(bs.size() == want.size());
		std::size_t j = 0;
		for (const auto& [value, count] : want) {
			if (j >= bs.size()) { break; }
			const std::size_t b = bs[j++];
			CHECK(es[b].bucket_key == agg::key(std::in_place_type<std::int64_t>, value));
			CHECK(es[b].doc_count == count);
		}
	}
	// Integer histogram: floor division by remainder, edges below int64 reported as doubles.
	{
		std::map<__int128, std::pair<std::uint64_t, __int128>> want;
		for (const auto& x : docs) {
			std::set<__int128> edges;
			for (const auto value : x.i) {
				const __int128 a = __int128(value) - 3, rem = ((a % 7) + 7) % 7;
				edges.insert(a - rem + 3);
			}
			__int128 usum = 0;
			for (const auto value : x.u) { usum += value; }
			for (const auto edge : edges) { auto& w = want[edge]; ++w.first; w.second += usum; }
		}
		const auto bs = buckets_of(es, top_of(es, h));
		CHECK(bs.size() == want.size());
		std::size_t j = 0;
		for (const auto& [edge, w] : want) {
			if (j >= bs.size()) { break; }
			const std::size_t b = bs[j++];
			const bool fits = edge >= std::numeric_limits<std::int64_t>::min() && edge <= std::numeric_limits<std::int64_t>::max();
			const agg::key expected = fits ? agg::key(std::in_place_type<std::int64_t>, std::int64_t(edge)) : agg::key(std::in_place_type<double>, static_cast<double>(edge));
			CHECK(es[b].bucket_key == expected);
			CHECK(es[b].doc_count == w.first);
			const auto& m = metric_at(es, sub_of(es, b, h_sum));
			CHECK(m.sum && same(*m.sum, static_cast<double>(w.second)));
		}
	}
	// Fractional histogram: floor((x - 0.1) / 0.25), keyed by bucket number.
	{
		std::map<double, std::uint64_t> want;
		for (const auto& x : docs) {
			std::set<double> qs;
			for (double value : x.d) {
				double q = std::floor(((value == 0.0 ? 0.0 : value) - 0.1) / 0.25);
				qs.insert(q == 0.0 ? 0.0 : q);
			}
			for (const double q : qs) { ++want[q]; }
		}
		const auto bs = buckets_of(es, top_of(es, hd));
		CHECK(bs.size() == want.size());
		std::size_t j = 0;
		for (const auto& [q, count] : want) {
			if (j >= bs.size()) { break; }
			const std::size_t b = bs[j++];
			double edge = std::fma(q, 0.25, 0.1);
			if (edge == 0.0) { edge = 0.0; }
			CHECK(es[b].bucket_key == agg::key(std::in_place_type<double>, edge));
			CHECK(es[b].doc_count == count);
		}
	}
	// Ranges, in declared order with generated names, each with terms below.
	{
		const std::array<std::string, 4> names{"..-1", "-1..1.5", "1.5..", "0..100"};
		auto in = [](std::size_t range, double x) {
			switch (range) {
			case 0: return x < -1.0;
			case 1: return x >= -1.0 && x < 1.5;
			case 2: return x >= 1.5;
			default: return x >= 0.0 && x < 100.0;
			}
		};
		const auto bs = buckets_of(es, top_of(es, r));
		CHECK(bs.size() == names.size());
		for (std::size_t range = 0; range < names.size() && range < bs.size(); ++range) {
			std::uint64_t count = 0;
			std::map<std::int64_t, std::uint64_t> terms;
			for (const auto& x : docs) {
				if (std::none_of(x.d.begin(), x.d.end(), [&](double value) { return in(range, value); })) { continue; }
				++count;
				for (const auto value : std::set<std::int64_t>(x.i.begin(), x.i.end())) { ++terms[value]; }
			}
			const std::size_t b = bs[range];
			CHECK(es[b].bucket_key == agg::key(std::in_place_type<std::string>, names[range]));
			CHECK(es[b].doc_count == count);
			const auto tbs = buckets_of(es, sub_of(es, b, r_terms));
			CHECK(tbs.size() == terms.size());
			std::size_t j = 0;
			for (const auto& [value, c] : terms) {
				if (j >= tbs.size()) { break; }
				const std::size_t tb = tbs[j++];
				CHECK(es[tb].bucket_key == agg::key(std::in_place_type<std::int64_t>, value));
				CHECK(es[tb].doc_count == c);
			}
		}
	}
	// Filter: one bucket of the documents holding "a".
	{
		std::uint64_t count = 0;
		std::vector<agg::number> ints, reals;
		for (const auto& x : docs) {
			if (!x.has("a")) { continue; }
			++count;
			const auto is = numbers_of(x, I64), ds = numbers_of(x, F64);
			ints.insert(ints.end(), is.begin(), is.end());
			reals.insert(reals.end(), ds.begin(), ds.end());
		}
		const auto bs = buckets_of(es, top_of(es, f));
		CHECK(bs.size() == 1);
		if (bs.size() == 1) {
			CHECK(std::holds_alternative<std::monostate>(es[bs[0]].bucket_key));
			CHECK(es[bs[0]].doc_count == count);
			check_metric(metric_at(es, sub_of(es, bs[0], f_stats)), agg::kind::stats, brute(ints), 2.0);
			check_metric(metric_at(es, sub_of(es, bs[0], f_median)), agg::kind::median, brute(reals), 2.0);
		}
	}
	const std::string want = agg::render(req, es);
	check_splits(req, docs, rng, want, 6);
	// Merging a partial into itself doubles it, as two partials over the same documents do.
	{
		agg::partial twice(req), doubled(req);
		for (const auto& x : docs) { twice.collect(x); doubled.collect(x); doubled.collect(x); }
		twice.merge(twice);
		CHECK(agg::render(req, twice.finish()) == agg::render(req, doubled.finish()));
	}
}

// Hand-checked values, including Xapiand's documented examples.
static void fixed() {
	{
		agg::stats s;
		const std::int64_t fives[] = {5, 5};
		s.add(agg::values(std::span<const std::int64_t>(fives)));
		CHECK(s.count() == 2 && s.sum() == 10.0);
	}
	{
		agg::distribution d;
		for (const std::int64_t x : {9, 2, 4, 1, 7, 2, 3}) { d.add(agg::number(x)); }
		CHECK(d.median() == 3.0);
		CHECK(d.mode() == agg::number(std::int64_t(2)));
		d.add(agg::number(std::int64_t(4)));  // 1 2 2 3 4 4 7 9: (3 + 4) / 2, and 2 wins the tie
		CHECK(d.median() == 3.5);
		CHECK(d.mode() == agg::number(std::int64_t(2)));
	}
	{
		agg::extended e;
		for (const double x : {1.0, 2.0, 3.0, 4.0}) { e.add(agg::number(x)); }
		CHECK(e.variance() && same(*e.variance(), 5.0 / 3.0));
		CHECK(e.sum_of_squares() == 30.0);
		const auto b = e.std_deviation_bounds(3.0);
		CHECK(b && same(b->first, std::fma(-3.0, std::sqrt(5.0 / 3.0), 2.5)) && same(b->second, std::fma(3.0, std::sqrt(5.0 / 3.0), 2.5)));
		agg::extended one;
		one.add(agg::number(1.0));
		CHECK(!one.variance() && !one.std_deviation_bounds(2.0));
	}
	{
		// Exact across kinds: 2^53 + 1 as an integer is above 2^53 as a double.
		const agg::number big{std::in_place_type<std::int64_t>, (std::int64_t(1) << 53) + 1};
		const agg::number near{std::in_place_type<double>, 0x1p53};
		CHECK(agg::compare(big, near) > 0 && agg::compare(near, big) < 0);
		CHECK(agg::compare(agg::number(std::uint64_t(1) << 63), agg::number(std::int64_t(-1))) > 0);
		CHECK(agg::compare(agg::number(-0.5), agg::number(std::int64_t(0))) < 0);
		CHECK(agg::compare(agg::number(std::int64_t(3)), agg::number(3.0)) == 0);
	}
	{
		agg::request req;
		auto spec = make(agg::kind::histogram, "h", I64);
		spec.interval = std::int64_t(7);
		spec.offset = std::int64_t(3);
		const auto h = req.add(agg::request::root, std::move(spec));
		agg::partial p(req);
		doc x;
		x.i = {-1, -4, -5, 3, 9, 10};  // buckets -4 (twice), -11, 3 (twice), 10
		p.collect(x);
		const auto es = p.finish();
		const auto bs = buckets_of(es, top_of(es, h));
		CHECK(bs.size() == 4);
		if (bs.size() == 4) {
			CHECK(es[bs[0]].bucket_key == agg::key(std::int64_t(-11)));
			CHECK(es[bs[1]].bucket_key == agg::key(std::int64_t(-4)));
			CHECK(es[bs[2]].bucket_key == agg::key(std::int64_t(3)));
			CHECK(es[bs[3]].bucket_key == agg::key(std::int64_t(10)));
			for (const auto b : bs) { CHECK(es[b].doc_count == 1); }
		}
	}
	{
		// Invalid requests reject.
		agg::request req;
		const auto c = req.add(agg::request::root, make(agg::kind::count, "c", I64));
		bool threw = false;
		try { req.add(c, make(agg::kind::sum, "s", I64)); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
		threw = false;
		auto zero = make(agg::kind::histogram, "z", I64);
		zero.interval = 0.0;
		try { req.add(agg::request::root, std::move(zero)); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
	}
}

// Nesting far past any recursion budget: filter, terms, range, histogram, repeated.
static void deep(std::size_t depth) {
	agg::request req;
	agg::node_id parent = agg::request::root;
	for (std::size_t level = 0; level < depth; ++level) {
		agg::spec s;
		s.name = "l" + std::to_string(level);
		switch (level % 4) {
		case 0: s.type = agg::kind::filter; break;
		case 1: s.type = agg::kind::terms; s.slot = BYTES; break;
		case 2: s.type = agg::kind::range; s.slot = I64; s.ranges.push_back({"all", std::nullopt, std::nullopt}); break;
		default: s.type = agg::kind::histogram; s.slot = I64; s.interval = std::int64_t(1000); break;
		}
		parent = req.add(parent, std::move(s));
	}
	const auto leaf = req.add(parent, make(agg::kind::stats, "leaf", I64));
	std::mt19937_64 rng(depth);
	std::vector<doc> docs(60);
	std::int64_t total = 0;
	for (std::size_t j = 5; j < docs.size(); ++j) {  // the first five are empty
		docs[j].b.push_back(vocabulary[rng() % 3]);
		docs[j].i.push_back(std::int64_t(rng() % 1000));
		total += docs[j].i.back();
	}
	agg::partial whole(req);
	const std::size_t before = whole.bytes();
	for (const auto& x : docs) { whole.collect(x); }
	CHECK(whole.bytes() > before);
	const auto es = whole.finish();
	std::size_t deepest = 0;
	std::uint64_t leaf_count = 0;
	double leaf_sum = 0;
	for (const auto& e : es) {
		deepest = std::max(deepest, e.depth);
		if (!e.bucket && e.node == leaf) { leaf_count += e.value.count.value_or(0); leaf_sum += e.value.sum.value_or(0); }
	}
	CHECK(deepest == 2 * depth + 1);
	CHECK(leaf_count == docs.size() - 5);
	CHECK(leaf_sum == double(total));
	check_splits(req, docs, rng, agg::render(req, es), 2);
}

static void empty() {
	agg::request req;
	std::vector<agg::node_id> ids;
	for (const auto k : metric_kinds) { ids.push_back(req.add(agg::request::root, make(k, std::string(agg::kind_name(k)), F64))); }
	const auto t = req.add(agg::request::root, make(agg::kind::terms, "t", BYTES));
	req.add(t, make(agg::kind::avg, "t_avg", F64));
	auto r_spec = make(agg::kind::range, "r", F64);
	r_spec.ranges.push_back({"low", std::nullopt, agg::number(0.0)});
	const auto r = req.add(agg::request::root, std::move(r_spec));
	const auto f = req.add(agg::request::root, make(agg::kind::filter, "f", 0));
	const auto f_min = req.add(f, make(agg::kind::min, "f_min", I64));
	const oracle none = brute({});
	for (std::size_t docs : {0, 10}) {
		agg::partial p(req);
		for (std::size_t j = 0; j < docs; ++j) { p.collect(doc{}); }
		const auto es = p.finish();
		CHECK(es[0].doc_count == docs);
		for (std::size_t k = 0; k < metric_kinds.size(); ++k) { check_metric(metric_at(es, top_of(es, ids[k])), metric_kinds[k], none, 2.0); }
		CHECK(buckets_of(es, top_of(es, t)).empty());
		const auto rb = buckets_of(es, top_of(es, r));
		CHECK(rb.size() == 1 && es[rb[0]].doc_count == 0);
		const auto fb = buckets_of(es, top_of(es, f));
		CHECK(fb.size() == 1 && es[fb[0]].doc_count == docs);
		if (fb.size() == 1) { CHECK(!metric_at(es, sub_of(es, fb[0], f_min)).min); }
		agg::partial a(req), b(req);
		for (std::size_t j = 0; j < docs; ++j) { b.collect(doc{}); }
		a.merge(b);
		CHECK(agg::render(req, a.finish()) == agg::render(req, es));
	}
}

int main() {
	fixed();
	empty();
	for (std::uint64_t seed = 1; seed <= 6; ++seed) {
		const std::size_t n = seed == 1 ? 1 : seed == 2 ? 2 : 50 + seed * 40;
		metrics(seed, n);
		buckets(seed + 100, n);
	}
	deep(2000);
	std::printf(failures ? "aggregate: FAILED\n" : "aggregate: ok\n");
	return failures ? 1 : 0;
}
