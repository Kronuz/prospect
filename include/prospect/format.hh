/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The search sections' encodings (docs/design/phase-2.md, layers.md L1): the length quantization
// and skip data, ordinal blocks, threshold seeds, and term info, which the inverted plugin
// writes and reads. The file's frame, section table, and structure bits are core/format.hh's.

#pragma once

#include "bitpack.hh"
#include "strata/core/format.hh"
#include "varint.hh"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace strata::format {

// --- document lengths in skip data ------------------------------------------------------------

// A one-byte log scale for lengths: exact below 24, then 3 mantissa bits per power of two.
// decode(encode(n)) <= n always, so bounds computed from stored lengths never understate.
inline std::uint8_t encode_length(std::uint32_t n) noexcept {
	if (n < 24) { return std::uint8_t(n); }
	unsigned e = unsigned(std::bit_width(n)) - 4;      // n >> e is in [8, 15]
	return std::uint8_t(24 + (e - 1) * 8 + ((n >> e) - 8));
}

inline std::uint32_t decode_length(std::uint8_t q) noexcept {
	if (q < 24) { return q; }
	unsigned e = (q - 24) / 8 + 1, m = 8 + (q - 24) % 8;
	return m << e;
}

// --- skip data (F10) --------------------------------------------------------------------------

// A term with skip blocks (df >= 128) is laid out as
//   [groups][skip flags u8][with positions: tail's first position, its block offset]
//   [pairs bytes][records bytes][blocks bytes]  (LEB128)
//   [level 1: per group last u32, pairs offset u32, record, blocks, and with positions first
//    position, position and count block offsets, each u32 or u64 under skip::wide]
//   [pairs: each group's frontier][records][blocks][tail]
// A group of up to 32 blocks has one record: its advance entries (A3), every block's pairs
// in encode_sized_impacts' form, then with positions each later block's first position and
// position and count block offsets as deltas from the block before it. The advance entries
// bit-pack the lasts of all but the last block (minus the group's base, the previous
// group's last or 0) at bit_width(last - base), the byte offsets of all but the first block
// at bit_width(group bytes - 1), then each block's codec code at the term's codec width:
// every width is derived from level 1 or the flags, so locating a block reads no header.
namespace skip {
inline constexpr std::uint8_t wide = 1;          // 64-bit level-1 offsets
inline constexpr std::uint8_t codec_shift = 1;   // bits 1-2: codec code width, 0 to 2
inline constexpr std::uint8_t partitioned = 8;   // reserved for F12's partitions (P1); refused
inline constexpr std::uint8_t known = wide | 6;  // every other bit is damage
inline constexpr std::size_t group_blocks = 32;
}  // namespace skip

// A block's ordinal codec (F10, F11).
// Code 2 is reserved for Elias-Fano blocks (M22, F13-R45's place): not written by the default
// writer, refused by readers until a later pre-freeze version or a structure bit enables it.
enum class block_codec : std::uint8_t { deltas = 0, bitset = 1 };
inline constexpr std::uint8_t max_block_codec = 1;

// The advance entries' size in bytes for a group of n blocks at the given widths.
inline constexpr std::uint64_t advance_bytes(std::uint64_t n, unsigned wl, unsigned wo, unsigned wf) noexcept {
	return ((n - 1) * (wl + wo) + n * wf + 7) / 8;
}

// Writes v's low w bits at bit `at` of out (bits little-endian within bytes).
inline void put_bits(std::string& out, std::uint64_t at, unsigned w, std::uint64_t v) {
	for (unsigned i = 0; i < w; ++i, ++at) {
		if (v >> i & 1) { out[at >> 3] = char(std::uint8_t(out[at >> 3]) | (1u << (at & 7))); }
	}
}

// Reads w <= 32 bits at bit `at` of the size bytes at p; the caller keeps at + w within them.
inline std::uint32_t get_bits(const char* p, std::uint64_t size, std::uint64_t at, unsigned w) noexcept {
	const std::uint64_t byte = at >> 3;
	std::uint64_t v = 0;
	if (size - byte >= 8) {
		std::memcpy(&v, p + byte, 8);
	} else {
		std::memcpy(&v, p + byte, std::size_t(size - byte));
	}
	return std::uint32_t((v >> (at & 7)) & ((std::uint64_t(1) << w) - 1));
}

// --- ordinal blocks (F11: M23) ------------------------------------------------------------------

// A FOR ordinal block stores each gap minus one (M23), so its ordinals increase by
// construction: bitpack's FOR or constant block over g[i] = o[i] - o[i - 1] - 1, o[-1] the
// previous block's last (-1 before a term's first block). Returns the encoded bytes.
inline std::size_t encode_gaps(const std::uint32_t* ords, std::uint32_t prev, bool first, std::uint8_t* out) noexcept {
	std::uint32_t g[block];
	for (std::size_t i = 0; i < block; ++i) {
		g[i] = ords[i] - (i ? ords[i - 1] : (first ? std::uint32_t(-1) : prev)) - 1;
	}
	return bitpack::encode_for(g, out);
}

