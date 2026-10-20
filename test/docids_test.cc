// The docids codec (docids.hh; reordering.md D1): every kind round-trips docid(ord) and
// ordinal_of(docid), absent docids inside the range included; the kind follows the order
// written; every validation check rejects the damage it names; and a permuted segment opens,
// pinned by test/fixtures/permuted.sx.
#include "prospect/reader.hh"
#include "prospect/writer.hh"
#include "strata/core/docids.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace {

using strata::ordinal_t;

bool rejects(const std::string& section, std::uint64_t count, std::uint64_t lo, std::uint64_t hi) {
	try {
		strata::docids::reader r(section, count, lo, hi);
		return false;
	} catch (const strata::corrupt_segment&) {
		return true;
	}
}

// The kind a sequence must be written as: contiguous, monotone, or permuted.
std::uint8_t kind_of(const std::vector<std::uint64_t>& ids) {
	bool increasing = true, consecutive = true;
	for (std::size_t i = 1; i < ids.size(); ++i) {
		increasing = increasing && ids[i] > ids[i - 1];
		consecutive = consecutive && ids[i] == ids[0] + i;
	}
	return !increasing ? strata::docids::permuted : consecutive ? strata::docids::contiguous : strata::docids::monotone;
}

// Every docid answers its ordinal, every ordinal its docid, and docids between them none.
void round_trip(const std::vector<std::uint64_t>& ids, std::uint8_t expect_kind) {
	CHECK(kind_of(ids) == expect_kind);
	const std::string s = strata::docids::encode(ids);
	const auto [lo, hi] = std::minmax_element(ids.begin(), ids.end());
	strata::docids::reader r(s, ids.size(), ids.empty() ? 0 : *lo, ids.empty() ? 0 : *hi);
	CHECK(r.kind() == expect_kind);
	CHECK(r.docid_ordered() == (expect_kind != strata::docids::permuted));
	std::set<std::uint64_t> present(ids.begin(), ids.end());
	for (std::size_t i = 0; i < ids.size(); ++i) {
		CHECK(r.docid(ordinal_t(i)) == ids[i]);
		CHECK(r.ordinal_of(ids[i]) == ordinal_t(i));
		for (std::uint64_t probe : {ids[i] - 1, ids[i] + 1}) {
			if (!present.count(probe)) { CHECK(!r.ordinal_of(probe)); }
		}
	}
	if (!ids.empty()) {
		CHECK(!r.ordinal_of(*lo - 1));
		if (*hi != ~std::uint64_t(0)) { CHECK(!r.ordinal_of(*hi + 1)); }
	}
}

}  // namespace

