// Ordered keys (Phase 4, D3a): round trips and order over every type's extremes: minimum
// and maximum integers, ±0, ±infinity, the smallest subnormals, and dates at years 1 and
// 9999 as f64 seconds; random pairs keep their order.
#include "strata/search/ordered.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

int main() {
	using namespace strata::ordered;
	using lim = std::numeric_limits<std::int64_t>;
	std::vector<std::int64_t> ints{lim::min(), lim::min() + 1, -2, -1, 0, 1, 2, lim::max() - 1, lim::max()};
	for (std::size_t i = 0; i < ints.size(); ++i) {
		CHECK(to_i64(from_i64(ints[i])) == ints[i]);
		if (i) { CHECK(from_i64(ints[i - 1]) < from_i64(ints[i])); }
	}
	using dl = std::numeric_limits<double>;
	const double year1 = -62135596800.0, year9999 = 253402300799.0;  // seconds since the epoch
	std::vector<double> doubles{-dl::infinity(), -dl::max(), year1, -1.0, -dl::min(), -dl::denorm_min(), 0.0, dl::denorm_min(), dl::min(), 1.0, year9999, dl::max(), dl::infinity()};
	for (std::size_t i = 0; i < doubles.size(); ++i) {
		CHECK(to_f64(from_f64(doubles[i])) == doubles[i]);
		if (i) { CHECK(from_f64(doubles[i - 1]) < from_f64(doubles[i])); }
	}
	CHECK(from_f64(-0.0) == from_f64(0.0) && !std::signbit(to_f64(from_f64(-0.0))));
	std::mt19937_64 rng(3);
	for (int i = 0; i < 100000; ++i) {
		const auto a = std::int64_t(rng()), b = std::int64_t(rng());
		CHECK((a < b) == (from_i64(a) < from_i64(b)));
		const double x = std::bit_cast<double>(rng()), y = std::bit_cast<double>(rng());
		if (std::isnan(x) || std::isnan(y)) { continue; }
		CHECK((x < y) == (from_f64(x) < from_f64(y)));
		unsigned char p[8], q[8]; put_be(from_f64(x), p); put_be(from_f64(y), q);
		CHECK((x < y) == (std::memcmp(p, q, 8) < 0) || x == y);
	}
	std::printf(failures ? "ordered: FAILED\n" : "ordered: ok\n");
	return failures ? 1 : 0;
}