// Decodes a block written by encode_gaps whose ordinals lie in (base, last] (from 0 when
// first) and end at last; returns the bytes read, or 0 when the block is malformed, carries
// exceptions, or is inconsistent with last. A header width of at most 25 bits cannot carry
// 128 gaps past 2^32, so its last ordinal checks the block; a wider one checks the 64-bit
// sum of its gaps (F10).
inline std::size_t decode_gaps(const std::uint8_t* in, std::size_t size, std::uint32_t base, std::uint32_t last, bool first, std::uint32_t* out) noexcept {
	if (size < 1 || (in[0] & bitpack::exceptions_flag)) { return 0; }
	const std::uint64_t span = std::uint64_t(last) - (first ? std::uint64_t(-1) : std::uint64_t(base));  // sum of the gaps
	if (first) { base = std::uint32_t(-1); }
	if (!first && last <= base) { return 0; }
	if (in[0] & bitpack::constant_flag) {
		if (in[0] != bitpack::constant_flag || size < 5) { return 0; }
		std::uint32_t v;
		std::memcpy(&v, in + 1, 4);
		if ((std::uint64_t(v) + 1) * block != span) { return 0; }
		for (std::size_t i = 0; i < block; ++i) { out[i] = base + std::uint32_t((i + 1) * (std::uint64_t(v) + 1)); }
		return 5;
	}
	const unsigned bits = in[0] & 0x3f;
	if (bits > 32 || size < 1 + bitpack::packed_bytes(bits)) { return 0; }
	if (bits <= 25) {
		bitpack::unpack_delta(in + 1, bits, base, out);
		for (std::size_t i = 0; i < block; ++i) { out[i] += std::uint32_t(i + 1); }
		if (out[block - 1] != last) { return 0; }
	} else {
		bitpack::unpack(in + 1, bits, out);
		std::uint64_t sum = 0;
		for (std::size_t i = 0; i < block; ++i) { sum += out[i]; }
		if (sum + block != span) { return 0; }
		for (std::size_t i = 0; i < block; ++i) { out[i] = base += out[i] + 1; }
	}
	return 1 + bitpack::packed_bytes(bits);
}

// --- threshold seeds (F13: M20) -------------------------------------------------------------

// A term with skip blocks and wdfs stores, after its skip flags (and tail positions), for each
// k of seed_ks up to its df, two dominance pairs: at least k of its postings have wdf >= w and
// length <= l. The first pair stores l as an encode_length code, the second exactly:
// [w LEB128][code u8][w LEB128][l LEB128]. A score non-decreasing in wdf and non-increasing in
// length gives each of those k postings at least f(w, l), under any statistics.
inline constexpr std::uint32_t seed_ks[3] = {10, 100, 1000};
struct seed_pairs {
	std::uint32_t code_wdf = 0;
	std::uint8_t code = 0;
	std::uint32_t exact_wdf = 0, exact = 0;
};

// The longest length whose encode_length is at most code.
inline std::uint32_t max_length_of(std::uint8_t code) noexcept {
	if (code == 255) { return 0xffffffffu; }
	std::uint64_t lo = 0, hi = 0xffffffffull;  // the last n with encode_length(n) <= code
	while (lo < hi) {
		const std::uint64_t mid = lo + (hi - lo + 1) / 2;
		if (encode_length(std::uint32_t(mid)) <= code) { lo = mid; } else { hi = mid - 1; }
	}
	return std::uint32_t(lo);
}

// --- term info (the termdict value) -----------------------------------------------------------

enum term_flags : std::uint8_t {
	no_wdf = 1,           // every posting has wdf 0 (boolean term)
	no_positions = 2,     // the term has no position stream
	single_posting = 4,   // df == 1: ordinal and wdf inline, no postings bytes
	counts_are_wdf = 8,   // every document's position count equals its wdf
	cf_is_df = 16,        // cf equals df: not stored
	absolute = 32,        // offsets stored; otherwise they follow the previous term's
};

// A competitive (wdf, length) pair: no posting in range has more wdf at less length.
struct impact {
	std::uint32_t wdf = 0;
	std::uint8_t length = 0;  // encode_length() of the shortest length at this wdf
};

