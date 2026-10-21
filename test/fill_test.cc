// postings_iterator::fill, the counting windows' bit fill: every block kind and window edge
// against a model of the term's ordinals, the fast paths against the fallback (this file is
// built twice, the second with PROSPECT_FILL_SLOW, which turns the fast paths off), and
// damaged blocks, which must fence or answer without a write outside the window's bits.
// Asserts stay on here whatever the build type: they state fill's bounds.
#undef NDEBUG
#include "prospect/reader.hh"
#include "prospect/writer.hh"
#include "skip_layout.hh"

#include <crc32c/crc32c.hh>
#include <algorithm>
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

namespace prospect {
struct fill_test_access {
	static bool scratch_clear(const postings_iterator& it) {
		for (auto w : it.word_sum_) { if (w) { return false; } }
		return true;
	}
};
}  // namespace prospect

namespace {

namespace f = prospect::format;
using strata::no_more_docs;
using strata::ordinal_t;
using ords_t = std::vector<ordinal_t>;

constexpr ordinal_t docs = 40000;

// One segment whose term t holds exactly ords[t] (docid order: ordinal = document number).
std::string build(const std::map<std::string, ords_t>& terms) {
	prospect::segment_writer w;
	std::map<std::string, std::size_t> at;
	std::mt19937_64 wdfs(9);
	for (ordinal_t d = 0; d < docs; ++d) {
		std::vector<prospect::posting_input> in;
		for (const auto& [t, o] : terms) {
			auto& i = at[t];
			if (i < o.size() && o[i] == d) {
				// "bigwdf" has wdfs of about 30 bits, so its blocks are large enough to hold a crafted one.
				in.push_back({t, t == "bigwdf" ? 1 + std::uint32_t(wdfs() >> 34) : 1, {}}), ++i;
			}
		}
		w.add(1 + d, in, 1);
	}
	return w.flush({});
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

// A window's bits with a guard of known words on both sides.
struct window {
	std::uint64_t before[8], bits[64], after[8], pre[64];
	window() { clear(); }
	// Empty, or already holding other clauses' bits (fill ORs into the window).
	void clear(std::mt19937_64* other = nullptr) {
		for (auto& w : before) { w = 0xa5a5a5a5a5a5a5a5ull; }
		for (auto& w : after) { w = 0x5a5a5a5a5a5a5a5aull; }
		for (std::size_t i = 0; i < 64; ++i) { bits[i] = pre[i] = other ? ((*other)() & (*other)()) : 0; }
	}
	// Whether the window now holds what it did plus the model's bits.
	bool holds(const std::uint64_t* model) const {
		for (std::size_t i = 0; i < 64; ++i) { if (bits[i] != (pre[i] | model[i])) { return false; } }
		return guarded();
	}
	bool guarded() const {
		for (auto w : before) { if (w != 0xa5a5a5a5a5a5a5a5ull) { return false; } }
		for (auto w : after) { if (w != 0x5a5a5a5a5a5a5a5aull) { return false; } }
		return true;
	}
};

// The model's window [base, end) as bits.
void expect(const ords_t& o, ordinal_t base, ordinal_t end, std::uint64_t* bits) {
	std::fill(bits, bits + 64, 0);
	for (auto it = std::lower_bound(o.begin(), o.end(), base); it != o.end() && *it < end; ++it) {
		bits[(*it - base) >> 6] |= std::uint64_t(1) << ((*it - base) & 63);
	}
}
ordinal_t first_from(const ords_t& o, ordinal_t t) {
	const auto it = std::lower_bound(o.begin(), o.end(), t);
	return it == o.end() ? no_more_docs : *it;
}

// Windows of `len` ordinals from `from`: the iterator is advanced to each window's start, fill
// sets the window, and the iterator must stand on the first posting at or past its end.
bool windows(const prospect::inverted_plugin::reader& inv, const f::term_info& info, const ords_t& o, ordinal_t from, ordinal_t len) {
	prospect::postings_iterator it(inv, info);
	window w;
	std::uint64_t want[64];
	std::mt19937_64 rng(from * 31 + len);
	for (ordinal_t base = from; base < docs; base += len) {
		const ordinal_t end = base + len, d = it.advance(base);
		if (d != first_from(o, base)) { return false; }
		if (d == no_more_docs) { break; }
		if (d >= end) { continue; }
		w.clear(rng() & 1 ? &rng : nullptr);
		it.fill(base, end, w.bits);
		expect(o, base, end, want);
		if (!w.holds(want) || it.doc() != first_from(o, end)) { return false; }
	}
	return true;
}

// count()'s walk: next() to a posting, the window around it aligned to 4,096, fill, repeat.
bool aligned(const prospect::inverted_plugin::reader& inv, const f::term_info& info, const ords_t& o) {
	prospect::postings_iterator it(inv, info);
	window w;
	std::uint64_t want[64], n = 0;
	for (ordinal_t d = it.next(); d != no_more_docs; d = it.doc()) {
		const ordinal_t base = d & ~ordinal_t(4095), end = base + 4096;
		w.clear();
		it.fill(base, end, w.bits);
		expect(o, base, end, want);
		if (!w.holds(want)) { return false; }
		for (auto x : w.bits) { n += std::uint64_t(std::popcount(x)); }
	}
	return n == o.size();
}

// Fills and next() calls interleaved at random: the postings after a fill are the model's.
bool mixed(const prospect::inverted_plugin::reader& inv, const f::term_info& info, const ords_t& o, std::uint64_t seed) {
	std::mt19937_64 rng(seed);
	prospect::postings_iterator it(inv, info);
	window w;
	std::uint64_t want[64];
	std::size_t at = 0;
	ordinal_t d = it.next();
	while (d != no_more_docs) {
		if (d != o[at]) { return false; }
		if (rng() % 3) {
			// A window from at or before d, after the posting before d, and past d.
			const ordinal_t floor = at ? o[at - 1] + 1 : 0, back = std::min<ordinal_t>(d - floor, ordinal_t(rng() % 70)), base = d - back;
			const ordinal_t end = d + 1 + ordinal_t(rng() % (4096 - back));
			w.clear(rng() & 1 ? &rng : nullptr);
			it.fill(base, end, w.bits);
			expect(o, base, end, want);
			if (!w.holds(want)) { return false; }
			at = std::size_t(std::lower_bound(o.begin(), o.end(), end) - o.begin());
			d = it.doc();
			if (at == o.size() ? d != no_more_docs : d != o[at]) { return false; }
		} else {
			for (std::size_t k = rng() % 300; k-- && d != no_more_docs;) {
				d = it.next();
				++at;
				if (at == o.size() ? d != no_more_docs : d != o[at]) { return false; }
			}
		}
	}
	return at == o.size();
}

// A window whose end sweeps across a block's last ordinal, starting in the block before.
bool ends(const prospect::inverted_plugin::reader& inv, const f::term_info& info, const ords_t& o) {
	window w;
	std::uint64_t want[64];
	std::mt19937_64 rng(5);
	for (std::size_t j = 2; j + 2 < o.size() / f::block && j < 12; ++j) {
		const ordinal_t last = o[j * f::block + f::block - 1], start = o[(j - 1) * f::block + 3];
		for (ordinal_t end = std::max<ordinal_t>(start + 1, last > 70 ? last - 70 : 1); end <= last + 70 && end - start <= 4096; ++end) {
			prospect::postings_iterator it(inv, info);
			if (it.advance(start) != start) { return false; }
			w.clear(&rng);
			it.fill(start, end, w.bits);
			expect(o, start, end, want);
			if (!w.holds(want) || it.doc() != first_from(o, end)) { return false; }
		}
	}
	return true;
}

void check_term(const prospect::segment& s, const std::string& term, const ords_t& o) {
	const auto& inv = s.get<prospect::inverted_plugin>();
	const auto info = *inv.term(term);
	CHECK(info.df == o.size());
	for (const ordinal_t len : {4096u, 4000u, 1024u, 300u, 64u, 1u}) {
		for (const ordinal_t from : {0u, 37u, 4095u}) { CHECK(windows(inv, info, o, from, len)); }
	}
	CHECK(aligned(inv, info, o));
	for (std::uint64_t seed = 1; seed <= 6; ++seed) { CHECK(mixed(inv, info, o, seed)); }
	CHECK(ends(inv, info, o));
}

// Posting-index blocks of one term as the file stores them.
struct kinds {
	std::size_t constant = 0, deltas = 0, bitset = 0;
};
kinds block_kinds(const std::string& file, const prospect::segment& s, const std::string& term) {
	const auto info = *s.get<prospect::inverted_plugin>().term(term);
	const auto l = skip_test::parse(file, s, info);
	kinds k;
	for (const auto& b : l.blocks) {
		if (b.codec == unsigned(f::block_codec::bitset)) {
			++k.bitset;
		} else if (std::uint8_t(file[b.at]) == bitpack::constant_flag) {
			++k.constant;
		} else {
			++k.deltas;
		}
	}
	return k;
}

// --- the terms --------------------------------------------------------------------------------

ords_t random_term(std::mt19937_64& rng, double p, ordinal_t from = 0, ordinal_t to = docs) {
	ords_t o;
	std::bernoulli_distribution b(p);
	for (ordinal_t d = from; d < to; ++d) { if (b(rng)) { o.push_back(d); } }
	return o;
}
void append(ords_t& o, ordinal_t a, ordinal_t b) {  // a..b inclusive
	for (ordinal_t d = a; d <= b; ++d) { o.push_back(d); }
}
// Runs of consecutive ordinals of random length between random gaps, so runs start and end
// anywhere in a word and straddle windows.
ords_t runs_term(std::mt19937_64& rng) {
	ords_t o;
	for (ordinal_t d = 7; d < docs - 3000;) {
		const ordinal_t len = 1 + ordinal_t(rng() % 2500);
		append(o, d, d + len - 1);
		d += len + 1 + ordinal_t(rng() % 3000);
	}
	return o;
}
// Each 64-bit word from {empty, full, bit 0, bit 63, bits 0 and 63, random}: full words
// followed by a lone bit 0 make a running sum of masks wrap to zero.
ords_t wordy_term(std::mt19937_64& rng) {
	ords_t o;
	for (ordinal_t w = 0; w < docs / 64; ++w) {
		std::uint64_t word = 0;
		switch (rng() % 8) {
			case 0: break;
			case 1: case 2: word = ~std::uint64_t(0); break;
			case 3: word = 1; break;
			case 4: word = std::uint64_t(1) << 63; break;
			case 5: word = 1 | std::uint64_t(1) << 63; break;
			default: word = rng(); break;
		}
		for (unsigned b = 0; b < 64; ++b) { if (word >> b & 1) { o.push_back(w * 64 + b); } }
	}
	return o;
}
// Per window: a block of every third ordinal, then, after a long gap that keeps it FOR, a block
// that is a full word, a lone bit 0 of the next word, and every other ordinal after: its masks
// sum to a multiple of 2^64 at that bit.
ords_t wrap_term() {
	ords_t o;
	for (ordinal_t w = 0; w < 8; ++w) {
		const ordinal_t at = w * 4096, pattern = at + 2048;
		for (ordinal_t i = 0; i < f::block; ++i) { o.push_back(at + 3 * i); }
		append(o, pattern, pattern + 64);
		for (ordinal_t j = 0; j < 63; ++j) { o.push_back(pattern + 128 + 2 * j); }
	}
	return o;
}
// A step-1 run starting and ending mid-word across a window's edge, with blocks of the run
// before it and a sparse block after; repeated at several alignments.
ords_t run_edges_term() {
	ords_t o;
	for (const ordinal_t start : {3990u, 8192u + 5u, 12288u - 1u, 16384u + 63u, 20480u - 64u, 24576u + 100u}) {
		const ordinal_t at = o.empty() ? 0 : (o.back() | 63) + 200;
		for (ordinal_t i = 0; i < 128; ++i) { o.push_back(std::max(at, start - 700) + 4 * i); }
		append(o, start, start + 128 * 9 + 36);  // nine blocks and a bit
	}
	return o;
}

// A damaged copy walked the way count() does (aligned windows) and in 700-ordinal windows from
// each posting: whether it fenced. The window's guard words and the scatter scratch must be
// untouched whatever happens.
bool walk_copy(const std::string& bad, const std::string& term) {
	try {
		prospect::segment sb(bad);
		const auto& inv = sb.get<prospect::inverted_plugin>();
		const auto info = *inv.term(term);
		for (const ordinal_t len : {4096u, 700u}) {
			prospect::postings_iterator it(inv, info);
			window w;
			for (ordinal_t d = it.next(); d != no_more_docs;) {
				const ordinal_t base = len == 4096 ? d & ~ordinal_t(4095) : d, end = base + len;
				w.clear();
				it.fill(base, end, w.bits);
				CHECK(w.guarded());
				CHECK(prospect::fill_test_access::scratch_clear(it));
				d = it.doc();
				if (d != no_more_docs && d < end) { CHECK(false); break; }
			}
		}
		return false;
	} catch (const strata::corrupt_segment&) {
		return true;
	}
}

int main_body() {
	std::mt19937_64 rng(2026);
	std::map<std::string, ords_t> terms;
	terms["mid"] = random_term(rng, 0.2);          // FOR blocks, gaps of a few
	terms["sparse"] = random_term(rng, 0.02);      // spans past the scatter's sparse bound
	terms["dense"] = random_term(rng, 0.8);        // bitset blocks
	terms["full"] = random_term(rng, 1.0, 100);    // one long run
	{  // bitset and FOR blocks
		ords_t o = random_term(rng, 0.45, 0, 8000);
		for (const auto x : random_term(rng, 0.8, 8000, 12000)) { o.push_back(x); }
		for (const auto x : random_term(rng, 0.1, 12000, 20000)) { o.push_back(x); }
		terms["mixed"] = o;
	}
	terms["runs"] = runs_term(rng);
	terms["wordy"] = wordy_term(rng);
	terms["bigwdf"] = random_term(rng, 0.2);
	terms["wrap"] = wrap_term();
	terms["run_edges"] = run_edges_term();
	{  // medium density with a dense stretch, an empty stretch, and a sparse tail
		ords_t o = random_term(rng, 0.3, 0, 9000);
		for (const auto x : random_term(rng, 0.95, 9000, 12000)) { o.push_back(x); }
		for (const auto x : random_term(rng, 0.004, 15000, 40000)) { o.push_back(x); }
		terms["stretches"] = o;
	}
	for (auto& [t, o] : terms) {
		CHECK(std::is_sorted(o.begin(), o.end()) && std::adjacent_find(o.begin(), o.end()) == o.end() && o.back() < docs);
	}
	const std::string file = build(terms);
	prospect::segment s(file);

	// The shapes the cases rely on are there (fast paths on or off: the file is the same).
	{
		const auto run = block_kinds(file, s, "run_edges"), dense = block_kinds(file, s, "dense"), mid = block_kinds(file, s, "mid"), wrap = block_kinds(file, s, "wrap");
		CHECK(run.constant >= 30 && dense.bitset >= 100 && mid.deltas >= 20 && wrap.deltas >= 8);
		const auto mixed = block_kinds(file, s, "mixed");
		CHECK(mixed.bitset > 0 && mixed.deltas > 0);
	}

	for (const auto& [t, o] : terms) { check_term(s, t, o); }

	// A fixed case for the sum: the lone bit after a full word must land in its own word.
	{
		const auto& inv = s.get<prospect::inverted_plugin>();
		const auto info = *inv.term("wrap");
		prospect::postings_iterator it(inv, info);
		window w;
		CHECK(it.next() == 0);
		it.fill(0, 4096, w.bits);
		CHECK(w.bits[32] == ~std::uint64_t(0) && w.bits[33] == 1 && w.guarded());
	}

	// --- damaged blocks: fence or answer, never write outside the window ------------------------
	std::size_t fenced = 0, answered = 0;
	for (const std::string term : {"mid", "dense", "mixed", "runs", "wordy", "wrap", "run_edges", "sparse"}) {
		const auto info0 = *s.get<prospect::inverted_plugin>().term(term);
		const auto layout = skip_test::parse(file, s, info0);
		std::mt19937_64 r(std::hash<std::string>{}(term));
		for (int trial = 0; trial < 160; ++trial) {
			std::string bad = file;
			const auto& b = layout.blocks[r() % layout.blocks.size()];
			switch (r() % 4) {
				case 0: bad[b.at + r() % std::min<std::size_t>(b.bytes, 8)] = char(r()); break;  // the header, the constant
				case 1: bad[b.at + r() % b.bytes] = char(r()); break;
				case 2: bad[b.at + r() % b.bytes] ^= char(1u << (r() % 8)); break;
				default: {  // an advance entry
					const auto& g = layout.groups[r() % layout.groups.size()];
					switch (r() % 3) {
						case 0: if (g.n > 1) { skip_test::put(bad, g.adv, g.bit_last(r() % (g.n - 1)), g.wl, r()); } break;
						case 1: if (g.n > 1) { skip_test::put(bad, g.adv, g.bit_offset(1 + r() % (g.n - 1)), g.wo, r()); } break;
						default: if (g.wf) { skip_test::put(bad, g.adv, g.bit_codec(r() % g.n), g.wf, r()); } break;
					}
				}
			}
			reseal(bad);
			++(walk_copy(bad, term) ? fenced : answered);
		}
	}
	CHECK(fenced > 100 && answered > 100);

	// --- crafted blocks that pass the last-ordinal check only by wrapping ---------------------------
	// Two checks keep the fused unpack from wrapping: widths of at most 25 bits, and an entry's last
	// above the one before it. Without either, a block can sum to the entry's last mod 2^32 with its
	// leading ordinals far outside the window. Each copy must fence (and, if a check is removed, the
	// assert in scatter or the guard words catch the write).
	{
		const auto info0 = *s.get<prospect::inverted_plugin>().term("bigwdf");
		const auto layout = skip_test::parse(file, s, info0);
		std::size_t wide = 0, flat = 0;
		for (std::size_t c = 2; c < layout.blocks.size(); ++c) {
			const auto& b = layout.blocks[c];
			const auto& g = layout.groups[c / 32];
			const std::size_t i = c % 32;
			if (b.codec != unsigned(f::block_codec::deltas) || b.bytes < 1 + 16 * 32) { continue; }
			{  // (a) width 32: gaps 2^31 and 2^31 + D - 128 sum, with 128 more, to D mod 2^32
				std::uint32_t in[f::block] = {};
				in[0] = 0x80000000u;
				in[1] = 0x80000000u + std::uint32_t(b.last - layout.blocks[c - 1].last) - f::block;
				std::string bad = file;
				bad[b.at] = char(32);
				bitpack::pack(in, 32, reinterpret_cast<std::uint8_t*>(&bad[b.at + 1]));
				reseal(bad);
				CHECK(walk_copy(bad, "bigwdf"));
				++wide;
			}
			if (i + 1 < g.n) {  // (b) entry c's last equal to c - 1's, 128 gaps of 2^25 summing to 0 mod 2^32
				std::uint32_t in[f::block];
				std::fill(in, in + f::block, (1u << 25) - 1);
				std::string bad = file;
				skip_test::put(bad, g.adv, g.bit_last(i), g.wl, layout.blocks[c - 1].last - g.base);
				bad[b.at] = char(25);
				bitpack::pack(in, 25, reinterpret_cast<std::uint8_t*>(&bad[b.at + 1]));
				reseal(bad);
				CHECK(walk_copy(bad, "bigwdf"));
				++flat;
			}
		}
		CHECK(wide >= 10 && flat >= 10);
	}
	std::printf("fill%s: %zu damaged copies fenced, %zu answered\n", prospect::fill_fast ? "" : " (fast paths off)", fenced, answered);
	return 0;
}

}  // namespace

int main() {
	main_body();
	std::printf("fill: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
