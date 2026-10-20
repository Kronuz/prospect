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

// The in-memory segment over a layout: the core's per-document fields and docids column, each
// plugin's builder (layers.md L5), and one sequential pass that writes the file. The keys and
// stored builders live here; the inverted builder is search/writer.hh's.

#pragma once

#include "compressor_zstd.h"
#include "fusefilter.hh"
#include "hashes.hh"
#include <crc32c/crc32c.hh>
#include "strata/core/budget.hh"
#include "strata/core/docids.hh"
#include "strata/core/format.hh"
#include "strata/core/keys.hh"
#include "strata/core/reader.hh"
#include "strata/core/section_io.hh"
#include "task.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace strata {

// Per-document fields the core hands every builder (layers.md L5).
struct doc_fields {
	std::uint64_t docid;
	std::uint64_t version;  // 0 for a keyless document
	std::string_view key;   // empty for a keyless document
};

namespace detail {
// Appends a section built only to be appended, charged while it lives.
inline void append_built(section_sink& sink, flush_charge* memory, format::section kind, const std::string& bytes) {
	if (memory) { memory->grow(bytes.capacity()); }
	sink.write(kind, bytes);
	if (memory) { memory->shrink(bytes.capacity()); }
}
// The core's sections that frame a segment (F5): the header first; docids and stats (the
// document count, F7) held for the tail; then the table of contents and the footer, which
// records the structures built (F4).
inline void begin_segment(section_sink& sink, const std::array<std::uint8_t, 16>& segment_id) {
	sink.write(format::section::header, format::encode_header(segment_id));
}
inline void write_docids(section_sink& sink, flush_charge* memory, std::span<const std::uint64_t> docids) {
	append_built(sink, memory, format::section::docids, docids::encode(docids, memory));
}
inline void finish_segment(section_sink& sink, std::span<const std::uint64_t> docids, format::structure_bits structures) {
	std::string stats;
	format::put<std::uint64_t>(stats, docids.size());
	sink.write(format::section::stats, stats);
	sink.finish();
	counting_output& o = sink.out();
	o.align(format::section_alignment);
	format::footer f;
	f.toc_offset = o.size();
	std::string toc_bytes = format::encode_toc(sink.toc());
	f.toc_length = toc_bytes.size();
	f.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
	f.doc_count = std::uint32_t(docids.size());
	f.min_docid = docids.empty() ? 0 : *std::min_element(docids.begin(), docids.end());
	f.max_docid = docids.empty() ? 0 : *std::max_element(docids.begin(), docids.end());
	f.structures = structures;
	o.write(toc_bytes);
	o.write(format::encode_footer(f));
}
// Charges a column's growth to `now` bytes, when a budget is given.
inline void charge_to(flush_charge* memory, std::uint64_t now, std::uint64_t& charged) {
	if (memory && now > charged) {
		memory->grow(now - charged);
		charged = now;
	}
}
// Bytes copied into chunks of at least `chunk` bytes that never move, so views into them
// stay valid; each chunk is charged as it is allocated.
class stable_bytes {
public:
	explicit stable_bytes(std::size_t chunk) noexcept : chunk_(chunk) {}
	std::string_view copy(std::string_view s, flush_charge* memory) {
		if (chunks_.empty() || chunks_.back().size() + s.size() > chunks_.back().capacity()) {
			chunks_.emplace_back();
			chunks_.back().reserve(std::max(chunk_, s.size()));
			if (memory) { memory->grow(chunks_.back().capacity()); }
		}
		std::string& c = chunks_.back();
		const char* at = c.data() + c.size();
		c.append(s);
		return {at, s.size()};
	}

private:
	std::deque<std::string> chunks_;
	std::size_t chunk_;
};
}  // namespace detail

// What the writer tells each builder before it works: the settings every builder reads as they
// are when a call starts.
struct builder_settings {
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;
	std::string_view key_prefix;
	format::structure_bits structures = 0;
};

// Keys and versions: the keys plugin's builder (F8, F9). keys streams with the plugins'
// sections; key_index and versions go to the tail.
class keys_builder {
public:
	flush_charge* memory = nullptr;

	void configure(const builder_settings& s) noexcept { memory = s.memory; }

