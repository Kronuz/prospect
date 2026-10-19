// Format 13's postings items (docs/design/format-13.md F10, F11, F13): the advance entries
// (M16, A3 at 128 postings), against a model of every term's postings, positions, and
// pairs; and corrupt entries, which fence or answer with ordinals in order and in range.
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/searcher.hh"
#include "strata/check.hh"
#include "strata/writer.hh"
#include "skip_layout.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace {

namespace f = strata::format;
using strata::ordinal_t;

struct posting {
	ordinal_t ord;
	std::uint32_t wdf;
	std::vector<std::uint32_t> positions;
};
struct corpus {
	std::map<std::string, std::vector<posting>> terms;
	std::vector<std::uint32_t> lengths;
	std::string bytes;
};

// Terms of several shapes: dense at 60% (bitset blocks), with and without wdf, every third,
// sparse, clustered runs (Elias-Fano blocks), boolean, irregular gaps with runs, and counts
// that differ from wdf; lengths 1 to 300.
corpus build(std::uint32_t docs, std::uint64_t seed, bool wide = false, std::uint64_t first_docid = 1) {
	std::mt19937_64 rng(seed);
	corpus c;
	strata::segment_writer w;
	w.wide_skip_offsets = wide;
	for (std::uint32_t d = 0; d < docs; ++d) {
		std::vector<strata::posting_input> in;
		std::vector<std::vector<std::uint32_t>> pos;
		pos.reserve(8);
		const std::uint32_t length = 1 + std::uint32_t(rng() % 300);
		auto add = [&](const char* t, std::uint32_t wdf, std::uint32_t count) {
			auto& p = pos.emplace_back();
			for (std::uint32_t k = 0, at = 0; k < count; ++k) { p.push_back(at += 1 + std::uint32_t(rng() % 4)); }
			in.push_back({t, wdf, p});
			c.terms[t].push_back({d, wdf, p});
		};
		if (rng() % 100 < 60) { add("dense", 1 + std::uint32_t(rng() % 3), 0); }
		if (rng() % 100 < 60) { add("dense0", 0, 0); }
		if (d % 3 == 0) {
			const std::uint32_t n = 1 + std::uint32_t(rng() % 4);
			add("mid", n, n);
		}
		if (d % 23 == 0) { add("sparse", 1 + std::uint32_t(rng() % 50), 0); }
		// Runs and long gaps (Elias-Fano blocks), and one long run (FOR constants).
		if ((d / 120) % 9 == 0 || d % 997 == 0 || (d >= 10000 && d < 11000)) { add("clustered", 1, 0); }
		if (d % 2 == 0) { add("bool", 0, 0); }
		if (rng() % 7 == 0 || (d / 1000) % 5 == 1) { add("gaps", 1 + std::uint32_t(rng() % 9), 0); }
		if (d % 4 == 1) {
			const std::uint32_t n = 1 + std::uint32_t(rng() % 3);
			add("counts", 3 * n, n);
		}
		c.lengths.push_back(length);
		w.add(first_docid + d, in, length);
	}
	c.bytes = w.flush({});
	return c;
}

void reseal(std::string& file) {
	const auto footer = f::decode_footer(file);
	auto toc = f::decode_toc(file, footer);
	for (auto& e : toc) { e = f::describe(e.kind, e.offset, std::string_view(file).substr(e.offset, e.length), e.region_shift); }
	const std::string toc_bytes = f::encode_toc(toc);
	file.replace(footer.toc_offset, toc_bytes.size(), toc_bytes);
	auto ft = footer;
	ft.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
	const std::string footer_bytes = f::encode_footer(ft);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

std::vector<f::impact> pairs_of(const corpus& c, const std::vector<posting>& ps, std::size_t from, std::size_t to) {
	std::vector<f::impact> out;
	for (std::size_t i = from; i < to; ++i) { out.push_back({ps[i].wdf, f::encode_length(c.lengths[ps[i].ord])}); }
	return f::competitive(out);
}

bool same(std::span<const f::impact> a, const std::vector<f::impact>& b) {
	if (a.size() != b.size()) { return false; }
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (a[i].wdf != b[i].wdf || a[i].length != b[i].length) { return false; }
	}
	return true;
}

