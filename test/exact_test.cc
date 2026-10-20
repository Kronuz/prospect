// Exact accumulators (Phase 4, D11): double sums equal Shewchuk's correctly rounded sum
// (Python's math.fsum algorithm), whatever the order and however they are split and
// merged; squares enter exactly; integer sums do not overflow; infinities and NaN follow
// IEEE addition.
#include "prospect/exact.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

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

int main() {
	std::mt19937_64 rng(11);
	auto random_double = [&](int spread) {
		const double mantissa = std::ldexp(double(rng() >> 11), -53) + 0.5;
		return (rng() & 1 ? -1.0 : 1.0) * std::ldexp(mantissa, int(rng() % (2 * spread + 1)) - spread);
	};
	for (int round = 0; round < 400; ++round) {
		const int spread = round % 4 == 0 ? 1020 : round % 4 == 1 ? 60 : round % 4 == 2 ? 10 : 1;
		std::vector<double> xs(1 + rng() % 400);
		for (auto& x : xs) { x = random_double(spread); }
		if (round % 5 == 0) { for (std::size_t i = 0; i < xs.size() / 2; ++i) { xs.push_back(-xs[i]); } }  // heavy cancellation
		if (round % 7 == 0) { xs.push_back(std::numeric_limits<double>::denorm_min()); xs.push_back(-std::numeric_limits<double>::min()); }
		prospect::exact::double_sum whole;
		for (double x : xs) { whole.add(x); }
		const double want = fsum(xs);
		CHECK(same(whole.value(), want));
		// Any order, any split into shards, merged in any order: the same bits.
		std::shuffle(xs.begin(), xs.end(), rng);
		const std::size_t shards = 1 + rng() % 64;
		std::vector<prospect::exact::double_sum> parts(shards);
		for (double x : xs) { parts[rng() % shards].add(x); }
		prospect::exact::double_sum merged;
		for (auto& p : parts) { merged.merge(p); }
		CHECK(same(merged.value(), want));
	}
	// Squares: x*x enters with its rounding error, so a sum of squares is exact.
	{
		prospect::exact::double_sum squares; std::vector<double> terms;
		for (int i = 0; i < 1000; ++i) {
			const double x = random_double(200);
			squares.add_square(x);
			const double high = x * x; terms.push_back(high); terms.push_back(std::fma(x, x, -high));
		}
		CHECK(same(squares.value(), fsum(terms)));
		prospect::exact::double_sum one; one.add_square(1.0 + std::ldexp(1.0, -52));  // 1 + 2^-51 + 2^-104
		CHECK(one.value() == 1.0 + std::ldexp(1.0, -51));
	}
	// Overflow to infinity, infinities and NaN, and an empty sum.
	{
		prospect::exact::double_sum big; big.add(std::numeric_limits<double>::max()); big.add(std::numeric_limits<double>::max());
		CHECK(std::isinf(big.value()) && big.value() > 0);
		prospect::exact::double_sum back; back.add(std::numeric_limits<double>::max()); back.add(std::numeric_limits<double>::max()); back.add(-std::numeric_limits<double>::max());
		CHECK(back.value() == std::numeric_limits<double>::max());  // the intermediate overflow does not stick
		prospect::exact::double_sum inf; inf.add(1.0); inf.add(-std::numeric_limits<double>::infinity());
		CHECK(std::isinf(inf.value()) && inf.value() < 0);
		inf.add(std::numeric_limits<double>::infinity());
		CHECK(std::isnan(inf.value()));
		CHECK(prospect::exact::double_sum{}.value() == 0.0);
	}
	// 128-bit integer sums past 2^63.
	{
		prospect::exact::integer_sum s;
		for (int i = 0; i < 4; ++i) { s.add(std::numeric_limits<std::int64_t>::max()); }
		CHECK(s.value == static_cast<__int128>(std::numeric_limits<std::int64_t>::max()) * 4);
	}
	std::printf(failures ? "exact: FAILED\n" : "exact: ok\n");
	return failures ? 1 : 0;
}
