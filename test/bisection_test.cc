// Recursive graph bisection (bisection.hh; reordering.md D4): the checked-in tables pinned by
// hash, the fixed-point log and the move gain pinned against offline values and bounded
// against the real-valued formula, and the order itself: a permutation, deterministic,
// separating shuffled clusters, lowering the log-gap cost, and honoring cancellation.
#include "strata/search/bisection.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace bs = strata::bisection;

namespace {

template <std::size_t N>
std::uint64_t fnv(const std::uint32_t (&values)[N]) {
	std::uint64_t h = 1469598103934665603ull;
	for (std::uint32_t v : values) {
		for (int b = 0; b < 4; ++b) {
			h ^= (v >> (8 * b)) & 0xff;
			h *= 1099511628211ull;
		}
	}
	return h;
}

double real_gain(double d1, double n1, double d2, double n2) {
	auto c = [](double d, double n) { return d * std::log2(n / (d + 1)); };
	return (c(d1, n1) + c(d2, n2) - c(d1 - 1, n1) - c(d2 + 1, n2)) * 65536;
}

// The papers' log-gap estimate of an order, in bits: per term, per block of `block` documents.
double log_gap_cost(const bs::graph& g, const std::vector<std::uint32_t>& perm, std::uint32_t block) {
	double cost = 0;
	for (std::size_t a = 0; a < perm.size(); a += block) {
		std::vector<std::uint32_t> deg(g.terms, 0);
		const std::size_t b = std::min(perm.size(), a + block);
		for (std::size_t i = a; i < b; ++i) {
			g.for_row(perm[i], [&](std::uint32_t t) { ++deg[t]; });
		}
		for (auto d : deg) {
			if (d) { cost += d * std::log2(double(b - a) / (d + 1)); }
		}
	}
	return cost;
}

}  // namespace