// Every read path of one term against the model.
void check_term(const corpus& c, const strata::segment& s, const std::string& term) {
	const auto& inv = s.get<strata::inverted_plugin>();
	const auto& ps = c.terms.at(term);
	const auto info = *inv.term(term);
	CHECK(info.df == ps.size());
	{  // next(), wdf, and positions in order
		strata::postings_iterator it(inv, info);
		strata::positions_reader pr(inv, info);
		std::size_t i = 0;
		bool ok = true;
		for (ordinal_t d = it.next(); d != strata::no_more_docs; d = it.next(), ++i) {
			ok = ok && i < ps.size() && d == ps[i].ord && it.wdf() == ps[i].wdf;
			if (ok && !(info.flags & f::no_positions)) {
				const auto got = pr.read(it);
				ok = std::vector<std::uint32_t>(got.begin(), got.end()) == ps[i].positions;
			}
		}
		CHECK(ok && i == ps.size());
	}
	{  // advance() from a fresh iterator to every ordinal, and forward in random steps
		bool ok = true;
		for (ordinal_t t = 0; t < c.lengths.size() + 2; t += 1 + t % 5) {
			strata::postings_iterator it(inv, info);
			const auto want = std::lower_bound(ps.begin(), ps.end(), t, [](const posting& p, ordinal_t v) { return p.ord < v; });
			const ordinal_t got = it.advance(t);
			ok = ok && got == (want == ps.end() ? strata::no_more_docs : want->ord);
			if (ok && want != ps.end() && !(info.flags & f::no_positions)) {
				strata::positions_reader pr(inv, info);
				const auto at = pr.read(it);
				ok = std::vector<std::uint32_t>(at.begin(), at.end()) == want->positions;
			}
		}
		std::mt19937 rng(7);
		strata::postings_iterator it(inv, info);
		for (ordinal_t t = 0; ok && t < c.lengths.size(); t += 1 + rng() % 700) {
			const auto want = std::lower_bound(ps.begin(), ps.end(), t, [](const posting& p, ordinal_t v) { return p.ord < v; });
			ok = it.advance(t) == (want == ps.end() ? strata::no_more_docs : want->ord) && (want == ps.end() || it.wdf() == want->wdf);
		}
		CHECK(ok);
	}
	{  // block_index_for, block_last, and the pairs of each block and group
		strata::postings_iterator it(inv, info);
		const std::size_t blocks = ps.size() / f::block;
		CHECK(it.blocks() == blocks);
		bool ok = true;
		for (std::size_t b = 0; b < blocks; ++b) {
			ok = ok && it.block_last(b) == ps[b * f::block + f::block - 1].ord;
			ok = ok && same(it.block_impacts_at(b), pairs_of(c, ps, b * f::block, (b + 1) * f::block));
		}
		for (std::size_t g = 0; g < it.groups(); ++g) {
			ok = ok && same(it.group_impacts(g), pairs_of(c, ps, g * 32 * f::block, std::min(blocks, g * 32 + 32) * f::block));
		}
		for (ordinal_t t = 0; blocks && t <= ps[blocks * f::block - 1].ord; ++t) {
			const auto at = std::size_t(std::lower_bound(ps.begin(), ps.end(), t, [](const posting& p, ordinal_t v) { return p.ord < v; }) - ps.begin());
			ok = ok && it.block_index_for(t) == at / f::block;
		}
		CHECK(ok);
	}
	{  // the pruned top-k against an exhaustive one
		strata::term_scorer pruned(inv, info), all(inv, info);
		const auto a = strata::top_k(s, pruned, 10);
		std::vector<strata::hit> b;
		for (ordinal_t d = all.next(); d != strata::no_more_docs; d = all.next()) { b.push_back({d, all.score()}); }
		std::sort(b.begin(), b.end(), [](const strata::hit& x, const strata::hit& y) { return x.score != y.score ? x.score > y.score : x.ord < y.ord; });
		b.resize(std::min<std::size_t>(b.size(), 10));
		bool ok = a.size() == b.size();
		for (std::size_t i = 0; ok && i < a.size(); ++i) { ok = a[i].ord == b[i].ord && a[i].score == b[i].score; }
		CHECK(ok);
	}
}

// Runs the read paths over a possibly damaged term: each must fence (corrupt_segment) or
// answer with ordinals increasing and below the document count, advance() at or past its
// target, and a located block ending at or after its target and its predecessor before it.
// Returns whether a path fenced; sets *bad when a consumer saw a wrong ordinal.
bool walk(const strata::segment& s, const f::term_info& info, bool* bad) {
	const auto& inv = s.get<strata::inverted_plugin>();
	const ordinal_t n = s.core().doc_count();
	bool fenced = false;
	auto guard = [&](const std::function<void()>& fn) {
		try {
			fn();
		} catch (const strata::corrupt_segment&) {
			fenced = true;
		}
	};
	guard([&] {
		strata::postings_iterator it(inv, info);
		ordinal_t prev = 0;
		bool first = true;
		for (ordinal_t d = it.next(); d != strata::no_more_docs; d = it.next()) {
			if (d >= n || (!first && d <= prev)) { *bad = true; }
			prev = d;
			first = false;
		}
	});
	for (ordinal_t step : {1u, 97u, 1000u, 5000u}) {
		guard([&] {
			strata::postings_iterator it(inv, info);
			ordinal_t prev = 0;
			for (ordinal_t t = 0; t < n; t += step) {
				const ordinal_t d = it.advance(t);
				if (d == strata::no_more_docs) { break; }
				if (d < t || d < prev || d >= n) { *bad = true; }
				prev = d;
			}
		});
	}
	guard([&] {
		strata::postings_iterator it(inv, info);
		for (ordinal_t t = 0; t < n; t += 13) {
			const std::size_t b = it.block_index_for(t);
			if (b >= it.blocks()) { continue; }
			if (it.block_last(b) < t || (b && it.block_last(b - 1) >= t)) { *bad = true; }
		}
	});
	guard([&] {
		strata::term_scorer sc(inv, info);
		(void)strata::top_k(s, sc, 10);
		strata::term_scorer counted(inv, info);
		(void)strata::count(counted);
	});
	return fenced;
}

