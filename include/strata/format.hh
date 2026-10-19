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

// The segment file format (docs/design/phase-2.md): header, sections, table of
// contents, footer, term info, and the length quantization used by skip data. Every
// fixed-width field is little-endian; variable-length integers are LEB128.

#pragma once

#include "bitpack.hh"
#include "crc32c.hh"
#include "termdict.hh"
#include "varint.hh"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata {

using ordinal_t = std::uint32_t;

// Raised when segment bytes break a format rule; the segment is fenced.
struct corrupt_segment : std::runtime_error {
	explicit corrupt_segment(const char* what) : std::runtime_error(std::string("strata: corrupt segment: ") + what) {}
};

// Raised for a segment this binary cannot read: another format version, or a structure it
// lacks (format-13.md F2, F4). Not damage, so it never fences a segment by itself.
struct unsupported_segment : std::runtime_error {
	explicit unsupported_segment(const std::string& what) : std::runtime_error("strata: unsupported segment: " + what) {}
};

// Throws corrupt_segment, out of line and cold, so the decoders that check keep no throw
// path inline.
[[noreturn, gnu::cold, gnu::noinline]] inline void corrupt(const char* what) { throw corrupt_segment(what); }

namespace format {

inline constexpr char header_magic[8] = {'S', 'T', 'R', 'S', 'E', 'G', '0', '1'};
inline constexpr char footer_magic[4] = {'S', 'E', 'N', 'D'};
// The version written, and the oldest one read (N - 1 from format 13 on: F2). Until format
// 13 freezes, both are a pre-freeze number, bumped with each byte change and 13 at the
// freeze, so an index from another pre-freeze build is refused, never misread (CR11).
inline constexpr std::uint32_t pre_freeze = 0x8000'0007;
inline constexpr std::uint32_t version = pre_freeze;
inline constexpr std::uint32_t min_read_version = pre_freeze;
// Bytes 0-15, frozen in every version: magic, version, CRC-32C of bytes 0-11 (F2).
inline constexpr std::size_t prefix_size = 16;
inline constexpr std::size_t header_size = 36;
inline constexpr std::size_t identity_offset = 20;  // the header's segment identity, 16 bytes (F6)
inline constexpr std::size_t footer_size = 56;
// Each section is verified per region of 2^region_shift bytes, recorded in its table of
// contents entry (4 KiB by default: docs/perf/p15-region-size.md).
inline constexpr std::uint8_t default_region_shift = 12;
inline constexpr std::uint8_t min_region_shift = 12, max_region_shift = 20;
// Stored records are packed into zstd frames of at most this many uncompressed bytes (a
// larger record takes a frame of its own).
inline constexpr std::size_t stored_block_size = 64 * 1024;
// The largest stored record a document may carry; a frame then holds at most one block
// of records or one record this size, plus its length header, which bounds what reading
// a frame allocates even when a file's checksums were recomputed (review C9).
inline constexpr std::uint64_t max_stored_record = 64ull << 20;
// The longest term: the term dictionary's key limit (fst's 32-bit depth), past Lucene's
// 32,766 and Tantivy's 65,530 bytes.
inline constexpr std::size_t max_term_bytes = termdict::max_key_bytes;
inline constexpr std::uint64_t max_stored_frame = stored_block_size + max_stored_record + 64;
// Sections start at multiples of 8: every fixed-width read is a memcpy (F5).
inline constexpr std::size_t section_alignment = 8;
inline constexpr std::size_t block = 128;

// Section kinds (F3). Ids are stable; a later format adds kinds and never renumbers.
enum class section : std::uint16_t {
	header = 0,
	terms = 1,
	term_index = 2,
	postings = 3,
	positions = 4,
	lengths = 5,
	stored = 6,
	docids = 7,
	stats = 8,
	norms = 9,  // one encode_length byte per document, for Lucene-compatible scoring
	versions = 10,   // one version per ordinal, bit-packed from the segment's minimum
	pk_filter = 11,  // retired: format 12's key filter; corruption wherever it appears
	keys = 12,       // key blocks: key -> ordinal (keys.hh, F8)
	key_index = 13,  // count, range, filter, and the key blocks' index (keys.hh, F8)
	values = 14,
	points = 15,
	term_lists = 16,
	index_sort = 17,
	pair_summaries = 18,
	stored_dictionary = 19,
	stored_holes = 20,
	quantized_impacts = 21,
	key_hash = 22,
};
inline constexpr std::uint16_t section_kinds = 23;

// Structure bits of the footer's structures word (F4): bits 0 to 31 in its first u32, 32 to
// 63 in the second (F13-R40), classified alike; no format-13 binary reads one past bit 15.
using structure_bits = std::uint64_t;
namespace bit {
inline constexpr structure_bits inverted = 1, norms = 2, keys = 4, stored = 8, values = 16, points = 32, term_lists = 64, index_sort = 128,
	pair_summaries = 256, stored_dictionary = 512, stored_holes = 1024, quantized_impacts = 2048, key_hash = 4096, stored_spans = 8192,
	postings_runs = 16384, compact = 32768, version_clocks = 65536;
}  // namespace bit

// The bit a structure's bit requires (F4): a sub-structure its owner's, index_sort values;
// 0 for none.
inline constexpr structure_bits requires_of(structure_bits b) noexcept {
	switch (b) {
	case bit::norms: case bit::pair_summaries: case bit::quantized_impacts: case bit::postings_runs: return bit::inverted;
	case bit::stored_dictionary: case bit::stored_holes: case bit::stored_spans: return bit::stored;
	case bit::key_hash: case bit::version_clocks: return bit::keys;
	case bit::index_sort: return bit::values;
	default: return 0;
	}
}

// Who owns a section kind (F3): the core or one plugin.
enum class owner : std::uint8_t { core, inverted, keys, stored, values, points, term_lists };
// How a section is verified (L7): whole at open, per region on first read, or whole on
// its first use.
enum class check : std::uint8_t { pinned, regions, first_use };
inline constexpr std::uint8_t unranked = 0xff;  // a reserved kind: its release fixes the rank

// One row of F3's table per kind: the only copy of a kind's owner, presence, class, and
// rank in file order.
struct kind_rule {
	section kind;
	const char* name;
	owner owned_by;
	structure_bits needs;  // the structure bit that requires it; 0: mandatory
	check verify;
	std::uint8_t rank;
};
inline constexpr kind_rule kinds[section_kinds] = {
	{section::header, "header", owner::core, 0, check::pinned, 0},
	{section::terms, "terms", owner::inverted, bit::inverted, check::regions, 3},
	{section::term_index, "term_index", owner::inverted, bit::inverted, check::pinned, 13},
	{section::postings, "postings", owner::inverted, bit::inverted, check::regions, 1},
	{section::positions, "positions", owner::inverted, bit::inverted, check::regions, 2},
	{section::lengths, "lengths", owner::inverted, bit::inverted, check::first_use, 4},
	{section::stored, "stored", owner::stored, bit::stored, check::regions, 6},
	{section::docids, "docids", owner::core, 0, check::pinned, 9},
	{section::stats, "stats", owner::core, 0, check::pinned, 10},
	{section::norms, "norms", owner::inverted, bit::norms, check::pinned, 12},
	{section::versions, "versions", owner::keys, bit::keys, check::pinned, 14},
	{section::pk_filter, "pk_filter", owner::core, 0, check::pinned, unranked},  // retired
	{section::keys, "keys", owner::keys, bit::keys, check::regions, 5},
	{section::key_index, "key_index", owner::keys, bit::keys, check::pinned, 15},
	{section::values, "values", owner::values, bit::values, check::regions, unranked},
	{section::points, "points", owner::points, bit::points, check::regions, unranked},
	{section::term_lists, "term_lists", owner::term_lists, bit::term_lists, check::regions, unranked},
	{section::index_sort, "index_sort", owner::core, bit::index_sort, check::pinned, unranked},
	{section::pair_summaries, "pair_summaries", owner::inverted, bit::pair_summaries, check::regions, unranked},
	{section::stored_dictionary, "stored_dictionary", owner::stored, bit::stored_dictionary, check::regions, unranked},
	{section::stored_holes, "stored_holes", owner::stored, bit::stored_holes, check::regions, unranked},
	{section::quantized_impacts, "quantized_impacts", owner::inverted, bit::quantized_impacts, check::regions, unranked},
	{section::key_hash, "key_hash", owner::keys, bit::key_hash, check::regions, unranked},
};
static_assert([] {
	for (std::uint16_t k = 0; k < section_kinds; ++k) {
		if (std::uint16_t(kinds[k].kind) != k) { return false; }
	}
	return true;
}(), "F3's table is indexed by kind");

// The kind's row, or null outside the table.
inline constexpr const kind_rule* rule_of(section kind) noexcept {
	return std::uint16_t(kind) < section_kinds ? &kinds[std::uint16_t(kind)] : nullptr;
}
inline constexpr const char* name_of(section kind) noexcept {
	const kind_rule* r = rule_of(kind);
	return r ? r->name : "unknown";
}

// --- little-endian fixed-width fields ---------------------------------------------------------

template <typename T>
inline void put(std::string& out, T v) {
	static_assert(std::endian::native == std::endian::little);
	char b[sizeof(T)];
	std::memcpy(b, &v, sizeof(T));
	out.append(b, sizeof(T));
}

template <typename T>
inline T get(const char* p) noexcept {
	T v;
	std::memcpy(&v, p, sizeof(T));
	return v;
}

inline void put_leb(std::string& out, std::uint64_t v) {
	char b[varint::leb128::max_bytes];
	out.append(b, varint::leb128::encode(b, v));
}

// Reads a LEB128 value from [*p, end) or throws corrupt_segment.
inline std::uint64_t get_leb(const char** p, const char* end) {
	std::uint64_t v = 0;
	if (!varint::leb128::decode(p, end, v)) { corrupt("truncated integer"); }
	return v;
}

// --- header, table of contents, footer --------------------------------------------------------

// The segment's version by the frozen prefix (F2): a good CRC gives the version, which
// must be one this binary reads; a format-12 file (version below 13, bytes 12-15 zero) is
// unsupported; anything else is damage.
inline std::uint32_t read_version(std::string_view file) {
	if (file.size() < prefix_size || std::memcmp(file.data(), header_magic, 8) != 0) { corrupt("header magic"); }
	const std::uint32_t v = get<std::uint32_t>(file.data() + 8);
	if (hashes::crc32c(file.data(), 12) == get<std::uint32_t>(file.data() + 12)) {
		if (v < min_read_version || v > version) { throw unsupported_segment("format version " + std::to_string(v)); }
		return v;
	}
	if (v < 13 && get<std::uint32_t>(file.data() + 12) == 0) { throw unsupported_segment("format version " + std::to_string(v)); }
	corrupt("header prefix checksum");
}

struct section_entry {
	section kind;
	std::uint64_t offset = 0;
	std::uint64_t length = 0;
	std::uint32_t crc = 0;
	std::vector<std::uint32_t> region_crcs;  // one per region of the section
	std::uint8_t region_shift = default_region_shift;
	std::uint64_t region_size() const noexcept { return std::uint64_t(1) << region_shift; }
};

// [magic][version u32][CRC-32C of bytes 0-11][flags u32 = 0][segment id] (F6).
inline std::string encode_header(const std::array<std::uint8_t, 16>& segment_id) {
	std::string h(header_magic, 8);
	put<std::uint32_t>(h, version);
	put<std::uint32_t>(h, hashes::crc32c(h.data(), h.size()));
	put<std::uint32_t>(h, 0);
	h.append(reinterpret_cast<const char*>(segment_id.data()), 16);
	return h;
}

// The section's CRC and its region CRCs over bytes.
inline section_entry describe(section kind, std::uint64_t offset, std::string_view bytes, std::uint8_t region_shift = default_region_shift) {
	section_entry e{kind, offset, bytes.size(), hashes::crc32c(bytes.data(), bytes.size()), {}, region_shift};
	const std::size_t size = e.region_size();
	for (std::size_t at = 0; at < bytes.size(); at += size) {
		e.region_crcs.push_back(hashes::crc32c(bytes.data() + at, std::min(size, bytes.size() - at)));
	}
	return e;
}

inline std::string encode_toc(const std::vector<section_entry>& entries) {
	std::string t;
	put<std::uint32_t>(t, std::uint32_t(entries.size()));
	for (const auto& e : entries) {
		put<std::uint16_t>(t, std::uint16_t(e.kind));
		put<std::uint16_t>(t, e.region_shift);
		put<std::uint64_t>(t, e.offset);
		put<std::uint64_t>(t, e.length);
		put<std::uint32_t>(t, e.crc);
		for (auto c : e.region_crcs) { put<std::uint32_t>(t, c); }
	}
	return t;
}

struct footer {
	std::uint64_t toc_offset = 0;
	std::uint64_t toc_length = 0;
	std::uint32_t toc_crc = 0;
	std::uint32_t doc_count = 0;
	std::uint64_t min_docid = 0;
	std::uint64_t max_docid = 0;
	structure_bits structures = 0;  // F4's bits, 0 to 63
};

inline constexpr std::uint64_t align_up(std::uint64_t n) noexcept { return (n + section_alignment - 1) / section_alignment * section_alignment; }

inline std::string encode_footer(const footer& f) {
	std::string out;
	put<std::uint64_t>(out, f.toc_offset);
	put<std::uint64_t>(out, f.toc_length);
	put<std::uint32_t>(out, f.toc_crc);
	put<std::uint32_t>(out, f.doc_count);
	put<std::uint64_t>(out, f.min_docid);
	put<std::uint64_t>(out, f.max_docid);
	put<std::uint64_t>(out, f.structures);  // bits 0 to 31, then 32 to 63 (F13-R40)
	put<std::uint32_t>(out, hashes::crc32c(out.data(), out.size()));
	out.append(footer_magic, 4);
	return out;
}

// The footer of a version-13 file (read_version first); the table must end at it, at the
// aligned end of the last section (F5).
inline footer decode_footer(std::string_view file) {
	if (file.size() < header_size + footer_size) { corrupt("file too small"); }
	const char* p = file.data() + file.size() - footer_size;
	if (std::memcmp(p + 52, footer_magic, 4) != 0) { corrupt("footer magic"); }
	if (hashes::crc32c(p, 48) != get<std::uint32_t>(p + 48)) { corrupt("footer checksum"); }
	footer f{get<std::uint64_t>(p), get<std::uint64_t>(p + 8), get<std::uint32_t>(p + 16), get<std::uint32_t>(p + 20),
		get<std::uint64_t>(p + 24), get<std::uint64_t>(p + 32), get<std::uint64_t>(p + 40)};
	const std::uint64_t body = file.size() - footer_size;
	if (f.toc_offset > body || f.toc_length > body - f.toc_offset || f.toc_offset + f.toc_length != body) { corrupt("table of contents bounds"); }
	return f;
}

// Parses and checks the table of contents: its CRC, entry bounds, every kind in F3's
// table, and the canonical layout (F5): the header at 0, then each entry in rank order at
// the previous one's aligned end, and the table at the last one's aligned end.
inline std::vector<section_entry> decode_toc(std::string_view file, const footer& f) {
	std::string_view t = file.substr(f.toc_offset, f.toc_length);
	if (hashes::crc32c(t.data(), t.size()) != f.toc_crc) { corrupt("table of contents checksum"); }
	const char* p = t.data();
	const char* end = p + t.size();
	auto need = [&](std::size_t n) {
		if (std::size_t(end - p) < n) { corrupt("table of contents truncated"); }
	};
	need(4);
	std::uint32_t n = get<std::uint32_t>(p);
	p += 4;
	if (n > 64) { corrupt("section count"); }
	std::vector<section_entry> entries;
	std::uint64_t at = 0;
	for (std::uint32_t i = 0; i < n; ++i) {
		need(24);
		section_entry e;
		e.kind = section(get<std::uint16_t>(p));
		if (!rule_of(e.kind)) { corrupt("unknown section kind"); }
		const std::uint16_t shift = get<std::uint16_t>(p + 2);
		if (shift < min_region_shift || shift > max_region_shift) { corrupt("region size"); }
		e.region_shift = std::uint8_t(shift);
		e.offset = get<std::uint64_t>(p + 4);
		e.length = get<std::uint64_t>(p + 12);
		e.crc = get<std::uint32_t>(p + 20);
		p += 24;
		if (e.offset > f.toc_offset || e.length > f.toc_offset - e.offset) { corrupt("section bounds"); }
		if (i == 0 ? (e.kind != section::header || e.offset != 0 || e.length != header_size) : e.offset != align_up(at)) { corrupt("section placement"); }
		if (i > 0 && rule_of(entries.back().kind)->rank >= rule_of(e.kind)->rank) { corrupt("section order"); }
		at = e.offset + e.length;
		const std::size_t regions = std::size_t((e.length + e.region_size() - 1) >> e.region_shift);
		need(regions * 4);
		e.region_crcs.reserve(regions);
		for (std::size_t r = 0; r < regions; ++r) { e.region_crcs.push_back(get<std::uint32_t>(p + 4 * r)); }
		p += regions * 4;
		entries.push_back(std::move(e));
	}
	if (p != end) { corrupt("table of contents trailing bytes"); }
	if (entries.empty()) { corrupt("section placement"); }
	if (f.toc_offset != align_up(at)) { corrupt("table of contents placement"); }
	return entries;
}

// Classifies the footer's structures word against the bits this binary reads (F4), before
// the table is decoded, since a later release's structure may add kinds or change the
// table (F23 rules 3 and 5): a bit without the bit it requires is corruption; a set bit
// this binary lacks is unsupported.
inline void classify_structures(structure_bits structures, structure_bits readable) {
	for (structure_bits b = 1; b; b <<= 1) {
		if ((structures & b) && requires_of(b) && !(structures & requires_of(b))) { corrupt("structure without its owner"); }
	}
	if (const structure_bits lacked = structures & ~readable) {
		throw unsupported_segment("structure bit " + std::to_string(std::countr_zero(lacked)));
	}
}

// In a segment whose bits this binary reads: each kind present exactly when its bit is
// set, mandatory kinds always (F4).
inline void check_presence(const std::vector<section_entry>& entries, structure_bits structures) {
	bool seen[section_kinds] = {};
	for (const auto& e : entries) { seen[std::uint16_t(e.kind)] = true; }
	for (const auto& r : kinds) {
		const bool wanted = r.needs == 0 || (structures & r.needs);
		if (seen[std::uint16_t(r.kind)] && (!wanted || r.rank == unranked)) { corrupt("section without its structure"); }
		if (wanted && r.rank != unranked && !seen[std::uint16_t(r.kind)]) { corrupt("missing section"); }
	}
}

inline void check_structures(const std::vector<section_entry>& entries, structure_bits structures, structure_bits readable) {
	classify_structures(structures, readable);
	check_presence(entries, structures);
}

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

}  // namespace format
}  // namespace strata