	// A borrowed key is held as a view, so its bytes must outlive finish() (the flush's
	// buffer, the merge's key map); any other is copied once into the builder's own chunks.
	void add(const doc_fields& f, bool borrowed) {
		// A version belongs to a key (F9, F13-R9).
		if (f.key.empty() && f.version != 0) { throw std::invalid_argument("strata: a version without a key"); }
		if (!f.key.empty()) {
			if (f.key.size() > format::max_term_bytes) { throw std::invalid_argument("strata: key longer than the term limit"); }
			const std::string_view k = borrowed ? f.key : owned_.copy(f.key, memory);
			keys_.push_back({k.data(), std::uint32_t(k.size()), ordinal_t(versions_.size())});
		}
		versions_.push_back(f.version);
		detail::charge_to(memory, versions_.capacity() * sizeof(std::uint64_t) + keys_.capacity() * sizeof(keys::entry), charged_);
	}
	// The entries are sorted in place, so a second flush writes the same bytes.
	void finish(section_sink& sink) {
		keys::sections sections = keys::encode(keys_, memory);
		const std::uint64_t built = sections.keys.capacity() + sections.index.capacity();
		if (memory) { memory->grow(built); }
		sink.write(format::section::keys, sections.keys);
		detail::append_built(sink, memory, format::section::versions, encode_versions());
		sink.write(format::section::key_index, sections.index);
		if (memory) { memory->shrink(built); }
	}

private:
	// versions: [min u64][width u8][(version - min) in width bits each, little-endian bit order].
	std::string encode_versions() const {
		std::string out;
		std::uint64_t lo = versions_.empty() ? 0 : *std::min_element(versions_.begin(), versions_.end());
		const std::uint64_t hi = versions_.empty() ? 0 : *std::max_element(versions_.begin(), versions_.end());
		unsigned width = unsigned(std::bit_width(hi - lo));
		// The reader requires lo + 2^width - 1 to fit in 64 bits; when it would not, the base is
		// 0 and the width 64, since every version then exceeds 2^63 (F13-R25 item 7).
		if (width && lo > ~std::uint64_t(0) - (width >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << width) - 1)) {
			lo = 0;
			width = 64;
		}
		format::put<std::uint64_t>(out, lo);
		out.push_back(char(width));
		std::string bits((versions_.size() * width + 7) / 8, '\0');
		for (std::size_t i = 0; i < versions_.size(); ++i) {
			const std::uint64_t v = versions_[i] - lo;
			for (unsigned b = 0; b < width; ++b) {
				if (v >> b & 1) {
					const std::size_t at = i * width + b;
					bits[at >> 3] = char(std::uint8_t(bits[at >> 3]) | (1u << (at & 7)));
				}
			}
		}
		return out + bits;
	}

	std::vector<std::uint64_t> versions_;
	std::vector<keys::entry> keys_;
	detail::stable_bytes owned_{std::size_t(64) << 10};  // the keys not borrowed
	std::uint64_t charged_ = 0;
};

// Stored records: the stored plugin's builder.
class stored_builder {
public:
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;  // checked per frame

	void configure(const builder_settings& s) noexcept {
		memory = s.memory;
		cancel = s.cancel;
	}
	void add(ordinal_t, const doc_fields&, const stored_plugin::part& p) { add(p.stored); }

	void add(std::string_view stored) {
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		detail::charge_to(memory, stored_ends_.capacity() * sizeof(std::uint64_t) + stored_.capacity(), charged_);
	}
	void finish(section_sink& sink) { detail::append_built(sink, memory, format::section::stored, encode_stored()); }

private:
	// Stored records: zstd frames back to back, each the payload [count][record lengths]
	// [records] (LEB128 counts and lengths), then one 16-byte index entry per frame (first
	// ordinal u32, uncompressed size u32, offset u64), then the frame count (u32).
	std::string encode_stored() const {
		std::string out, index, raw;
		std::uint32_t frames = 0;
		for (std::size_t first = 0; first < stored_ends_.size();) {
			if (cancel && cancel->requested()) { throw task::cancelled{}; }
			std::size_t last = first;
			const std::uint64_t begin = first ? stored_ends_[first - 1] : 0;
			while (last + 1 < stored_ends_.size() && stored_ends_[last + 1] - begin <= format::stored_block_size) { ++last; }
			raw.clear();
			format::put_leb(raw, last - first + 1);
			for (std::size_t i = first; i <= last; ++i) { format::put_leb(raw, stored_ends_[i] - (i ? stored_ends_[i - 1] : 0)); }
			raw.append(stored_, begin, stored_ends_[last] - begin);
			if (raw.size() > std::numeric_limits<std::uint32_t>::max()) { throw std::invalid_argument("strata: stored record too large"); }
			format::put<std::uint32_t>(index, std::uint32_t(first));
			format::put<std::uint32_t>(index, std::uint32_t(raw.size()));
			format::put<std::uint64_t>(index, out.size());
			out += compress_zstd(raw);
			++frames;
			first = last + 1;
		}
		out += index;
		format::put<std::uint32_t>(out, frames);
		return out;
	}

	std::string stored_;                      // stored records, concatenated
	std::vector<std::uint64_t> stored_ends_;  // where each document's record ends
	std::uint64_t charged_ = 0;
};

template <class Layout>
class basic_segment_writer;

// The in-memory segment of a layout: per-document fields and docids in the core, everything
// else in the plugins' builders, which the layout's entries name (`P::builder`).
template <class... Entries>
class basic_segment_writer<layout<Entries...>> {
	static constexpr bool keyed = (std::is_same_v<Entries, keys_plugin> || ...);

public:
	// Documents arrive in output order with distinct docids in any order (a reordering
	// merge, reordering.md D2); flushes leave it off, so docids increase.
	bool permuted = false;
	// The verification region size of every section, as a power of two.
	std::uint8_t region_shift = format::default_region_shift;
	// The run's draw on the indexing budget, if any: the builders' buffers and the flush's
	// scratch and output report their allocations.
	flush_charge* memory = nullptr;
	// When given, the stored section checks it per frame and throws task::cancelled.
	const task::cancel_token* cancel = nullptr;
	// The index's key prefix, for the inverted builder's backstop (F8); empty for none.
	std::string key_prefix;
	// The structures the segment holds, set from the index's configuration, never from its
	// documents (F4).
	format::structure_bits structures = (format::structure_bits(0) | ... | Entries::structures);

