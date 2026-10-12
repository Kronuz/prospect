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

#include "crc32c.hh"
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
#include <vector>

namespace strata {

using ordinal_t = std::uint32_t;

// Raised when segment bytes break a format rule; the segment is fenced.
struct corrupt_segment : std::runtime_error {
	explicit corrupt_segment(const char* what) : std::runtime_error(std::string("strata: corrupt segment: ") + what) {}
};

namespace format {

inline constexpr char header_magic[8] = {'S', 'T', 'R', 'S', 'E', 'G', '0', '1'};
inline constexpr char footer_magic[4] = {'S', 'E', 'N', 'D'};
inline constexpr std::uint32_t version = 3;
inline constexpr std::size_t header_size = 32;
inline constexpr std::size_t footer_size = 48;
inline constexpr std::size_t region_size = 64 * 1024;
inline constexpr std::size_t section_alignment = 64;
inline constexpr std::size_t block = 128;

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
};

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
	if (!varint::leb128::decode(p, end, v)) { throw corrupt_segment("truncated integer"); }
	return v;
}

// --- header, table of contents, footer --------------------------------------------------------

struct section_entry {
	section kind;
	std::uint64_t offset = 0;
	std::uint64_t length = 0;
	std::uint32_t crc = 0;
	std::vector<std::uint32_t> region_crcs;  // one per 64 KiB of the section
};

inline std::string encode_header(const std::array<std::uint8_t, 16>& segment_id) {
	std::string h(header_magic, 8);
	put<std::uint32_t>(h, version);
	put<std::uint32_t>(h, 0);
	h.append(reinterpret_cast<const char*>(segment_id.data()), 16);
	return h;
}

// The section's CRC and its region CRCs over bytes.
inline section_entry describe(section kind, std::uint64_t offset, std::string_view bytes) {
	section_entry e{kind, offset, bytes.size(), hashes::crc32c(bytes.data(), bytes.size()), {}};
	for (std::size_t at = 0; at < bytes.size(); at += region_size) {
		e.region_crcs.push_back(hashes::crc32c(bytes.data() + at, std::min(region_size, bytes.size() - at)));
	}
	return e;
}

