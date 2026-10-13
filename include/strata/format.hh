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
inline constexpr std::uint32_t version = 12;
inline constexpr std::size_t header_size = 32;
inline constexpr std::size_t footer_size = 48;
inline constexpr std::size_t region_size = 64 * 1024;
// Stored records are packed into zstd frames of at most this many uncompressed bytes (a
// larger record takes a frame of its own).
inline constexpr std::size_t stored_block_size = 64 * 1024;
// The largest stored record a document may carry; a frame then holds at most one block
// of records or one record this size, plus its length header, which bounds what reading
// a frame allocates even when a file's checksums were recomputed (review C9).
inline constexpr std::uint64_t max_stored_record = 64ull << 20;
inline constexpr std::uint64_t max_stored_frame = stored_block_size + max_stored_record + 64;
inline constexpr std::size_t section_alignment = 64;
inline constexpr std::size_t block = 128;

// Kinds of the docids section: docid = base + ordinal, or strictly increasing deltas.
inline constexpr std::uint8_t docids_contiguous = 0;
inline constexpr std::uint8_t docids_monotone = 1;

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
	pk_filter = 11,  // the key terms' range and a binary fuse filter over their hashes
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
	// Each kind at most once; sections never overlap.
	std::vector<const section_entry*> sorted;
	for (const auto& e : entries) {
		for (const auto* o : sorted) {
			if (o->kind == e.kind) { throw corrupt_segment("duplicate section"); }
		}
		sorted.push_back(&e);
	}
	std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->offset < b->offset; });
	for (std::size_t i = 1; i < sorted.size(); ++i) {
		if (sorted[i - 1]->offset + sorted[i - 1]->length > sorted[i]->offset) { throw corrupt_segment("overlapping sections"); }
	}
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
	if (n == 0 || n > 256) { throw corrupt_segment("impact count"); }
	const std::uint64_t size = (h & 1) ? 2 * n : get_leb(p, end);
	if (size > std::uint64_t(end - *p)) { throw corrupt_segment("impacts bounds"); }
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
		if (size != std::uint64_t(end - p)) { throw corrupt_segment("impacts length"); }
		const std::uint64_t m = get_leb(&p, end);
		if (m != n) { throw corrupt_segment("impact count"); }
	}
	for (std::uint64_t k = 0; k < n; ++k) {
		impact i;
		if (h & 1) {
			if (end - p < 2) { throw corrupt_segment("impacts truncated"); }
			i.wdf = std::uint8_t(*p++);
		} else {
			i.wdf = std::uint32_t(get_leb(&p, end));
			if (p == end) { throw corrupt_segment("impacts truncated"); }
		}
		i.length = std::uint8_t(*p++);
		if (f(i)) { return true; }
	}
	if (p != end) { throw corrupt_segment("impacts length"); }
	return false;
}

inline std::vector<impact> decode_impacts(const char** p, const char* end) {
	std::vector<impact> out;
	decode_impacts(p, end, out);
	return out;
}

// Term infos in one dictionary restart run share a cursor: a term's postings and
// positions start where the previous term's end. A run's first entry, or one after a gap,
// stores both cursors (flag `absolute`); the rest only their lengths. Layout: [flags u8]
// [df][cf unless cf_is_df][if absolute: postings cursor, positions cursor], then for a
// single posting [ordinal][wdf], else [postings length]; with positions [positions
// length]; with skip blocks (df >= 128) the impacts. Integers are LEB128.
struct term_run {
	std::uint64_t postings = 0, positions = 0;  // where the next term's bytes start
	bool started = false;
};

inline std::string encode_term_info(term_info t, term_run& run, bool restart) {
	const bool singles = t.flags & single_posting, positionless = t.flags & no_positions;
	const bool contiguous = run.started && (singles || t.postings == run.postings) && (positionless || t.positions == run.positions);
	t.flags = std::uint8_t(t.flags & ~(cf_is_df | absolute));
	if (t.cf == t.df) { t.flags |= cf_is_df; }
	if (restart || !contiguous) {
		t.flags |= absolute;
		if (!singles) { run.postings = t.postings; }
		if (!positionless) { run.positions = t.positions; }
	}
	std::string out;
	out.push_back(char(t.flags));
	put_leb(out, t.df);
	if (!(t.flags & cf_is_df)) { put_leb(out, t.cf); }
	if (t.flags & absolute) {
		put_leb(out, run.postings);
		put_leb(out, run.positions);
	}
	if (singles) {
		put_leb(out, t.postings);
		put_leb(out, t.single_wdf);
	} else {
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
	if (p == end) { throw corrupt_segment("term info truncated"); }
	t.flags = std::uint8_t(*p++);
	if (t.flags & ~0x3f) { throw corrupt_segment("term flags"); }
	if (!(t.flags & absolute) && !run.started) { throw corrupt_segment("term offsets without a run"); }
	t.df = std::uint32_t(get_leb(&p, end));
	t.cf = t.flags & cf_is_df ? t.df : get_leb(&p, end);
	if (t.flags & absolute) {
		run.postings = get_leb(&p, end);
		run.positions = get_leb(&p, end);
	}
	if (t.flags & single_posting) {
		t.postings = get_leb(&p, end);
		t.single_wdf = std::uint32_t(get_leb(&p, end));
	} else {
		t.postings_length = get_leb(&p, end);
		t.postings = run.postings;
		if (run.postings + t.postings_length < run.postings) { throw corrupt_segment("postings range"); }
		run.postings += t.postings_length;
	}
	if (!(t.flags & no_positions)) {
		t.positions_length = get_leb(&p, end);
		t.positions = run.positions;
		if (run.positions + t.positions_length < run.positions) { throw corrupt_segment("positions range"); }
		run.positions += t.positions_length;
	}
	if (t.df >= block) {
		if (with_impacts) {
			t.impacts = decode_impacts(&p, end);
		} else {
			p = end;
		}
	}
	if (p != end || t.df == 0 || ((t.flags & single_posting) != 0) != (t.df == 1)) { throw corrupt_segment("term info"); }
	run.started = true;
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