	// Plugin P's builder.
	template <class P>
	typename P::builder& builder() noexcept {
		return std::get<typename P::builder>(builders_);
	}
	std::size_t doc_count() const noexcept { return docids_.size(); }

	// The core's step for one document: the limits, docid order, the docids column, and the
	// key and version (F8, F9); returns its ordinal. A borrowed key's bytes outlive the flush
	// (keys_builder::add). The plugins' parts follow, through add() or the builders.
	ordinal_t begin(const doc_fields& f, bool borrowed) {
		sync();
		// Ordinals are 32-bit and the largest value means no more documents.
		if (docids_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (f.docid == 0 || (!permuted && !docids_.empty() && f.docid <= docids_.back())) { throw std::invalid_argument("strata: docids must increase within a segment"); }
		// Only configured builders collect: a keyless segment holds no versions it never writes
		// (CR8). A version belongs to a key (F9).
		if constexpr (keyed) {
			if (structures & keys_plugin::structure) {
				builder<keys_plugin>().add(f, borrowed);
			} else {
				refuse_keyless(f);
			}
		} else {
			refuse_keyless(f);
		}
		docids_.push_back(f.docid);
		detail::charge_to(memory, docids_.capacity() * sizeof(std::uint64_t), charged_);
		return ordinal_t(docids_.size() - 1);
	}

	// A document: begin(), then each plugin's part of `doc`, in layout order. A part for a
	// structure the segment does not build is dropped, or refused where the plugin says so.
	template <class Doc>
	ordinal_t add(const doc_fields& f, const Doc& doc, bool borrowed) {
		(refuse_part<Entries>(doc), ...);
		const ordinal_t ord = begin(f, borrowed);
		(add_part<Entries>(ord, f, doc), ...);
		return ord;
	}

	// Writes the segment file and returns its bytes.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id) {
		std::string file;
		string_output out(file, memory);
		string_spill positions(memory);
		flush(segment_id, out, positions);
		return file;
	}

	// Streams the segment file to out in one pass. A section produced alongside another (the
	// positions beside the postings) goes to `held` until the other ends.
	void flush(const std::array<std::uint8_t, 16>& segment_id, output& out, spill& held) {
		write(segment_id, out, held, [](auto& b, section_sink& s) { b.finish(s); });
	}

	// The file in F5's order: the header; each configured builder's sections, in layout order
	// (`finish(builder, sink)` writes them; the streamed ones in rank order); the docids and the
	// pinned tail by rank; then the table of contents and footer, with the structures (F4).
	template <class Finish>
	void write(const std::array<std::uint8_t, 16>& segment_id, output& out, spill& held, Finish&& finish) {
		sync();
		if ((structures & ~(format::structure_bits(0) | ... | Entries::structures)) || !(consistent<Entries>() && ...)) { throw std::invalid_argument("strata: segment structures"); }
		section_sink sink(out, region_shift, &held, memory);
		detail::begin_segment(sink, segment_id);
		(finish_entry<Entries>(sink, finish), ...);
		detail::write_docids(sink, memory, docids_);
		detail::finish_segment(sink, docids_, structures);
	}

protected:
	// Builders read the writer's settings as they are when each call starts.
	void sync() {
		const builder_settings s{memory, cancel, key_prefix, structures};
		(builder<Entries>().configure(s), ...);
	}

private:
	static void refuse_keyless(const doc_fields& f) {
		if (!f.key.empty()) { throw std::invalid_argument("strata: a key in a segment without keys"); }
		if (f.version != 0) { throw std::invalid_argument("strata: a version without a key"); }
	}
	template <class E, class Doc>
	void refuse_part(const Doc& doc) const {
		if constexpr (requires { typename E::part; E::refuse(static_cast<const typename E::part&>(doc), true); }) {
			E::refuse(static_cast<const typename E::part&>(doc), (structures & E::structure) != 0);
		}
	}
	template <class E, class Doc>
	void add_part(ordinal_t ord, const doc_fields& f, const Doc& doc) {
		if constexpr (requires { typename E::part; }) {
			if (structures & E::structure) { builder<E>().add(ord, f, static_cast<const typename E::part&>(doc)); }
		}
	}
	template <class E>
	bool consistent() const {
		if constexpr (requires { E::consistent(structures); }) {
			return E::consistent(structures);
		} else {
			return true;
		}
	}
	template <class E, class Finish>
	void finish_entry(section_sink& sink, Finish& finish) {
		if (structures & E::structure) { finish(builder<E>(), sink); }
	}

	std::tuple<typename Entries::builder...> builders_;
	std::vector<std::uint64_t> docids_;
	std::uint64_t charged_ = 0;  // the docids column's charged capacity
};

}  // namespace strata
