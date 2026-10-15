// Doc values (Phase 4, D4): columns round-trip for dense, sparse, very sparse, empty,
// constant, 64-bit, mapped negative, multi-valued, and bytes slots; block and group bounds,
// GCDs, and statistics (a sum past 2^64) match the input; single-byte corruptions throw
// corrupt_segment or decode without crashing, and truncations throw.
#include "strata/docvalues.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

namespace dv = strata::docvalues;
using strata::ordinal_t;
using docs_t = std::vector<std::vector<std::uint64_t>>;
using strings_t = std::vector<std::vector<std::string>>;

// Present ordinals in order, each one's values sorted: the value-index order.
static std::vector<std::uint64_t> stream_of(const docs_t& docs) {
	std::vector<std::uint64_t> out;
	for (auto d : docs) { std::sort(d.begin(), d.end()); out.insert(out.end(), d.begin(), d.end()); }
	return out;
}

static void check_bounds(const dv::column_view& c, const std::vector<std::uint64_t>& stream, std::uint64_t group_values) {
	CHECK(c.block_count() == (stream.size() + 127) / 128);
	for (std::size_t b = 0; b < c.block_count(); ++b) {
		const std::size_t first = b * 128, last = std::min(stream.size(), first + 128);
		const auto lo = *std::min_element(stream.begin() + std::ptrdiff_t(first), stream.begin() + std::ptrdiff_t(last));
		const auto hi = *std::max_element(stream.begin() + std::ptrdiff_t(first), stream.begin() + std::ptrdiff_t(last));
		std::uint64_t gcd = 0;
		for (std::size_t i = first; i < last; ++i) { gcd = std::gcd(gcd, stream[i] - lo); }
		if (!gcd) { gcd = 1; }
		const auto e = c.block(b);
		CHECK(e.min == lo && e.max == hi);
		CHECK(e.gcd == gcd);
		CHECK(e.bits == unsigned(std::bit_width((hi - lo) / gcd)));
	}
	CHECK(c.group_values() == group_values);
	CHECK(c.group_count() == (stream.size() + group_values - 1) / group_values);
	for (std::size_t g = 0; g < c.group_count(); ++g) {
		const std::size_t first = std::size_t(g * group_values), last = std::min<std::size_t>(stream.size(), std::size_t(first + group_values));
		const auto b = c.group(g);
		CHECK(b.min == *std::min_element(stream.begin() + std::ptrdiff_t(first), stream.begin() + std::ptrdiff_t(last)));
		CHECK(b.max == *std::max_element(stream.begin() + std::ptrdiff_t(first), stream.begin() + std::ptrdiff_t(last)));
	}
}

static std::string roundtrip(dv::kind k, bool multi, const docs_t& docs, std::uint32_t group_values = 4096) {
	const auto bytes = dv::encode(k, multi, docs, dv::options{group_values});
	const auto c = dv::column_view::open(bytes.data(), bytes.size());
	CHECK(c.type() == k);
	CHECK(c.multi_valued() == multi);
	CHECK(c.doc_count() == docs.size());
	std::uint64_t present = 0;
	std::vector<std::uint64_t> got;
	for (std::size_t o = 0; o < docs.size(); ++o) {
		auto want = docs[o];
		std::sort(want.begin(), want.end());
		CHECK(c.has(ordinal_t(o)) == !want.empty());
		CHECK(c.values(ordinal_t(o), got) == want.size());
		CHECK(got == want);
		if (!want.empty()) { CHECK(c.value_index(ordinal_t(o)) == present); ++present; }
	}
	CHECK(!c.has(ordinal_t(docs.size())));
	const auto stream = stream_of(docs);
	__int128 sum = 0;
	for (const auto v : stream) {
		if (k == dv::kind::u64) { sum += static_cast<__int128>(v); } else if (k == dv::kind::i64) { sum += strata::ordered::to_i64(v); }
	}
	const auto& s = c.stats();
	CHECK(s.docs == present);
	CHECK(s.values == stream.size());
	CHECK(s.min == (stream.empty() ? 0 : *std::min_element(stream.begin(), stream.end())));
	CHECK(s.max == (stream.empty() ? 0 : *std::max_element(stream.begin(), stream.end())));
	CHECK(s.has_sum == (k == dv::kind::u64 || k == dv::kind::i64));
	CHECK(s.sum == (s.has_sum ? sum : 0));
	check_bounds(c, stream, group_values);
	return bytes;
}

