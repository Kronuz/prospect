// Format 13's postings items (docs/design/format-13.md F10, F11, F13): the advance entries
// (M16, A3 at 128 postings), against a model of every term's postings, positions, and
// pairs; and corrupt entries, which fence or answer with ordinals in order and in range.
#include "strata/core/reader.hh"
#include "strata/search/search.hh"
#include "strata/search/searcher.hh"
#include "strata/core/check.hh"
#include "strata/core/writer.hh"
#include "skip_layout.hh"

#include <crc32c/crc32c.hh>
#include <algorithm>
#include <array>
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
// sparse, clustered runs, boolean, irregular gaps with runs, and counts
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
		// Runs and long gaps, and one long run (FOR constants).
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
	ft.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
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
		// No block is Elias-Fano: code 2 is reserved (M22), so the codec width stays within 1.
		const auto clustered = skip_test::parse(c.bytes, s, *s.get<strata::inverted_plugin>().term("clustered"));
		for (const auto& b : clustered.blocks) { CHECK(b.codec <= f::max_block_codec); }
		CHECK(clustered.groups[0].wf <= 1);
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
		// A codec width past the kept codes: width 2 is the reserved Elias-Fano code's (M22).
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

	// --- F11: the bitset boundary stays format 12's -----------------------------------------
	// A block of gaps 1 and 2 is FOR at width 1 over gaps - 1 (17 bytes) but a bitset (18 bytes)
	// against FOR over plain gaps at width 2 (33): the writer keeps the bitset, which counting ORs
	// in without decoding.
	{
		strata::segment_writer w;
		const std::vector<std::uint32_t> none;
		for (std::uint32_t d = 0; d < 3000; ++d) {
			std::vector<strata::posting_input> in;
			if (d % 10 != 0) { in.push_back({"ninety", 0, none}); }
			w.add(d + 1, in, 1);
		}
		const std::string bytes = w.flush({});
		strata::segment s(bytes);
		const auto l = skip_test::parse(bytes, s, *s.get<strata::inverted_plugin>().term("ninety"));
		bool all = !l.blocks.empty();
		for (const auto& b : l.blocks) { all = all && b.codec == unsigned(f::block_codec::bitset); }
		CHECK(all);
	}

	// --- F11 (M22's place): codec code 2 is reserved and refused ------------------------------
	// A term with a FOR block and room in its advance entries' last byte, rewritten by hand to a
	// codec width of 2 with that block's code 2 (Elias-Fano's) and the others' codes kept: every
	// other byte is as written, so only the width check stands between it and a FOR decode.
	{
		strata::segment_writer w;
		const std::vector<std::uint32_t> none;
		std::mt19937_64 rng(2);
		std::vector<std::string> names;
		for (std::uint32_t t = 0; t < 24; ++t) { names.push_back("t" + std::to_string(t)); }
		for (std::uint32_t d = 0; d < 20000; ++d) {
			std::vector<strata::posting_input> in;
			for (std::uint32_t t = 0; t < 24; ++t) {  // term t within the first 1,000 + 250t documents
				if (d < 1000 + 250 * t && rng() % (5 + t % 4) == 0) { in.push_back({names[t], 1, none}); }
			}
			w.add(d + 1, in, 1 + d % 50);
		}
		std::string file = w.flush({});
		strata::segment s0(file);
		const auto& inv0 = s0.get<strata::inverted_plugin>();
		std::string term;
		skip_test::layout l;
		std::size_t target = 0;
		for (std::uint32_t t = 0; t < 24 && term.empty(); ++t) {
			const auto info = inv0.term(names[t]);
			if (!info || info->df < f::block) { continue; }
			const auto cand = skip_test::parse(file, s0, *info);
			const auto& g = cand.groups[0];
			if (g.wf > 1 || f::advance_bytes(g.n, g.wl, g.wo, 2) != f::advance_bytes(g.n, g.wl, g.wo, g.wf)) { continue; }
			for (std::size_t i = 0; i < g.n && term.empty(); ++i) {
				if (cand.blocks[i].codec == unsigned(f::block_codec::deltas)) {
					term = names[t];
					l = cand;
					target = i;
				}
			}
		}
		CHECK(!term.empty());
		if (!term.empty()) {
			const auto& g = l.groups[0];
			file[l.flags_at] = char((std::uint8_t(file[l.flags_at]) & ~6) | 2 << f::skip::codec_shift);
			for (std::size_t i = 0; i < g.n; ++i) {
				skip_test::put(file, g.adv, (g.n - 1) * (g.wl + g.wo) + 2 * i, 2, i == target ? 2 : l.blocks[i].codec);
			}
			reseal(file);
			strata::segment s(file);
			const auto info = *s.get<strata::inverted_plugin>().term(term);
			bool fenced = false;
			try {
				strata::postings_iterator it(s.get<strata::inverted_plugin>(), info);
				while (it.next() != strata::no_more_docs) {}
			} catch (const strata::corrupt_segment&) {
				fenced = true;
			}
			CHECK(fenced);  // a format-13 reader without M22 refuses code 2 as damage
		}
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
		// The stored pairs are exactly the writer's rule: levels of wdf highest first, each k's
		// k-th shortest at a level, the first strictly best kept (scores compared as exact
		// rationals, ties to the higher wdf), stopping where no lower wdf can beat every best at
		// the segment's shortest length. Lengths reach past 65,536, where the writer's exact
		// counts end, and the k-th shortest lands past it.
		{
			std::mt19937_64 rng(77);
			strata::segment_writer w;
			std::map<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>> terms;  // (wdf, length)
			std::vector<std::uint32_t> lengths;
			for (std::uint32_t d = 0; d < 6000; ++d) {
				const std::uint32_t r = std::uint32_t(rng() % 100);
				const std::uint32_t length = r < 5 ? 65536 + std::uint32_t(rng() % 200000) : r < 8 ? 65520 + std::uint32_t(rng() % 32) : 1 + std::uint32_t(rng() % 3000);
				std::vector<strata::posting_input> in;
				auto add = [&](const char* t, std::uint32_t wdf) {
					in.push_back({t, wdf, {}});
					terms[t].push_back({wdf, length});
				};
				add("all", 1 + std::uint32_t(rng() % 3));
				if (length >= 65520) { add("long", 1 + std::uint32_t(rng() % 2)); }  // the k-th shortest past the cap
				if (rng() % 3 == 0) { add("skewed", rng() % 50 == 0 ? 1 + std::uint32_t(rng() % 70000) : 1 + std::uint32_t(rng() % 4)); }
				if (rng() % 2 == 0) { add("zeros", std::uint32_t(rng() % 3)); }
				lengths.push_back(length);
				w.add(1 + d, in, length);
			}
			const std::string bytes = w.flush({});
			strata::segment s2(bytes);
			const auto& inv2 = s2.get<strata::inverted_plugin>();
			std::uint64_t total = 0;
			for (auto l : lengths) { total += l; }
			// bm25: w / (w + 1.2·(0.25 + 0.75·len / avg)), avg = T / N here (T >= N), which orders as
			// w / (w + (T + 3·len·N)); bm25_xapian: w / (w + 0.5 + 0.5·max(len / avg, 0.5)), as
			// w / (w + 2T + max(2·len·N, T)). One score beats another when w1·n2 > w2·n1.
			using u128 = unsigned __int128;
			const u128 T = total, N = lengths.size();
			CHECK(total >= lengths.size());
			auto n_lucene = [&](std::uint32_t l) { return T + 3 * u128(f::decode_length(f::encode_length(l))) * N; };
			auto n_xapian = [&](std::uint32_t l) { return 2 * T + std::max(2 * u128(l) * N, T); };
			auto beats = [](std::uint32_t w1, u128 n1, std::uint32_t w2, u128 n2) { return w2 == 0 || u128(w1) * n2 > u128(w2) * n1; };
			const std::uint32_t shortest = *std::min_element(lengths.begin(), lengths.end());
			for (const auto& [term, ps] : terms) {
				std::vector<std::uint32_t> levels;
				for (const auto& p : ps) { levels.push_back(p.first); }
				std::sort(levels.begin(), levels.end(), std::greater<>());
				levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
				std::array<f::seed_pairs, 3> want{};
				std::uint32_t bw_l[3] = {}, bw_x[3] = {};
				u128 bn_l[3] = {}, bn_x[3] = {};
				const std::size_t ks = std::size_t(std::count_if(std::begin(f::seed_ks), std::end(f::seed_ks), [&](std::uint32_t k) { return k <= ps.size(); }));
				for (std::size_t li = 0; li < levels.size() && levels[li] > 0; ++li) {
					const std::uint32_t lw = levels[li];
					std::vector<std::uint32_t> lens;
					for (const auto& p : ps) {
						if (p.first >= lw) { lens.push_back(p.second); }
					}
					bool full = true;
					for (std::size_t j = 0; j < ks; ++j) {
						const std::size_t k = f::seed_ks[j];
						if (lens.size() < k) {
							full = false;
							continue;
						}
						std::nth_element(lens.begin(), lens.begin() + std::ptrdiff_t(k - 1), lens.end());
						const std::uint32_t l = lens[k - 1];
						if (beats(lw, n_lucene(l), bw_l[j], bn_l[j])) { bw_l[j] = lw, bn_l[j] = n_lucene(l), want[j].code_wdf = lw, want[j].code = f::encode_length(l); }
						if (beats(lw, n_xapian(l), bw_x[j], bn_x[j])) { bw_x[j] = lw, bn_x[j] = n_xapian(l), want[j].exact_wdf = lw, want[j].exact = l; }
					}
					if (full && li + 1 < levels.size()) {
						const std::uint32_t next = levels[li + 1];
						bool done = true;
						for (std::size_t j = 0; j < ks; ++j) { done = done && !beats(next, n_lucene(shortest), bw_l[j], bn_l[j]) && !beats(next, n_xapian(shortest), bw_x[j], bn_x[j]); }
						if (done) { break; }
					}
				}
				strata::postings_iterator it(inv2, *inv2.term(term));
				CHECK(it.seed_count() == ks);
				for (std::size_t j = 0; j < it.seed_count(); ++j) {
					const auto& got = it.seed_at(j);
					CHECK(got.code_wdf == want[j].code_wdf && got.code == want[j].code && got.exact_wdf == want[j].exact_wdf && got.exact == want[j].exact);
				}
			}
			// "long"'s 100th shortest is past the cap; "skewed" has a wdf past 65,536 (the sort).
			std::size_t below = 0, high = 0;
			for (const auto& p : terms["long"]) { below += p.second < 65536; }
			for (const auto& p : terms["skewed"]) { high += p.first > 65536; }
			CHECK(terms["long"].size() >= 128 && below < 100 && high > 0);
		}
		// An exact tie keeps the higher wdf. Average length 9 (2,502 over 278 documents): bm25
		// orders as w / (w + 9 + 3·len), so wdf 3 at length 3 and wdf 2 at length 1 score 3/21
		// both; one empty document keeps the stop rule from ending at wdf 3.
		{
			strata::segment_writer w;
			std::uint64_t docid = 1;
			auto doc = [&](const char* t, std::uint32_t wdf, std::uint32_t length) {
				std::vector<strata::posting_input> in;
				if (t) { in.push_back({t, wdf, {}}); }
				w.add(docid++, in, length);
			};
			for (int i = 0; i < 10; ++i) { doc("tie", 3, 3); }
			for (int i = 0; i < 10; ++i) { doc("tie", 2, 1); }
			for (int i = 0; i < 110; ++i) { doc("tie", 1, 1); }
			doc(nullptr, 0, 0);
			for (int i = 0; i < 147; ++i) { doc("filler", 1, 16); }
			const std::string bytes = w.flush({});
			strata::segment s3(bytes);
			const auto& inv3 = s3.get<strata::inverted_plugin>();
			CHECK(inv3.doc_count() == 278 && inv3.total_length() == 2502);
			strata::postings_iterator it(inv3, *inv3.term("tie"));
			CHECK(it.seed_count() == 2);
			CHECK(it.seed_at(0).code_wdf == 3 && it.seed_at(0).code == f::encode_length(3));
		}
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
