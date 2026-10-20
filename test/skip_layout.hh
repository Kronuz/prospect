// A term's skip data parsed for tests that check or edit it in place (format.hh, "skip
// data"): each group's advance entries and each block's bytes, as file offsets.
#pragma once

#include "strata/search/reader.hh"

#include <cstddef>
#include <string_view>
#include <vector>

namespace skip_test {

struct group {
	std::size_t adv = 0;  // the advance entries' file offset
	std::size_t n = 0;
	unsigned wl = 0, wo = 0, wf = 0;
	strata::ordinal_t base = 0, last = 0;
	std::uint64_t bit_last(std::size_t i) const { return i * wl; }                            // i < n - 1
	std::uint64_t bit_offset(std::size_t i) const { return (n - 1) * wl + (i - 1) * wo; }      // 0 < i
	std::uint64_t bit_codec(std::size_t i) const { return (n - 1) * (wl + wo) + i * wf; }
};
struct block {
	std::size_t at = 0, bytes = 0;  // file offset and length
	unsigned codec = 0;
	strata::ordinal_t last = 0;
};
struct layout {
	std::size_t flags_at = 0;  // the skip flags byte's file offset
	std::size_t seeds_at = 0;  // the threshold seeds' file offset
	std::vector<group> groups;
	std::vector<block> blocks;
};

inline layout parse(std::string_view file, const strata::segment& s, const strata::format::term_info& info) {
	namespace f = strata::format;
	const auto& inv = s.get<strata::inverted_plugin>();
	const char* base = file.data();
	const char* p = inv.postings_bytes().data() + info.postings;
	const char* end = p + info.postings_length;
	const bool positions = !(info.flags & f::no_positions);
	layout out;
	const std::uint64_t groups = f::get_leb(&p, end);
	out.flags_at = std::size_t(p - base);
	const std::uint8_t flags = std::uint8_t(*p++);
	const std::size_t w = flags & f::skip::wide ? 8 : 4;
	const unsigned wf = flags >> f::skip::codec_shift & 3;
	if (positions) {
		(void)f::get_leb(&p, end);
		(void)f::get_leb(&p, end);
	}
	out.seeds_at = std::size_t(p - base);
	for (std::size_t j = 0; j < 3 && !(info.flags & f::no_wdf) && f::seed_ks[j] <= info.df; ++j) {
		(void)f::get_leb(&p, end);
		++p;
		(void)f::get_leb(&p, end);
		(void)f::get_leb(&p, end);
	}
	const std::uint64_t pairs = f::get_leb(&p, end), records = f::get_leb(&p, end), blocks = f::get_leb(&p, end);
	const std::size_t entry = 8 + w * (positions ? 5 : 2);
	const char* level1 = p;
	const char* recs = p + groups * entry + pairs;
	const char* blks = recs + records;
	auto offset = [&](std::size_t g, std::size_t i) -> std::uint64_t {
		const char* e = level1 + g * entry + 8 + w * i;
		return w == 8 ? f::get<std::uint64_t>(e) : f::get<std::uint32_t>(e);
	};
	const std::size_t nblocks = info.df / f::block;
	for (std::size_t g = 0; g < groups; ++g) {
		group gr;
		gr.n = std::min<std::size_t>(32, nblocks - g * 32);
		gr.last = f::get<std::uint32_t>(level1 + g * entry);
		gr.base = g ? f::get<std::uint32_t>(level1 + (g - 1) * entry) : 0;
		const std::uint64_t b0 = offset(g, 1), b1 = g + 1 < groups ? offset(g + 1, 1) : blocks;
		gr.wl = unsigned(std::bit_width(gr.last - gr.base));
		gr.wo = unsigned(std::bit_width(b1 - b0 - 1));
		gr.wf = wf;
		const char* adv = recs + offset(g, 0);
		gr.adv = std::size_t(adv - base);
		const std::uint64_t size = f::advance_bytes(gr.n, gr.wl, gr.wo, gr.wf);
		for (std::size_t i = 0; i < gr.n; ++i) {
			block b;
			const std::uint64_t start = i ? f::get_bits(adv, size, gr.bit_offset(i), gr.wo) : 0;
			const std::uint64_t stop = i + 1 < gr.n ? f::get_bits(adv, size, gr.bit_offset(i + 1), gr.wo) : b1 - b0;
			b.at = std::size_t(blks + b0 + start - base);
			b.bytes = std::size_t(stop - start);
			b.codec = wf ? f::get_bits(adv, size, gr.bit_codec(i), wf) : 0;
			b.last = i + 1 < gr.n ? gr.base + f::get_bits(adv, size, gr.bit_last(i), gr.wl) : gr.last;
			out.blocks.push_back(b);
		}
		out.groups.push_back(gr);
	}
	return out;
}

// Rewrites bits [at, at + w) of the file's bytes at offset `adv`.
inline void put(std::string& file, std::size_t adv, std::uint64_t at, unsigned w, std::uint64_t v) {
	for (unsigned i = 0; i < w; ++i, ++at) {
		char& c = file[adv + (at >> 3)];
		c = char((std::uint8_t(c) & ~(1u << (at & 7))) | ((v >> i & 1) << (at & 7)));
	}
}

}  // namespace skip_test