static std::string roundtrip_bytes(bool multi, const strings_t& docs) {
	const auto bytes = dv::encode_bytes(multi, docs);
	const auto c = dv::column_view::open(bytes.data(), bytes.size());
	CHECK(c.type() == dv::kind::bytes);
	CHECK(c.multi_valued() == multi);
	CHECK(c.doc_count() == docs.size());
	std::vector<std::string> dictionary;
	for (const auto& d : docs) { dictionary.insert(dictionary.end(), d.begin(), d.end()); }
	std::sort(dictionary.begin(), dictionary.end());
	dictionary.erase(std::unique(dictionary.begin(), dictionary.end()), dictionary.end());
	CHECK(c.terms() == dictionary.size());
	for (std::size_t i = 0; i < dictionary.size(); ++i) {
		CHECK(c.term(i) == dictionary[i]);
		CHECK(c.find(dictionary[i]) == std::optional<std::uint64_t>(i));
	}
	CHECK(!c.find("~missing").has_value());
	std::uint64_t present = 0, values = 0;
	std::vector<std::uint64_t> got;
	for (std::size_t o = 0; o < docs.size(); ++o) {
		auto want = docs[o];
		std::sort(want.begin(), want.end());
		want.erase(std::unique(want.begin(), want.end()), want.end());
		CHECK(c.has(ordinal_t(o)) == !want.empty());
		CHECK(c.values(ordinal_t(o), got) == want.size());
		std::vector<std::string> terms;
		for (const auto ord : got) { terms.emplace_back(c.term(ord)); }
		CHECK(terms == want);
		present += !want.empty();
		values += want.size();
	}
	CHECK(c.stats().docs == present);
	CHECK(c.stats().values == values);
	CHECK(!c.stats().has_sum);
	if (values) { CHECK(c.stats().min == 0 && c.stats().max == dictionary.size() - 1); }
	return bytes;
}

// Reads everything a malformed column admits; corrupt_segment is the only allowed failure.
static void exercise(const std::string& bytes) {
	try {
		const auto c = dv::column_view::open(bytes.data(), bytes.size());
		std::vector<std::uint64_t> got;
		const auto n = std::min<std::uint64_t>(c.doc_count(), 4096);
		for (std::uint64_t o = 0; o < n; ++o) {
			if (c.values(ordinal_t(o), got) && c.type() == dv::kind::bytes) {
				for (const auto ord : got) { (void)c.term(ord); }
			}
		}
		for (std::size_t b = 0; b < c.block_count(); ++b) { (void)c.block(b); }
		for (std::size_t g = 0; g < c.group_count(); ++g) { (void)c.group(g); }
		for (std::uint64_t t = 0; t < std::min<std::uint64_t>(c.terms(), 4096); ++t) { (void)c.term(t); }
		(void)c.stats();
	} catch (const strata::corrupt_segment&) {
	}
}

static void corrupt_and_truncate(const std::string& good) {
	const std::size_t step = std::max<std::size_t>(1, good.size() / 600);
	for (std::size_t at = 0; at < good.size(); at += step) {
		for (const unsigned mask : {0x01u, 0x80u, 0xffu}) {
			auto bad = good;
			bad[at] = char(std::uint8_t(bad[at]) ^ mask);
			exercise(bad);
		}
	}
	std::vector<std::size_t> lengths;
	for (std::size_t n = 0; n < good.size(); n += std::max<std::size_t>(1, good.size() / 300)) { lengths.push_back(n); }
	lengths.push_back(good.size() - 1);
	for (const auto n : lengths) {
		bool threw = false;
		try { (void)dv::column_view::open(good.data(), n); } catch (const strata::corrupt_segment&) { threw = true; }
		CHECK(threw);
	}
	auto longer = good + '\0';
	bool threw = false;
	try { (void)dv::column_view::open(longer.data(), longer.size()); } catch (const strata::corrupt_segment&) { threw = true; }
	CHECK(threw);
}