struct term_info {
	std::uint8_t flags = 0;
	std::uint32_t df = 0;
	std::uint64_t cf = 0;  // collection frequency: sum of wdf
	// Whole-list competitive pairs; stored for terms with skip blocks (df >= 128), computed
	// from the postings for the others (segment::term).
	std::vector<impact> impacts;
	std::uint64_t postings = 0;   // offset in `postings`, or the ordinal when single_posting
	std::uint64_t positions = 0;  // offset in `positions`
	std::uint32_t single_wdf = 0; // wdf when single_posting
	// Byte lengths of the term's postings and positions, so a reader verifies their
	// regions before reading them.
	std::uint64_t postings_length = 0, positions_length = 0;
};

inline void encode_impacts(std::string& out, const std::vector<impact>& impacts) {
	put_leb(out, impacts.size());
	for (const auto& i : impacts) {
		put_leb(out, i.wdf);
		out.push_back(char(i.length));
	}
}

// Appends one encoded list of competitive pairs to out; returns how many.
inline std::size_t decode_impacts(const char** p, const char* end, std::vector<impact>& out) {
	const std::uint64_t n = get_leb(p, end);
	if (n == 0 || n > 256) { corrupt("impact count"); }  // lengths are 1 byte
	for (std::size_t k = 0; k < n; ++k) {
		impact i;
		i.wdf = std::uint32_t(get_leb(p, end));
		if (*p == end) { corrupt("impacts truncated"); }
		i.length = std::uint8_t(*(*p)++);
		out.push_back(i);
	}
	return n;
}

// Skip data prefixes each list with its byte length, so readers step over it and decode
// it only when a bound needs it.
// A block's competitive pairs in skip data: a LEB128 header (count << 1 | compact). A
// compact list (every wdf below 256) is count pairs of (wdf byte, length byte), so the
// header alone gives its size; otherwise its size (LEB128) and encode_impacts' bytes follow.
inline void encode_sized_impacts(std::string& out, const std::vector<impact>& impacts) {
	const bool compact = std::all_of(impacts.begin(), impacts.end(), [](const impact& i) { return i.wdf < 256; });
	put_leb(out, std::uint64_t(impacts.size()) << 1 | std::uint64_t(compact));
	if (compact) {
		for (const auto& i : impacts) {
			out.push_back(char(i.wdf));
			out.push_back(char(i.length));
		}
		return;
	}
	std::string list;
	encode_impacts(list, impacts);
	put_leb(out, list.size());
	out += list;
}

// Steps over a list written by encode_sized_impacts; returns all its bytes.
inline std::string_view skip_sized_impacts(const char** p, const char* end) {
	const char* start = *p;
	const std::uint64_t h = get_leb(p, end), n = h >> 1;
	if (n == 0 || n > 256) { corrupt("impact count"); }
	const std::uint64_t size = (h & 1) ? 2 * n : get_leb(p, end);
	if (size > std::uint64_t(end - *p)) { corrupt("impacts bounds"); }
	*p += size;
	return std::string_view(start, std::size_t(*p - start));
}

// Calls f(impact) for each pair of a list returned by skip_sized_impacts until f returns
// true; returns whether it did.
template <class F>
inline bool any_sized_impact(std::string_view list, F&& f) {
	const char* p = list.data();
	const char* end = p + list.size();
	const std::uint64_t h = get_leb(&p, end), n = h >> 1;
	if (!(h & 1)) {
		const std::uint64_t size = get_leb(&p, end);
		if (size != std::uint64_t(end - p)) { corrupt("impacts length"); }
		const std::uint64_t m = get_leb(&p, end);
		if (m != n) { corrupt("impact count"); }
	}
	for (std::uint64_t k = 0; k < n; ++k) {
		impact i;
		if (h & 1) {
			if (end - p < 2) { corrupt("impacts truncated"); }
			i.wdf = std::uint8_t(*p++);
		} else {
			i.wdf = std::uint32_t(get_leb(&p, end));
			if (p == end) { corrupt("impacts truncated"); }
		}
		i.length = std::uint8_t(*p++);
		if (f(i)) { return true; }
	}
	if (p != end) { corrupt("impacts length"); }
	return false;
}

inline std::vector<impact> decode_impacts(const char** p, const char* end) {
	std::vector<impact> out;
	decode_impacts(p, end, out);
	return out;
}

// Term infos in one dictionary restart run share a cursor: a term's postings and
// positions start where the previous term's end. A run's first entry, or one after a gap,
// stores both cursors (flag `absolute`); the rest only their lengths. Layout, integers
// LEB128: [df << 5 | no_wdf, no_positions, counts_are_wdf, cf_is_df, absolute as bits
// 0-4][if absolute: postings cursor, positions cursor], then for a single posting (df 1)
// [ordinal][wdf unless 1, which cf_is_df marks], else [cf unless cf_is_df][postings
// length]; with positions [positions length]; with skip blocks (df >= 128) the impacts.
struct term_run {
	std::uint64_t postings = 0, positions = 0;  // where the next term's bytes start
	bool started = false;
};