inline std::string encode_toc(const std::vector<section_entry>& entries) {
	std::string t;
	put<std::uint32_t>(t, std::uint32_t(entries.size()));
	for (const auto& e : entries) {
		put<std::uint16_t>(t, std::uint16_t(e.kind));
		put<std::uint16_t>(t, 0);
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
};

inline std::string encode_footer(const footer& f) {
	std::string out;
	put<std::uint64_t>(out, f.toc_offset);
	put<std::uint64_t>(out, f.toc_length);
	put<std::uint32_t>(out, f.toc_crc);
	put<std::uint32_t>(out, f.doc_count);
	put<std::uint64_t>(out, f.min_docid);
	put<std::uint64_t>(out, f.max_docid);
	put<std::uint32_t>(out, hashes::crc32c(out.data(), out.size()));
	out.append(footer_magic, 4);
	return out;
}

inline footer decode_footer(std::string_view file) {
	if (file.size() < header_size + footer_size) { throw corrupt_segment("file too small"); }
	const char* p = file.data() + file.size() - footer_size;
	if (std::memcmp(p + 44, footer_magic, 4) != 0) { throw corrupt_segment("footer magic"); }
	if (hashes::crc32c(p, 40) != get<std::uint32_t>(p + 40)) { throw corrupt_segment("footer checksum"); }
	footer f{get<std::uint64_t>(p), get<std::uint64_t>(p + 8), get<std::uint32_t>(p + 16), get<std::uint32_t>(p + 20),
		get<std::uint64_t>(p + 24), get<std::uint64_t>(p + 32)};
	const std::uint64_t body = file.size() - footer_size;
	if (f.toc_offset > body || f.toc_length > body - f.toc_offset || f.toc_offset + f.toc_length != body) { throw corrupt_segment("table of contents bounds"); }
	return f;
}

// Parses and checks the table of contents: its CRC, entry bounds, and every section's
// placement inside the file before the table of contents.
inline std::vector<section_entry> decode_toc(std::string_view file, const footer& f) {
	std::string_view t = file.substr(f.toc_offset, f.toc_length);
	if (hashes::crc32c(t.data(), t.size()) != f.toc_crc) { throw corrupt_segment("table of contents checksum"); }
	const char* p = t.data();
	const char* end = p + t.size();
	auto need = [&](std::size_t n) {
		if (std::size_t(end - p) < n) { throw corrupt_segment("table of contents truncated"); }
	};
	need(4);
	std::uint32_t n = get<std::uint32_t>(p);
	p += 4;
	if (n > 64) { throw corrupt_segment("section count"); }
	std::vector<section_entry> entries;
	for (std::uint32_t i = 0; i < n; ++i) {
		need(24);
		section_entry e;
		e.kind = section(get<std::uint16_t>(p));
		e.offset = get<std::uint64_t>(p + 4);
		e.length = get<std::uint64_t>(p + 12);
		e.crc = get<std::uint32_t>(p + 20);
		p += 24;
		if (e.offset > f.toc_offset || e.length > f.toc_offset - e.offset) { throw corrupt_segment("section bounds"); }
		std::size_t regions = (e.length + region_size - 1) / region_size;
		need(regions * 4);
		for (std::size_t r = 0; r < regions; ++r) { e.region_crcs.push_back(get<std::uint32_t>(p + 4 * r)); }
		p += regions * 4;
		entries.push_back(std::move(e));
	}
	if (p != end) { throw corrupt_segment("table of contents trailing bytes"); }
	return entries;
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

// --- term info (the termdict value) -----------------------------------------------------------

enum term_flags : std::uint8_t {
	no_wdf = 1,           // every posting has wdf 0 (boolean term)
	no_positions = 2,     // the term has no position stream
	single_posting = 4,   // df == 1: ordinal and wdf inline, no postings bytes
	counts_are_wdf = 8,   // every document's position count equals its wdf
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
	std::uint32_t max_wdf = 0;
	std::vector<impact> impacts;  // whole-list competitive pairs
	std::uint64_t postings = 0;   // offset in `postings`, or the ordinal when single_posting
	std::uint64_t positions = 0;  // offset in `positions`
	std::uint32_t single_wdf = 0; // wdf when single_posting
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
	if (n == 0 || n > 256) { throw corrupt_segment("impact count"); }  // lengths are 1 byte
	for (std::size_t k = 0; k < n; ++k) {
		impact i;
		i.wdf = std::uint32_t(get_leb(p, end));
		if (*p == end) { throw corrupt_segment("impacts truncated"); }
		i.length = std::uint8_t(*(*p)++);
		out.push_back(i);
	}
	return n;
}

inline std::vector<impact> decode_impacts(const char** p, const char* end) {
	std::vector<impact> out;
	decode_impacts(p, end, out);
	return out;
}

inline std::string encode_term_info(const term_info& t) {
	std::string out;
	out.push_back(char(t.flags));
	put_leb(out, t.df);
	put_leb(out, t.cf);
	put_leb(out, t.max_wdf);
	encode_impacts(out, t.impacts);
	put_leb(out, t.postings);
	if (t.flags & single_posting) { put_leb(out, t.single_wdf); }
	if (!(t.flags & no_positions)) { put_leb(out, t.positions); }
	return out;
}

inline term_info decode_term_info(std::string_view bytes) {
	const char* p = bytes.data();
	const char* end = p + bytes.size();
	term_info t;
	if (p == end) { throw corrupt_segment("term info truncated"); }
	t.flags = std::uint8_t(*p++);
	if (t.flags & ~0x0f) { throw corrupt_segment("term flags"); }
	t.df = std::uint32_t(get_leb(&p, end));
	t.cf = get_leb(&p, end);
	t.max_wdf = std::uint32_t(get_leb(&p, end));
	t.impacts = decode_impacts(&p, end);
	t.postings = get_leb(&p, end);
	if (t.flags & single_posting) { t.single_wdf = std::uint32_t(get_leb(&p, end)); }
	if (!(t.flags & no_positions)) { t.positions = get_leb(&p, end); }
	if (p != end || t.df == 0 || ((t.flags & single_posting) != 0) != (t.df == 1)) { throw corrupt_segment("term info"); }
	return t;
}

// The competitive pairs of a list of (wdf, length) pairs: its Pareto frontier (higher
// wdf, lower length), highest wdf first. At most 256 pairs, one per length code.
inline std::vector<impact> competitive(std::vector<impact> pairs) {
	std::sort(pairs.begin(), pairs.end(), [](const impact& a, const impact& b) { return a.wdf != b.wdf ? a.wdf > b.wdf : a.length < b.length; });
	std::vector<impact> frontier;
	for (const auto& p : pairs) {
		if (frontier.empty() || p.length < frontier.back().length) { frontier.push_back(p); }
	}
	if (frontier.empty()) { frontier.push_back({}); }
	return frontier;
}

}  // namespace format
}  // namespace strata