int main() {
	using namespace strata::ordered;
	std::mt19937_64 rng(4);
	constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();

	// Dense, full-width values.
	{
		docs_t docs(10000);
		for (auto& d : docs) { d.push_back(rng()); }
		roundtrip(dv::kind::u64, false, docs);
	}
	// Dense, small signed range.
	{
		docs_t docs(5000);
		for (auto& d : docs) { d.push_back(from_i64(std::int64_t(rng() % 2001) - 1000)); }
		roundtrip(dv::kind::i64, false, docs);
	}
	// Sparse, and very sparse with a bitset chunk among array chunks.
	{
		docs_t docs(20000);
		for (auto& d : docs) { if (rng() % 10 == 0) { d.push_back(rng() % 100000); } }
		roundtrip(dv::kind::u64, false, docs);
		docs_t very(300000);
		for (std::size_t o = 0; o < very.size(); ++o) {
			const bool dense = o >= 70000 && o < 140000;
			if (dense ? rng() % 2 == 0 : rng() % 3000 == 0) { very[o].push_back(rng() % 50); }
		}
		roundtrip(dv::kind::u64, false, very);
	}
	// Empty columns: no ordinals, and ordinals without values.
	{
		roundtrip(dv::kind::u64, false, docs_t{});
		roundtrip(dv::kind::i64, true, docs_t{});
		roundtrip(dv::kind::u64, false, docs_t(100));
		roundtrip(dv::kind::f64, true, docs_t(100));
	}
	// All-equal values pack at width 0.
	{
		docs_t docs(5000, std::vector<std::uint64_t>{42});
		const auto bytes = roundtrip(dv::kind::u64, false, docs);
		const auto c = dv::column_view::open(bytes.data(), bytes.size());
		for (std::size_t b = 0; b < c.block_count(); ++b) { CHECK(c.block(b).bits == 0); }
	}
	// Values needing all 64 bits.
	{
		docs_t docs(1000);
		for (auto& d : docs) { d.push_back(rng()); }
		docs[3] = {0};
		docs[7] = {u64_max};
		const auto bytes = roundtrip(dv::kind::u64, false, docs);
		const auto c = dv::column_view::open(bytes.data(), bytes.size());
		CHECK(c.block(0).bits == 64);
	}
	// Mapped negatives: i64 extremes and doubles of both signs.
	{
		docs_t ints(3000), doubles(3000);
		for (auto& d : ints) { d.push_back(from_i64(std::int64_t(rng()))); }
		ints[0] = {from_i64(std::numeric_limits<std::int64_t>::min())};
		ints[1] = {from_i64(std::numeric_limits<std::int64_t>::max())};
		roundtrip(dv::kind::i64, false, ints);
		std::uniform_real_distribution<double> real(-1e6, 1e6);
		for (auto& d : doubles) { if (rng() % 4) { d.push_back(from_f64(real(rng))); } }
		doubles[0] = {from_f64(-std::numeric_limits<double>::infinity())};
		doubles[1] = {from_f64(-0.0)};
		roundtrip(dv::kind::f64, false, doubles);
	}
	// Multi-valued with duplicates, and small groups.
	{
		docs_t docs(3000);
		for (auto& d : docs) {
			const auto n = rng() % 5;
			for (std::uint64_t i = 0; i < n; ++i) { d.push_back(from_i64(std::int64_t(rng() % 21) - 10)); }
		}
		docs[0] = {from_i64(5), from_i64(5)};
		roundtrip(dv::kind::i64, true, docs);
		roundtrip(dv::kind::i64, true, docs, 256);
		docs_t wide(2000);
		for (auto& d : wide) { for (std::uint64_t i = rng() % 3; i > 0; --i) { d.push_back(rng()); } }
		roundtrip(dv::kind::u64, true, wide, 128);
	}
	// GCD blocks: multiples of 1000 from an offset.
	{
		docs_t docs(1000);
		for (auto& d : docs) { d.push_back(from_i64(7 + 1000 * std::int64_t(rng() % 1000))); }
		const auto bytes = roundtrip(dv::kind::i64, false, docs);
		const auto c = dv::column_view::open(bytes.data(), bytes.size());
		for (std::size_t b = 0; b < c.block_count(); ++b) {
			CHECK(c.block(b).gcd % 1000 == 0);
			CHECK(c.block(b).bits <= 10);
		}
	}
	// Exact sums past 2^64, and below -2^64.
	{
		docs_t big(10);
		for (std::size_t i = 0; i < big.size(); ++i) { big[i] = {u64_max - i}; }
		const auto bytes = roundtrip(dv::kind::u64, false, big);
		const auto c = dv::column_view::open(bytes.data(), bytes.size());
		CHECK(c.stats().sum == static_cast<__int128>(u64_max) * 10 - 45);
		CHECK(c.stats().sum > (static_cast<__int128>(1) << 64));
		docs_t low(4, std::vector<std::uint64_t>{from_i64(std::numeric_limits<std::int64_t>::min())});
		low.push_back({from_i64(std::numeric_limits<std::int64_t>::min()), from_i64(-1)});
		const auto lbytes = roundtrip(dv::kind::i64, true, low);
		const auto l = dv::column_view::open(lbytes.data(), lbytes.size());
		CHECK(l.stats().sum == static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) * 5 - 1);
	}
	// Bytes: shared prefixes and empty strings, single- and multi-valued.
	{
		const std::vector<std::string> stems{"", "a", "ab", "abc", "prefix/", "prefix/x"};
		auto random_string = [&] {
			auto s = stems[rng() % stems.size()];
			for (auto n = rng() % 4; n > 0; --n) { s += char('a' + rng() % 3); }
			return s;
		};
		strings_t multi(2000), single(2000);
		for (auto& d : multi) { for (auto n = rng() % 5; n > 0; --n) { d.push_back(random_string()); } }
		for (auto& d : single) { if (rng() % 3) { d.push_back(random_string()); } }
		multi[0] = {"", "", "prefix/"};
		single[0] = {""};
		roundtrip_bytes(true, multi);
		roundtrip_bytes(false, single);
		roundtrip_bytes(true, strings_t(10));
		roundtrip_bytes(false, strings_t{});
	}
	// Misuse: several values in a single-valued slot, a group size off the block.
	{
		bool threw = false;
		try { (void)dv::encode(dv::kind::u64, false, docs_t{{1, 2}}); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
		threw = false;
		try { (void)dv::encode(dv::kind::u64, false, docs_t{{1}}, dv::options{100}); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
	}
	// Corruptions and truncations: a sparse multi-valued column with presence and offsets,
	// a dense single-valued one, and a bytes column with its dictionary.
	{
		docs_t sparse(400);
		for (auto& d : sparse) { if (rng() % 3 == 0) { for (auto n = 1 + rng() % 3; n > 0; --n) { d.push_back(from_i64(std::int64_t(rng() % 1000) - 500)); } } }
		corrupt_and_truncate(dv::encode(dv::kind::i64, true, sparse, dv::options{128}));
		docs_t dense(300);
		for (auto& d : dense) { d.push_back(rng() % 5000 * 3); }
		corrupt_and_truncate(dv::encode(dv::kind::u64, false, dense, dv::options{128}));
		strings_t strings(200);
		for (auto& d : strings) { if (rng() % 2) { d = {"prefix/" + std::to_string(rng() % 40), std::string(rng() % 3, 'z')}; } }
		corrupt_and_truncate(dv::encode_bytes(true, strings));
	}
	std::printf(failures ? "docvalues: FAILED\n" : "docvalues: ok\n");
	return failures ? 1 : 0;
}