// One damaged copy of a term: edit(file, layout), resealed. Returns whether it fenced.
bool damaged(const corpus& c, const std::string& term, const std::function<void(std::string&, const skip_test::layout&)>& edit, bool* bad) {
	std::string file = c.bytes;
	strata::segment s0(file);
	const auto info = *s0.get<strata::inverted_plugin>().term(term);
	edit(file, skip_test::parse(file, s0, info));
	reseal(file);
	strata::segment s(file);
	return walk(s, info, bad);
}

}  // namespace

int main() {
	// --- F10 (M16): advance entries locate every block -----------------------------------------
	for (const bool wide : {false, true}) {
		const corpus c = build(20000, 11, wide);
		strata::segment s(c.bytes);
		for (const auto& [term, _] : c.terms) { check_term(c, s, term); }
		// The shapes the cases below rely on: two groups of FOR blocks, and bitset blocks.
		const auto mid = skip_test::parse(c.bytes, s, *s.get<strata::inverted_plugin>().term("mid"));
		CHECK(mid.groups.size() == 2 && mid.groups[0].n == 32);
		for (const auto& b : mid.blocks) { CHECK(b.codec == unsigned(f::block_codec::deltas)); }
		const auto dense = skip_test::parse(c.bytes, s, *s.get<strata::inverted_plugin>().term("dense"));
		std::size_t bitsets = 0;
		for (const auto& b : dense.blocks) { bitsets += b.codec == unsigned(f::block_codec::bitset); }
		CHECK(bitsets > 0 && dense.groups[0].wf >= 1);
		const auto clustered = skip_test::parse(c.bytes, s, *s.get<strata::inverted_plugin>().term("clustered"));
		std::size_t efs = 0;
		for (const auto& b : clustered.blocks) { efs += b.codec == unsigned(f::block_codec::elias_fano); }
		CHECK(efs > 0 && clustered.groups[0].wf == 2);
		// Each group's advance entries take (n - 1) lasts and offsets and n codes at the
		// widths level 1 implies, and the last block's last is the group's.
		for (std::size_t i = 0; i < mid.blocks.size(); ++i) { CHECK(mid.blocks[i].last == c.terms.at("mid")[i * 128 + 127].ord); }
	}

	// --- F10 (M16): damaged advance entries fence or answer in order ----------------------------
	{
		const corpus c = build(20000, 12);
		bool bad = false;
		// Block 1's last below block 0's: its decode fences.
		CHECK(damaged(c, "mid", [](std::string& file, const skip_test::layout& l) {
			const auto& g = l.groups[0];
			skip_test::put(file, g.adv, g.bit_last(1), g.wl, l.blocks[0].last - g.base - 1);
		}, &bad));
		// Block 0's last at its field's largest value, past the group's last.
		CHECK(damaged(c, "mid", [](std::string& file, const skip_test::layout& l) {
			const auto& g = l.groups[0];
			skip_test::put(file, g.adv, g.bit_last(0), g.wl, (std::uint64_t(1) << g.wl) - 1);
		}, &bad));
		// Block 2's offset at block 1's: block 1 empty.
		CHECK(damaged(c, "mid", [](std::string& file, const skip_test::layout& l) {
			const auto& g = l.groups[0];
			skip_test::put(file, g.adv, g.bit_offset(2), g.wo, l.blocks[1].at - l.blocks[0].at);
		}, &bad));
		// Block 3's offset past the group's bytes.
		CHECK(damaged(c, "mid", [](std::string& file, const skip_test::layout& l) {
			const auto& g = l.groups[0];
			skip_test::put(file, g.adv, g.bit_offset(3), g.wo, (std::uint64_t(1) << g.wo) - 1);
		}, &bad));
		// A code no format-13 codec has (3, at width 2), on a FOR block that would decode.
		CHECK(damaged(c, "clustered", [](std::string& file, const skip_test::layout& l) {
			std::size_t i = 0;
			while (i + 1 < l.blocks.size() && l.blocks[i].codec != unsigned(f::block_codec::deltas)) { ++i; }
			CHECK(l.blocks[i].codec == unsigned(f::block_codec::deltas));
			const auto& g = l.groups[i / 32];
			skip_test::put(file, g.adv, g.bit_codec(i % 32), g.wf, 3);
		}, &bad));
		// Elias-Fano blocks read as FOR, and FOR blocks as Elias-Fano.
		for (unsigned code : {0u, 2u}) {
			(void)damaged(c, "clustered", [&](std::string& file, const skip_test::layout& l) {
				for (std::size_t i = 0; i < l.blocks.size(); ++i) {
					const auto& g = l.groups[i / 32];
					if (l.blocks[i].codec != code) { skip_test::put(file, g.adv, g.bit_codec(i % 32), g.wf, code); }
				}
			}, &bad);
		}
		// A bitset block read as FOR deltas.
		CHECK(damaged(c, "dense", [](std::string& file, const skip_test::layout& l) {
			const auto& g = l.groups[0];
			skip_test::put(file, g.adv, g.bit_codec(1), g.wf, 0);
		}, &bad));
		// The skip flags: F12's reserved partition bit, an unknown bit, a codec width this
		// format lacks.
		for (const std::uint8_t set : {std::uint8_t(f::skip::partitioned), std::uint8_t(0x10), std::uint8_t(0x80)}) {
			CHECK(damaged(c, "mid", [&](std::string& file, const skip_test::layout& l) { file[l.flags_at] = char(std::uint8_t(file[l.flags_at]) | set); }, &bad));
		}
		CHECK(damaged(c, "mid", [&](std::string& file, const skip_test::layout& l) {
			file[l.flags_at] = char((std::uint8_t(file[l.flags_at]) & ~6) | (std::bit_width(unsigned(f::max_block_codec)) + 1) << f::skip::codec_shift);
		}, &bad));
		CHECK(!bad);
		// Every bit of the two groups' advance entries flipped in turn: fence or answer.
		strata::segment s0(c.bytes);
		for (const char* term : {"mid", "dense", "dense0", "gaps", "clustered"}) {
			const auto info = *s0.get<strata::inverted_plugin>().term(term);
			const auto l = skip_test::parse(c.bytes, s0, info);
			std::size_t flips = 0, fenced = 0;
			for (const auto& g : l.groups) {
				const std::uint64_t bits = f::advance_bytes(g.n, g.wl, g.wo, g.wf) * 8;
				for (std::uint64_t at = 0; at < bits; ++at) {
					std::string file = c.bytes;
					file[g.adv + at / 8] ^= char(1u << (at % 8));
					reseal(file);
					strata::segment s(file);
					fenced += walk(s, info, &bad);
					++flips;
				}
			}
			std::printf("  %s: %zu advance-entry bits flipped, %zu fenced\n", term, flips, fenced);
			CHECK(flips > 0 && fenced > 0);
		}
		CHECK(!bad);
	}

	// --- F10 (M16): a block made consistent with a last past the document count --------------
	// Three bitset blocks without wdf (gaps 1, 1, 3), the last posting the segment's last
	// document. Block 1's entry raised to the document count, its bits and bytes rewritten to
	// match: only the entry's check against its group's last fences it.
	{
		std::vector<ordinal_t> ords{0};
		for (std::size_t k = 1; k < 384; ++k) { ords.push_back(ords.back() + (k % 3 == 2 ? 3 : 1)); }
		strata::segment_writer w;
		for (ordinal_t d = 0, i = 0; d <= ords.back(); ++d) {
			std::vector<strata::posting_input> in;
			const std::vector<std::uint32_t> none;
			if (d == ords[i]) {
				in.push_back({"t", 0, none});
				++i;
			}
			w.add(d + 1, in, 1);
		}
		std::string file = w.flush({});
		strata::segment s0(file);
		const auto info = *s0.get<strata::inverted_plugin>().term("t");
		const auto l = skip_test::parse(file, s0, info);
		const auto& g = l.groups[0];
		CHECK(l.blocks.size() == 3 && g.last + 1 == s0.core().doc_count());
		for (const auto& b : l.blocks) { CHECK(b.codec == unsigned(f::block_codec::bitset)); }
		const ordinal_t first = l.blocks[0].last + 1, last = g.last + 1;
		const std::size_t bytes = (last - first + 1 + 7) / 8;
		skip_test::put(file, g.adv, g.bit_last(1), g.wl, last - g.base);
		skip_test::put(file, g.adv, g.bit_offset(2), g.wo, l.blocks[1].at - l.blocks[0].at + bytes);
		std::string bits(bytes, '\0');
		for (std::size_t k = 0; k < 127; ++k) { bits[k / 8] = char(std::uint8_t(bits[k / 8]) | 1u << (k % 8)); }
		bits[(last - first) / 8] = char(std::uint8_t(bits[(last - first) / 8]) | 1u << ((last - first) % 8));
		file.replace(l.blocks[1].at, bytes, bits);
		reseal(file);
		strata::segment s(file);
		bool bad = false;
		CHECK(walk(s, info, &bad));
		CHECK(!bad);
	}

	// --- F11 (M23): gaps - 1, checked by the header width or a 64-bit sum ---------------------
	{
		std::mt19937_64 rng(23);
		std::uint32_t out[f::block];
		std::uint8_t buf[bitpack::max_encoded_bytes];
		// Round trips at every width, from a term's first block and from a base.
		for (unsigned width = 0; width <= 32; ++width) {
			for (const bool first : {true, false}) {
				const std::uint32_t base = first ? 0 : std::uint32_t(rng() % 1000);
				std::uint32_t ords[f::block], at = first ? std::uint32_t(-1) : base;
				const std::uint64_t cap = width ? (std::uint64_t(1) << width) - 1 : 0;
				const std::uint64_t room = (0xffffffffull - (first ? 0 : base)) / f::block - 1;  // gaps that stay below 2^32
				for (std::size_t i = 0; i < f::block; ++i) { ords[i] = at += std::uint32_t(1 + std::min<std::uint64_t>(i == 7 ? cap : rng() & cap, room)); }
				const std::size_t n = f::encode_gaps(ords, base, first, buf);
				CHECK(f::decode_gaps(buf, n, base, ords[f::block - 1], first, out) == n);
				CHECK(std::equal(ords, ords + f::block, out));
				// A last other than the block's: refused.
				CHECK(f::decode_gaps(buf, n, base, ords[f::block - 1] - 1, first, out) == 0);
				CHECK(f::decode_gaps(buf, n - 1, base, ords[f::block - 1], first, out) == 0);  // truncated
			}
		}
		// Consecutive ordinals: a 5-byte constant block of 0.
		std::uint32_t run[f::block];
		for (std::size_t i = 0; i < f::block; ++i) { run[i] = std::uint32_t(500 + i); }
		CHECK(f::encode_gaps(run, 499, false, buf) == 5 && buf[0] == bitpack::constant_flag && f::decode_gaps(buf, 5, 499, 627, false, out) == 5);
		CHECK(f::decode_gaps(buf, 5, 498, 627, false, out) == 0);  // a constant inconsistent with the span
		// Gaps that wrap by 2^32 and land on the entry's last: two gaps of 2^31, then 126 of
		// 1. The last value matches, so only the 64-bit sum (header width 31 > 25) refuses it.
		{
			std::uint32_t g[f::block] = {};
			g[0] = g[1] = (1u << 31) - 1;
			std::uint8_t wide[1 + bitpack::packed_bytes(32)];
			const std::size_t n = bitpack::encode_for(g, wide);
			CHECK(wide[0] == 31);
			const std::uint32_t base = 100, last = base + 126;
			std::uint32_t at = base;
			for (auto v : g) { at += v + 1; }
			CHECK(at == last);
			CHECK(f::decode_gaps(wide, n, base, last, false, out) == 0);
			// The same width with an exact sum decodes.
			for (std::size_t i = 0; i < f::block; ++i) { g[i] = i == 5 ? (1u << 30) : 0; }
			const std::size_t m = bitpack::encode_for(g, wide);
			CHECK(wide[0] == 31 && f::decode_gaps(wide, m, base, base + (1u << 30) + f::block, false, out) == m);
			CHECK(out[4] == base + 5 && out[5] == base + 6 + (1u << 30) && out[f::block - 1] == base + (1u << 30) + f::block);
		}
		// An exception block (PFOR) is not an ordinal block, even where its low part alone
		// (width 0: every gap 1) would end at the entry's last.
		std::uint32_t g[f::block] = {};
		g[3] = 1u << 20;
		const std::size_t n = bitpack::encode_pfor(g, buf);
		CHECK(buf[0] == bitpack::exceptions_flag);
		CHECK(f::decode_gaps(buf, n, 0, f::block, false, out) == 0);
		CHECK(f::decode_gaps(buf, n, 0, (1u << 20) + f::block, false, out) == 0);
	}

	// --- F11 (M22): Elias-Fano blocks --------------------------------------------------------
	{
		std::mt19937_64 rng(22);
		std::uint32_t out[f::block];
		std::uint8_t buf[2048];
		std::size_t chosen = 0;
		for (int trial = 0; trial < 400; ++trial) {
			const bool first = trial % 3 == 0;
			const std::uint32_t base = first ? 0 : std::uint32_t(rng() % 100000);
			std::uint32_t ords[f::block], at = first ? std::uint32_t(-1) : base;
			const unsigned shape = trial % 4;  // runs with jumps, uniform, one far last, consecutive
			for (std::size_t i = 0; i < f::block; ++i) {
				const std::uint32_t gap = shape == 0 ? (rng() % 8 == 0 ? 1 + std::uint32_t(rng() % 50000) : 1)
					: shape == 1 ? 1 + std::uint32_t(rng() % (1u << (trial % 20)))
					: shape == 2 ? (i == f::block - 1 ? 1u << 28 : 1) : 1;
				ords[i] = at += gap;
			}
			const std::uint32_t last = ords[f::block - 1];
			const std::uint64_t u = f::ef::span(base, last, first);
			const std::size_t n = f::ef::encode(ords, base, first, buf);
			CHECK(n == f::ef::bytes(u));
			CHECK(f::ef::decode(buf, base, last, first, out) && std::equal(ords, ords + f::block, out));
			CHECK(f::ef::probe_ready(buf, base, last, first));
			// The successor of every ordinal in range, and of each block value, is the model's.
			bool ok = true;
			const std::uint32_t from = first ? 0 : base + 1;
			for (std::uint64_t t = from; t <= last; t += 1 + (last - from) / 3000) {
				const auto want = std::size_t(std::lower_bound(ords, ords + f::block, std::uint32_t(t)) - ords);
				const auto [i, d] = f::ef::successor(buf, base, last, first, std::uint32_t(t));
				ok = ok && i == want && d == ords[want];
			}
			for (std::size_t k = 0; k < f::block; ++k) {
				const auto [i, d] = f::ef::successor(buf, base, last, first, ords[k]);
				ok = ok && i == k && d == ords[k];
			}
			CHECK(ok);
			std::uint8_t gaps[bitpack::max_encoded_bytes];
			chosen += n < f::encode_gaps(ords, base, first, gaps);
			// Damage: a high bit cleared, a duplicate low part, the last moved, a short span.
			std::uint8_t bad[2048];
			const std::size_t lowb = bitpack::packed_bytes(f::ef::low_bits(u));
			std::memcpy(bad, buf, n);
			for (std::size_t k = lowb; k < n; ++k) {
				if (bad[k]) { bad[k] = std::uint8_t(bad[k] & (bad[k] - 1)); break; }
			}
			CHECK(!f::ef::decode(bad, base, last, first, out) && !f::ef::probe_ready(bad, base, last, first));
			CHECK(!f::ef::decode(buf, base, last - 1, first, out) && !f::ef::probe_ready(buf, base, last + 1, first));
			if (!first) { CHECK(!f::ef::decode(buf, last - 100, last, false, out) && !f::ef::probe_ready(buf, last - 100, last, false)); }
		}
		CHECK(chosen > 50);  // shapes where Elias-Fano is smaller than FOR at gap - 1
		// Two equal ordinals: one high bit moved onto its neighbor's bucket with the same low part.
		std::uint32_t ords[f::block];
		for (std::size_t i = 0; i < f::block; ++i) { ords[i] = std::uint32_t(10 * i + 3); }
		const std::size_t n = f::ef::encode(ords, 0, true, buf);
		std::uint32_t dup[f::block];
		std::copy(ords, ords + f::block, dup);
		dup[5] = dup[4];
		std::uint8_t bad[2048];
		CHECK(f::ef::encode(dup, 0, true, bad) == n);
		CHECK(!f::ef::decode(bad, 0, ords[f::block - 1], true, out));
		CHECK(f::ef::probe_ready(bad, 0, ords[f::block - 1], true));  // the probe's check passes: decode is the one that refuses
		// An ordinal past the last in the last's bucket: the probe refuses to answer it.
		std::copy(ords, ords + f::block, dup);
		dup[126] = 1279;  // bucket 159 at 3 low bits, as the last (1273), with a larger low part
		CHECK(f::ef::encode(dup, 0, true, bad) == n && f::ef::probe_ready(bad, 0, 1273, true));
		CHECK(f::ef::successor(bad, 0, 1273, true, 1273).first == f::block);
	}

	// --- F13 (M20): threshold seeds -----------------------------------------------------------
	{
		// The stored pairs are the best dominance pairs for each preset at the segment's own
		// statistics (E4b's seeds), and each dominates its k postings.
		const corpus c = build(20000, 20);
		strata::segment s(c.bytes);
		const auto& inv = s.get<strata::inverted_plugin>();
		const strata::statistics st(inv);
		for (const auto& [term, ps] : c.terms) {
			const auto info = *inv.term(term);
			strata::postings_iterator it(inv, info);
			const bool stored = ps.size() >= f::block && !(info.flags & f::no_wdf);
			CHECK(it.seed_count() == (stored ? std::size_t(std::count_if(std::begin(f::seed_ks), std::end(f::seed_ks), [&](std::uint32_t k) { return k <= ps.size(); })) : 0));
			const auto lucene = strata::weight::bm25(st, info.df), xapian = strata::weight::bm25_xapian(st, info.df);
			for (std::size_t j = 0; j < it.seed_count(); ++j) {
				const std::size_t k = f::seed_ks[j];
				double best_l = 0, best_x = 0;  // over every w: the k-th shortest length among wdf >= w
				for (const auto& p : ps) {
					std::vector<std::uint32_t> lens;
					for (const auto& q : ps) {
						if (q.wdf >= p.wdf) { lens.push_back(c.lengths[q.ord]); }
					}
					if (lens.size() < k) { continue; }
					std::nth_element(lens.begin(), lens.begin() + std::ptrdiff_t(k - 1), lens.end());
					best_l = std::max(best_l, lucene.by_norm(p.wdf, f::encode_length(lens[k - 1])));
					best_x = std::max(best_x, xapian(p.wdf, lens[k - 1]));
				}
				const auto& pair = it.seed_at(j);
				CHECK(lucene.by_norm(pair.code_wdf, pair.code) == best_l);
				CHECK(xapian(pair.exact_wdf, pair.exact) == best_x);
				std::size_t by_code = 0, exact = 0;
				for (const auto& p : ps) {
					by_code += p.wdf >= pair.code_wdf && f::encode_length(c.lengths[p.ord]) <= pair.code;
					exact += p.wdf >= pair.exact_wdf && c.lengths[p.ord] <= pair.exact;
				}
				CHECK(by_code >= k && exact >= k);
			}
		}
		strata::check_report r;
		strata::check_segment(s, nullptr, r);
		CHECK(r.terms == c.terms.size());
		// check() refuses a seed that claims more than its postings hold.
		{
			std::string file = c.bytes;
			const strata::segment copy(file);
			const auto info = *copy.get<strata::inverted_plugin>().term("mid");
			const auto l = skip_test::parse(file, copy, info);
			const char* p = file.data() + l.seeds_at;
			(void)f::get_leb(&p, file.data() + file.size());
			file[std::size_t(p - file.data())] = char(255);  // the first pair's code: every length
			const char* q = file.data() + l.seeds_at;
			const std::uint64_t w = f::get_leb(&q, file.data() + file.size());
			CHECK(w < 0x7f);
			file[l.seeds_at] = char(0x7f);  // and a wdf no posting reaches
			reseal(file);
			strata::segment bad(file);
			strata::check_report r2;
			bool refused = false;
			try {
				strata::check_segment(bad, nullptr, r2);
			} catch (const strata::corrupt_segment&) {
				refused = true;
			}
			CHECK(refused);
		}

		// Searches with seeds equal an exhaustive reference: terms and unions, k = 1, 10, 50,
		// 100, 1,000, both presets, one segment, three, and three with deletes (seeds off).
		const corpus parts[3] = {build(6000, 31, false, 1), build(6000, 32, false, 100000), build(6000, 33, false, 200000)};
		std::vector<strata::segment> segs;
		for (const auto& p : parts) { segs.emplace_back(p.bytes); }
		std::vector<std::uint64_t> deleted((6000 + 63) / 64);
		for (std::size_t d = 0; d < 6000; d += 3) { deleted[d / 64] |= std::uint64_t(1) << (d % 64); }
		using query = std::vector<std::string>;
		const std::vector<query> queries = {{"dense"}, {"mid"}, {"sparse"}, {"counts"}, {"clustered"}, {"mid", "sparse"}, {"dense", "counts", "gaps"}, {"bool", "mid"}};
		std::size_t seeded = 0, compared = 0;
		for (const int shape : {1, 3, 4}) {  // segments, the fourth: three with deletes in the second
			std::vector<strata::segment_snapshot> snap;
			for (int i = 0; i < std::min(shape, 3); ++i) { snap.push_back({&segs[std::size_t(i)], shape == 4 && i == 1 ? &deleted : nullptr}); }
			strata::term_table table(snap);
			for (const auto scheme : {strata::scheme::bm25, strata::scheme::bm25_xapian}) {
				for (const auto& q : queries) {
					auto build_q = [&](const strata::segment& seg) -> strata::scorer_ptr {
						std::vector<strata::scorer_ptr> kids;
						for (const auto& t : q) {
							const auto* info = table.info(seg, t);
							if (!info) { continue; }
							const auto w = scheme == strata::scheme::bm25 ? strata::weight::bm25(table.stats(), table.df(t)) : strata::weight::bm25_xapian(table.stats(), table.df(t));
							kids.push_back(std::make_unique<strata::term_scorer>(seg.get<strata::inverted_plugin>(), *info, w));
						}
						if (kids.empty()) { return nullptr; }
						if (kids.size() == 1) { return std::move(kids[0]); }
						return std::make_unique<strata::disjunction_scorer>(std::move(kids));
					};
					for (const std::size_t k : {1u, 10u, 50u, 100u, 1000u}) {
						std::vector<strata::doc_hit> want;
						for (const auto& sn : snap) {
							auto sc = build_q(*sn.seg);
							if (!sc) { continue; }
							if (sn.deleted == nullptr) { seeded += sc->seed(k) > 0; }
							for (ordinal_t d = sc->next(); d != strata::no_more_docs; d = sc->next()) {
								if (sn.deleted && ((*sn.deleted)[d / 64] >> (d % 64) & 1)) { continue; }
								want.push_back({sn.seg->core().docid(d), sc->score()});
							}
						}
						std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
						want.resize(std::min(want.size(), k));
						const auto got = strata::search(snap, build_q, k);
						bool ok = got.size() == want.size();
						for (std::size_t i = 0; ok && i < got.size(); ++i) { ok = got[i].docid == want[i].docid && got[i].score == want[i].score; }
						CHECK(ok);
						++compared;
					}
				}
			}
		}
		std::printf("  seeds: %zu searches equal the exhaustive reference, %zu seeded scorers\n", compared, seeded);
		CHECK(seeded > 50);

		// Seeds are off in a segment with deletes: here the k-th best stored pair belongs to
		// deleted postings, so a seeded floor would drop every live hit.
		{
			strata::segment_writer w;
			const std::vector<std::uint32_t> none;
			for (std::uint32_t d = 0; d < 400; ++d) {
				std::vector<strata::posting_input> in{{"hot", d < 40 ? 50u : 1u, none}};
				w.add(d + 1, in, d < 40 ? 2 : 200);
			}
			const std::string bytes = w.flush({});
			strata::segment hot(bytes);
			std::vector<std::uint64_t> gone((400 + 63) / 64);
			for (std::size_t d = 0; d < 40; ++d) { gone[d / 64] |= std::uint64_t(1) << (d % 64); }
			const strata::segment_snapshot one[] = {{&hot, &gone}};
			auto b = [&](const strata::segment& seg) -> strata::scorer_ptr { return std::make_unique<strata::term_scorer>(seg.get<strata::inverted_plugin>(), *seg.get<strata::inverted_plugin>().term("hot")); };
			strata::term_scorer probe(hot.get<strata::inverted_plugin>(), *hot.get<strata::inverted_plugin>().term("hot"));
			CHECK(probe.seed(10) > strata::weight::bm25(hot.get<strata::inverted_plugin>(), 400)(1, 200));  // the seed comes from the deleted postings
			const auto got = strata::search(one, b, 10);
			CHECK(got.size() == 10);
			for (const auto& h : got) { CHECK(h.docid > 40); }
		}
	}

	// --- M3t: bound-ordered traversal of a single term in a reordered segment ------------------
	// Permuted docids (a reordering merge's output): high-wdf documents gathered into a few
	// groups, ties by docid. top_k and search() against an exhaustive reference, both presets,
	// with and without seeds.
	{
		std::mt19937_64 rng(3);
		std::vector<std::uint64_t> ids(20000);
		for (std::uint64_t d = 0; auto& id : ids) { id = d += 1 + rng() % 5; }
		std::shuffle(ids.begin(), ids.end(), rng);
		strata::segment_writer w;
		w.permuted = true;
		const std::vector<std::uint32_t> none;
		for (std::uint32_t d = 0; d < 20000; ++d) {
			std::vector<strata::posting_input> in;
			const bool hot = (d >= 12000 && d < 12600) || (d >= 3000 && d < 3100);
			in.push_back({"skew", hot ? 5 + std::uint32_t(rng() % 20) : 1 + std::uint32_t(rng() % 3), none});
			if (d % 3 == 0) { in.push_back({"third", 1 + std::uint32_t(rng() % 4), none}); }
			if (d % 2 == 0 && rng() % 4 == 0) { in.push_back({"tie", 1, none}); }  // equal scores at equal lengths
			w.add(ids[d], in, hot ? 20 : 10 + std::uint32_t(rng() % 3));
		}
		const std::string bytes = w.flush({});
		strata::segment seg(bytes);
		CHECK(!seg.core().docid_ordered());
		const auto& inv = seg.get<strata::inverted_plugin>();
		std::size_t compared = 0;
		for (const char* term : {"skew", "third", "tie"}) {
			const auto info = *inv.term(term);
			for (const bool xapian : {false, true}) {
				const auto wt = xapian ? strata::weight::bm25_xapian(inv, info.df) : strata::weight::bm25(inv, info.df);
				std::vector<strata::doc_hit> all;
				strata::term_scorer ref(inv, info, wt);
				for (ordinal_t d = ref.next(); d != strata::no_more_docs; d = ref.next()) { all.push_back({seg.core().docid(d), ref.score()}); }
				std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
				for (const std::size_t k : {1u, 10u, 100u, 1000u}) {
					for (const bool seeded : {false, true}) {
						strata::term_scorer sc(inv, info, wt);
						const auto got = strata::top_k(seg, sc, k, nullptr, seeded ? sc.seed(k) : 0.0);
						bool ok = got.size() == std::min(k, all.size());
						for (std::size_t i = 0; ok && i < got.size(); ++i) { ok = seg.core().docid(got[i].ord) == all[i].docid && got[i].score == all[i].score; }
						CHECK(ok);
						++compared;
					}
					const strata::segment_snapshot one[] = {{&seg, nullptr}};
					const auto got = strata::search(one, [&](const strata::segment& sg) { return std::make_unique<strata::term_scorer>(sg.get<strata::inverted_plugin>(), info, wt); }, k);
					bool ok = got.size() == std::min(k, all.size());
					for (std::size_t i = 0; ok && i < got.size(); ++i) { ok = got[i].docid == all[i].docid && got[i].score == all[i].score; }
					CHECK(ok);
				}
			}
		}
		std::printf("  M3t: %zu reordered top-k equal the exhaustive reference\n", compared);
	}

	std::printf("postings: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
