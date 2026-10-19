// Format 13's postings items (docs/design/format-13.md F10, F11, F13): the advance entries
// (M16, A3 at 128 postings), against a model of every term's postings, positions, and
// pairs; and corrupt entries, which fence or answer with ordinals in order and in range.
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/writer.hh"
#include "skip_layout.hh"

#include <cstdio>
#include <cstdlib>
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
// sparse, boolean, irregular gaps with runs, and counts that differ from wdf; lengths 1 to 300.
corpus build(std::uint32_t docs, std::uint64_t seed, bool wide = false) {
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
		if (d % 2 == 0) { add("bool", 0, 0); }
		if (rng() % 7 == 0 || (d / 1000) % 5 == 1) { add("gaps", 1 + std::uint32_t(rng() % 9), 0); }
		if (d % 4 == 1) {
			const std::uint32_t n = 1 + std::uint32_t(rng() % 3);
			add("counts", 3 * n, n);
		}
		c.lengths.push_back(length);
		w.add(d + 1, in, length);
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
		CHECK(bitsets > 0 && dense.groups[0].wf == 1);
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
		for (const char* term : {"mid", "dense", "dense0", "gaps"}) {
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

	std::printf("postings: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