namespace detail {
inline constexpr std::uint8_t packed_flags[5] = {no_wdf, no_positions, counts_are_wdf, cf_is_df, absolute};
}

inline std::string encode_term_info(term_info t, term_run& run, bool restart) {
	const bool single = t.flags & single_posting, positionless = t.flags & no_positions;
	const bool contiguous = run.started && (single || t.postings == run.postings) && (positionless || t.positions == run.positions);
	t.flags = std::uint8_t(t.flags & ~(cf_is_df | absolute));
	if (single ? t.single_wdf == 1 : t.cf == t.df) { t.flags |= cf_is_df; }
	if (restart || !contiguous) {
		t.flags |= absolute;
		if (!single) { run.postings = t.postings; }
		if (!positionless) { run.positions = t.positions; }
	}
	std::uint64_t head = std::uint64_t(t.df) << 5;
	for (unsigned b = 0; b < 5; ++b) {
		if (t.flags & detail::packed_flags[b]) { head |= 1u << b; }
	}
	std::string out;
	put_leb(out, head);
	if (t.flags & absolute) {
		put_leb(out, run.postings);
		put_leb(out, run.positions);
	}
	if (single) {
		put_leb(out, t.postings);
		if (!(t.flags & cf_is_df)) { put_leb(out, t.single_wdf); }
	} else {
		if (!(t.flags & cf_is_df)) { put_leb(out, t.cf); }
		put_leb(out, t.postings_length);
		run.postings += t.postings_length;
	}
	if (!positionless) {
		put_leb(out, t.positions_length);
		run.positions += t.positions_length;
	}
	if (t.df >= block) { encode_impacts(out, t.impacts); }
	run.started = true;
	return out;
}

// Decodes one entry of a run, resolving its offsets from the run's cursor; without
// impacts, they are skipped unread (an entry decoded only to move the cursor).
inline term_info decode_term_info(std::string_view bytes, term_run& run, bool with_impacts = true) {
	const char* p = bytes.data();
	const char* end = p + bytes.size();
	term_info t;
	const std::uint64_t head = get_leb(&p, end);
	if (head >> 5 == 0 || head >> 5 > 0xffffffffu) { corrupt("term info"); }
	t.df = std::uint32_t(head >> 5);
	for (unsigned b = 0; b < 5; ++b) {
		if (head >> b & 1) { t.flags |= detail::packed_flags[b]; }
	}
	if (t.df == 1) { t.flags |= single_posting; }
	if (!(t.flags & absolute) && !run.started) { corrupt("term offsets without a run"); }
	if (t.flags & absolute) {
		run.postings = get_leb(&p, end);
		run.positions = get_leb(&p, end);
	}
	if (t.flags & single_posting) {
		t.postings = get_leb(&p, end);
		t.single_wdf = t.flags & cf_is_df ? 1 : std::uint32_t(get_leb(&p, end));
		t.cf = t.single_wdf;
	} else {
		t.cf = t.flags & cf_is_df ? t.df : get_leb(&p, end);
		t.postings_length = get_leb(&p, end);
		t.postings = run.postings;
		if (run.postings + t.postings_length < run.postings) { corrupt("postings range"); }
		run.postings += t.postings_length;
	}
	if (!(t.flags & no_positions)) {
		t.positions_length = get_leb(&p, end);
		t.positions = run.positions;
		if (run.positions + t.positions_length < run.positions) { corrupt("positions range"); }
		run.positions += t.positions_length;
	}
	if (t.df >= block) {
		if (with_impacts) {
			t.impacts = decode_impacts(&p, end);
		} else {
			p = end;
		}
	}
	if (p != end) { corrupt("term info"); }
	run.started = true;
	return t;
}

// The competitive pairs of a list of (wdf, length) pairs: its Pareto frontier (higher
// wdf, lower length), highest wdf first. At most 256 pairs, one per length code.
inline std::vector<impact> competitive(const std::vector<impact>& pairs) {
	// Lengths are one byte: the highest wdf at each length, then one pass from the shortest
	// keeps each length whose wdf beats every shorter one's. No sort.
	std::array<std::uint64_t, 256> best{};  // wdf + 1 at each length; 0 for none
	for (const auto& p : pairs) { best[p.length] = std::max<std::uint64_t>(best[p.length], std::uint64_t(p.wdf) + 1); }
	std::vector<impact> frontier;
	std::uint64_t beat = 0;
	for (unsigned length = 0; length < 256; ++length) {
		if (best[length] > beat) {
			beat = best[length];
			frontier.push_back({std::uint32_t(beat - 1), std::uint8_t(length)});
		}
	}
	std::reverse(frontier.begin(), frontier.end());  // highest wdf first
	if (frontier.empty()) { frontier.push_back({}); }
	return frontier;
}

}  // namespace strata::format