int main() {
	// The tables, against the hashes of the offline values.
	CHECK(fnv(bs::tables::log2_mantissa) == 0x50d45cdc52ff0b72ull);
	CHECK(fnv(bs::tables::d_correction) == 0xf7487ab2c602ef7aull);

	// L(x): exact at powers of two, within its stated error elsewhere.
	for (unsigned k = 0; k < 63; ++k) { CHECK(bs::log2_fixed(std::uint64_t(1) << k) == std::int64_t(k) << 16); }
	std::mt19937_64 rng(41);
	for (int i = 0; i < 100000; ++i) {
		const std::uint64_t x = 1 + (rng() >> (rng() % 63));
		const double err = double(bs::log2_fixed(x)) - 65536.0 * std::log2(double(x));
		CHECK(err > -93 && err < 1);
	}

	// The gain: pinned against offline values, and near the real-valued formula.
	struct gain_case {
		std::uint64_t d1, n1, d2, n2;
		std::int64_t expect;
	};
	const gain_case cases[] = {
		{1, 2, 0, 2, 0},
		{5, 100, 3, 100, -22897},
		{4096, 2516051, 10, 2516051, -563768},
		{2000000, 2516051, 500000, 2516051, -131072},
		{100000, 157253, 100031, 157253, 61},
		{64, 9828, 9000, 9828, 468377},
		{3, 32, 29, 33, 226204},
	};
	for (const auto& c : cases) {
		CHECK(bs::move_gain(c.d1, c.n1, c.d2, c.n2) == c.expect);
		CHECK(std::abs(double(c.expect) - real_gain(double(c.d1), double(c.n1), double(c.d2), double(c.n2))) < 420);
	}
	for (int i = 0; i < 100000; ++i) {
		const std::uint64_t n1 = 2 + rng() % 5000000, n2 = 2 + rng() % 5000000;
		const std::uint64_t d1 = 1 + rng() % n1, d2 = rng() % n2;
		const double err = double(bs::move_gain(d1, n1, d2, n2)) - real_gain(double(d1), double(n1), double(d2), double(n2));
		CHECK(std::abs(err) < 420);  // 2 · 92 for the L terms, 2 · (92 + 24) for the D terms
	}

	// The order: four clusters of documents, each drawing its terms from its own pool, shuffled.
	bs::graph_buffer buf;
	std::vector<int> cluster;
	std::vector<std::vector<std::uint32_t>> rows;
	for (int i = 0; i < 4000; ++i) {
		const int c = int(rng() % 4);
		std::set<std::uint32_t> row;
		for (int k = 0, n = 5 + int(rng() % 10); k < n; ++k) { row.insert(std::uint32_t(c * 60 + rng() % 60)); }
		rows.emplace_back(row.begin(), row.end());
		buf.add(rows.back());
		cluster.push_back(c);
	}
	const bs::graph g = buf.view(4 * 60);
	// The bytes round-trip, and check() rejects damage to them.
	for (std::uint32_t d = 0; d < g.docs; ++d) {
		std::vector<std::uint32_t> back;
		g.for_row(d, [&](std::uint32_t t) { back.push_back(t); });
		CHECK(back == rows[d]);
	}
	auto rejects = [](const bs::graph& h) {
		try {
			h.check();
			return false;
		} catch (const std::runtime_error&) {
			return true;
		}
	};
	CHECK(!rejects(g));
	CHECK(rejects(buf.view(4 * 60 - 1)));  // an id past the terms
	{
		bs::graph_buffer b2 = buf;
		b2.rows[g.offset(1) - 1] |= char(0x80);  // a varint running past its row's end
		CHECK(rejects(b2.view(240)));
		bs::graph_buffer b3;
		const std::uint32_t twice[] = {3, 3};
		b3.add(twice);  // not increasing
		CHECK(rejects(b3.view(240)));
		bs::graph short_offsets = g;
		short_offsets.offsets.remove_suffix(8);
		CHECK(rejects(short_offsets));
	}
	const auto perm = bs::order(g);
	std::vector<std::uint32_t> sorted = perm;
	std::sort(sorted.begin(), sorted.end());
	for (std::uint32_t i = 0; i < sorted.size(); ++i) { CHECK(sorted[i] == i); }  // a permutation
	CHECK(bs::order(g) == perm);                                                   // deterministic
	for (std::uint32_t threads : {2u, 4u, 8u}) {                                    // at any thread count
		bs::presets tp;
		tp.threads = threads;
		CHECK(bs::order(g, tp) == perm);
	}
	// Each quarter of the order is mostly one cluster.
	for (int q = 0; q < 4; ++q) {
		int count[4] = {};
		for (int i = q * 1000; i < (q + 1) * 1000; ++i) { ++count[cluster[perm[i]]]; }
		CHECK(*std::max_element(count, count + 4) >= 900);
	}
	std::vector<std::uint32_t> identity(perm.size());
	std::iota(identity.begin(), identity.end(), 0u);
	const double before = log_gap_cost(g, identity, 128), after = log_gap_cost(g, perm, 128);
	CHECK(after < 0.8 * before);
	std::printf("clusters: log-gap cost %.0f -> %.0f bits\n", before, after);

	// Parts at or below min_partition are left alone: a small graph keeps its order.
	bs::graph_buffer small;
	for (std::uint32_t t : {0u, 1u, 0u, 1u}) { small.add(std::span<const std::uint32_t>(&t, 1)); }
	CHECK(bs::order(small.view(2)) == std::vector<std::uint32_t>({0, 1, 2, 3}));

	// Cancellation.
	task::cancel_source stop;
	stop.request();
	const task::cancel_token token = stop.token();
	bool threw = false;
	try {
		(void)bs::order(g, {}, &token);
	} catch (const task::cancelled&) {
		threw = true;
	}
	CHECK(threw);
	{
		bs::presets tp;
		tp.threads = 4;
		bool threw4 = false;
		try {
			(void)bs::order(g, tp, &token);
		} catch (const task::cancelled&) {
			threw4 = true;
		}
		CHECK(threw4);
	}

	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("bisection: ok");
	return 0;
}