int main() {
	std::mt19937_64 rng(31);
	// The kind follows the order: increasing docids never write kind 2.
	round_trip({}, strata::docids::contiguous);
	round_trip({7}, strata::docids::contiguous);
	round_trip({5, 6, 7, 8}, strata::docids::contiguous);
	round_trip({5, 9, 10, 400}, strata::docids::monotone);
	round_trip({2, 1}, strata::docids::permuted);
	for (std::size_t n : {2u, 3u, 127u, 128u, 129u, 300u, 4096u, 20000u}) {
		for (int gaps = 0; gaps < 3; ++gaps) {
			std::vector<std::uint64_t> ids(n);
			std::uint64_t d = gaps == 2 ? ~std::uint64_t(0) - 4 * n : 1;  // near 2^64: 64-bit widths
			for (auto& v : ids) { v = d += 1 + (gaps ? rng() % (gaps == 1 ? 3 : 2) : 0); }
			if (gaps == 2) { ids.back() = ~std::uint64_t(0); }
			round_trip(ids, kind_of(ids));
			std::shuffle(ids.begin(), ids.end(), rng);
			round_trip(ids, kind_of(ids));
			std::reverse(ids.begin(), ids.end());
			round_trip(ids, kind_of(ids));
			std::sort(ids.begin(), ids.end(), std::greater<>());  // descending: always permuted
			round_trip(ids, strata::docids::permuted);
		}
	}
	// Every width (P1-R23): the bit arrays at widths 0-64, at counts ending on every bit
	// offset, with the largest value; and permuted sections at docid widths 1-64, where
	// get_bits reads a ninth byte (widths above 57) and width 64 skips the mask.
	for (unsigned width = 0; width <= 64; ++width) {
		const std::uint64_t mask = width == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << width) - 1;
		for (std::size_t n = 1; n <= 17; ++n) {
			std::vector<std::uint64_t> v(n);
			for (auto& x : v) { x = rng() & mask; }
			v[n / 2] = mask;
			std::string b;
			strata::docids::detail::put_bits(b, v, width);
			CHECK(b.size() == strata::docids::detail::bit_bytes(n, width));
			for (std::size_t i = 0; i < n; ++i) { CHECK(strata::docids::detail::get_bits(b.data(), i, width) == v[i]); }
			for (std::size_t i = (n * width + 7) / 8; i < b.size(); ++i) { CHECK(b[i] == 0); }
		}
	}
	for (unsigned width = 1; width <= 64; ++width) {
		const std::uint64_t lo = 1, hi = width == 64 ? ~std::uint64_t(0) : lo + ((std::uint64_t(1) << width) - 1);
		for (std::size_t n = 2; n <= 17; ++n) {
			std::set<std::uint64_t> picked{lo, hi};
			while (picked.size() < std::min<std::uint64_t>(n, hi - lo + 1)) { picked.insert(lo + rng() % (hi - lo + 1)); }
			std::vector<std::uint64_t> ids(picked.rbegin(), picked.rend());  // descending: permuted
			if (ids.size() < 2) { continue; }
			const std::string sec = strata::docids::encode(ids);
			CHECK(std::uint8_t(sec[9]) == width);
			round_trip(ids, strata::docids::permuted);
		}
	}

	// Invalid input to the encoder.
	for (const std::vector<std::uint64_t>& bad_ids : {std::vector<std::uint64_t>{3, 1, 3}, std::vector<std::uint64_t>{2, 0}}) {
		bool threw = false;
		try {
			(void)strata::docids::encode(bad_ids);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		CHECK(threw);
	}

	// Validation: each check rejects the damage it names.
	{
		std::vector<std::uint64_t> ids(300);
		std::iota(ids.begin(), ids.end(), 10);
		std::shuffle(ids.begin(), ids.end(), rng);
		const std::string good = strata::docids::encode(ids);
		const std::uint64_t n = ids.size(), lo = 10, hi = 309;
		CHECK(!rejects(good, n, lo, hi));
		auto with = [&](const std::function<void(std::string&)>& f) {
			std::string s = good;
			f(s);
			return s;
		};
		for (int kind = 3; kind < 256; kind += 37) { CHECK(rejects(with([&](std::string& s) { s[0] = char(kind); }), n, lo, hi)); }
		CHECK(rejects(good, n + 1, lo, hi));                                                 // count against the footer
		CHECK(rejects(good, n, lo + 1, hi));                                                 // the footer's minimum
		CHECK(rejects(good, n, lo, hi + 1));                                                 // the footer's maximum
		CHECK(rejects(with([](std::string& s) { s[9] = char(s[9] + 1); }), n, lo, hi));       // docid width
		CHECK(rejects(with([](std::string& s) { s[10] = char(s[10] + 1); }), n, lo, hi));     // ordinal width
		CHECK(rejects(with([](std::string& s) { s.push_back('\0'); }), n, lo, hi));           // length
		CHECK(rejects(with([](std::string& s) { s.pop_back(); }), n, lo, hi));
		const std::size_t fwd = strata::docids::detail::bit_bytes(n, 9), srt = strata::docids::detail::bit_bytes(n, 9);
		CHECK(rejects(with([&](std::string& s) { s[11 + fwd - 1] = '\x01'; }), n, lo, hi));   // forward padding
		CHECK(rejects(with([&](std::string& s) { s[11 + fwd + srt - 1] = '\x01'; }), n, lo, hi));  // sorted padding
		CHECK(rejects(with([&](std::string& s) { s[11 + fwd + srt] ^= 1; }), n, lo, hi));     // an index entry
		CHECK(rejects(with([&](std::string& s) { s[11] ^= 0x40; }), n, lo, hi));              // a forward entry out of order or range
		CHECK(rejects(with([&](std::string& s) { s[11 + fwd] ^= 1; }), n, lo, hi));           // the sorted ordinals
		// Increasing forward docids are kind 1's, not kind 2's.
		std::vector<std::uint64_t> up(ids.begin(), ids.end());
		std::sort(up.begin(), up.end());
		std::string s = with([](std::string&) {});
		std::vector<std::uint64_t> zero(n, 0);
		CHECK(rejects(std::string(1, char(strata::docids::permuted)) + s.substr(1, 10) + [&] {
			std::string b;
			std::vector<std::uint64_t> fwd_vals(n), srt_vals(n);
			for (std::uint64_t i = 0; i < n; ++i) { fwd_vals[i] = up[i] - lo, srt_vals[i] = i; }
			strata::docids::detail::put_bits(b, fwd_vals, 9);
			strata::docids::detail::put_bits(b, srt_vals, 9);
			for (std::uint64_t i = 0; i < n; i += 128) { strata::format::put<std::uint64_t>(b, up[i]); }
			return b;
		}(), n, lo, hi));
		// Every single-byte change either fails validation or decodes the same answers.
		for (std::size_t at = 0; at < good.size(); ++at) {
			for (int bit = 0; bit < 8; ++bit) {
				const std::string damaged = with([&](std::string& d) { d[at] = char(d[at] ^ (1 << bit)); });
				if (!rejects(damaged, n, lo, hi)) {
					strata::docids::reader r(damaged, n, lo, hi);
					for (std::size_t i = 0; i < n; ++i) { CHECK(r.ordinal_of(r.docid(ordinal_t(i))) == ordinal_t(i)); }
				}
			}
		}
	}

	// A permuted segment: the writer's permuted mode, opened as a segment, and pinned.
	{
		prospect::segment_writer w;
		w.permuted = true;
		const char* words[] = {"alpha", "beta", "gamma"};
		std::vector<std::uint64_t> ids(260);
		for (std::size_t i = 0; i < ids.size(); ++i) { ids[i] = 3 * ((i * 97) % ids.size()) + 1; }  // a fixed permutation with gaps
		std::vector<std::uint32_t> pos = {1, 4};
		for (std::size_t i = 0; i < ids.size(); ++i) {
			std::vector<prospect::posting_input> in;
			for (std::size_t k = 0; k < 3; ++k) {
				if ((i + k) % (k + 2) == 0) { in.push_back({words[k], 2, pos}); }
			}
			const std::string key = "Q" + std::to_string(ids[i]);
			w.add(ids[i], in, 4, "record " + std::to_string(ids[i]), 1 + i % 3, key);
		}
		const std::string bytes = w.flush({});
		const char* path = PROSPECT_FIXTURES "/permuted.sx";
		if (std::getenv("PROSPECT_WRITE_FIXTURES")) {
			std::FILE* f = std::fopen(path, "wb");
			std::fwrite(bytes.data(), 1, bytes.size(), f);
			std::fclose(f);
		}
		std::string golden;
		if (std::FILE* f = std::fopen(path, "rb")) {
			for (int c; (c = std::fgetc(f)) != EOF;) { golden.push_back(char(c)); }
			std::fclose(f);
		}
		CHECK(golden == bytes);
		const prospect::segment seg(golden);
		CHECK(!seg.core().docid_ordered());
		CHECK(seg.core().min_docid() == 1 && seg.core().max_docid() == 3 * 259 + 1);
		for (std::size_t i = 0; i < ids.size(); ++i) {
			CHECK(seg.core().docid(ordinal_t(i)) == ids[i]);
			CHECK(seg.core().ordinal_of(ids[i]) == ordinal_t(i));
			CHECK(!seg.core().ordinal_of(ids[i] + 1));
			CHECK(seg.get<strata::stored_plugin>().stored(ordinal_t(i)) == "record " + std::to_string(ids[i]));
			CHECK(seg.get<strata::keys_plugin>().may_hold("Q" + std::to_string(ids[i])));
		}
		seg.verify_all();
		// A flush (docids increasing) still refuses an out-of-order docid.
		prospect::segment_writer flush;
		flush.add(5, {}, 0);
		bool threw = false;
		try {
			flush.add(4, {}, 0);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		CHECK(threw);
	}

	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("docids: ok");
	return 0;
}
