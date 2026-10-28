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

// The search plugin's reading side: the inverted plugin's reader (terms, postings, positions,
// lengths, norms), the postings iterator and positions reader, the search layout and its
// aliases (`segment`, `document`).

#pragma once

#include "bitpack.hh"
#include "docset.hh"
#include "prospect/format.hh"
#include "prospect/strata.hh"
#include "prospect/values.hh"
#include "strata/core/reader.hh"
#include "prospect/fielddict.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prospect {

static_assert(docset::no_more_docs == no_more_docs, "the ordinal sentinel is docset's");

// One term of a document as the host builds it; terms are unique within a document.
struct term_entry {
	std::string term;
	std::uint32_t wdf = 1;
	std::vector<std::uint32_t> positions;
};
// A token of a document's text: the term at [offset, offset + size) of the text, at a position.
struct text_token {
	std::uint32_t offset = 0, size = 0, position = 0;
};

namespace text_detail {
// The high bit of each byte of w that is a space, exactly.
inline std::uint64_t spaces(std::uint64_t w) noexcept {
	constexpr std::uint64_t ones = 0x0101010101010101ull, low7 = 0x7F7F7F7F7F7F7F7Full;
	const std::uint64_t x = w ^ (ones * ' ');
	return ~(((x & low7) + low7) | x | low7);
}

// Calls f(offset, size) for each word of text, a maximal run of bytes other than ' ', in
// order, 8 bytes at a time.
template <class F>
inline void split_words(std::string_view text, F&& f) {
	std::size_t start = 0;  // where the current word starts
	auto end = [&](std::size_t j) {
		if (j > start) { f(start, j - start); }
		start = j + 1;
	};
	std::size_t at = 0;
	if constexpr (std::endian::native == std::endian::little) {
		for (; at + 8 <= text.size(); at += 8) {
			std::uint64_t w;
			std::memcpy(&w, text.data() + at, 8);
			for (std::uint64_t m = spaces(w); m; m &= m - 1) { end(at + std::size_t(std::countr_zero(m)) / 8); }
		}
	}
	for (; at < text.size(); ++at) {
		if (text[at] == ' ') { end(at); }
	}
	end(text.size());
}

// The number of words in text and their bytes, without splitting it.
struct word_counts {
	std::uint64_t words = 0, bytes = 0;
};
inline word_counts count_words(std::string_view text) noexcept {
	word_counts c;
	bool in_word = false;  // the byte before
	std::size_t at = 0;
	if constexpr (std::endian::native == std::endian::little) {
		for (; at + 8 <= text.size(); at += 8) {
			std::uint64_t w;
			std::memcpy(&w, text.data() + at, 8);
			const std::uint64_t letters = ~spaces(w) & 0x8080808080808080ull;
			const std::uint64_t before = letters << 8 | (in_word ? 0x80u : 0u);  // each byte's previous byte, in words
			c.words += std::uint64_t(std::popcount(letters & ~before));
			c.bytes += std::uint64_t(std::popcount(letters));
			in_word = letters >> 63;
		}
	}
	for (; at < text.size(); ++at) {
		const bool letter = text[at] != ' ';
		c.words += letter && !in_word;
		c.bytes += letter;
		in_word = letter;
	}
	return c;
}

// Whether a text splits into words that fit format 12: at most 2^32 - 1 of them (32-bit
// positions and lengths), none over the term limit. Only a text past 2^32 - 1 bytes can
// break either, so only such a text is read.
inline bool splittable(std::string_view text, std::uint64_t max_word) noexcept {
	constexpr std::uint64_t most = std::numeric_limits<std::uint32_t>::max();
	if (text.size() <= most) { return true; }
	std::uint64_t words = 0;
	bool ok = true;
	split_words(text, [&](std::size_t, std::size_t size) { ok = ok && ++words <= most && size <= max_word; });
	return ok;
}
}  // namespace text_detail

class inverted_builder;
class inverting_buffer;
struct inverted_plugin;
using search_layout = layout<inverted_plugin, keys_plugin, stored_plugin, values_plugin>;

}  // namespace prospect

// Declared here, defined in reorder.hh: a translation unit that names the search layout without
// reorder.hh fails to compile rather than take the primary's "no order source" and a different
// index_options layout than the rest of the program.
template <>
struct strata::order_source<prospect::search_layout>;

namespace prospect {

using segment = basic_segment<search_layout>;

// Terms, postings, positions, lengths, and norms.
struct inverted_plugin {
	static constexpr std::string_view id = "inverted";

	// One field's share of a document (L10, fields-and-keys D5, D9). `tokens` index `text` in
	// order, positions strictly increasing; each adds one to its term's wdf and its position
	// to the term's positions. A term is in `terms` or among the tokens, not both. With
	// `split_text`, `tokens` is empty, `length` is 0, and the text's words (maximal runs of
	// bytes other than ' ') are its tokens at positions 1, 2, ...; the index counts them as
	// its length. `length` is the host's sum of the wdf of the field's terms.
	struct field_part {
		field_ref field;
		std::vector<term_entry> terms;
		std::uint32_t length = 0;
		std::string text;
		std::vector<text_token> tokens;
		bool split_text = false;
	};
	// A document's terms by field. Its length is the host's total, which defaults to the sum
	// of its parts' lengths (D5): Xapiand passes the sum of every wdf, its id term's included.
	struct part {
		std::vector<field_part> fields;
		std::optional<std::uint32_t> length;
		// The part naming `ref`, added empty when absent.
		field_part& field(field_ref ref = field_ref::declared(0)) {
			for (auto& f : fields) {
				if (f.field == ref) { return f; }
			}
			fields.emplace_back();
			fields.back().field = ref;
			return fields.back();
		}
	};
	// The document's length: the host's, or the sum of its parts'.
	static std::uint64_t document_length(const part& p) {
		if (p.length) { return *p.length; }
		std::uint64_t n = 0;
		for (const auto& f : p.fields) { n += f.split_text ? text_detail::count_words(f.text).words : f.length; }
		return n;
	}
	// Stage-1 checks (D10): every field part names a field the transaction resolves, other
	// than the key field, once; terms within the term limit (the empty term is a term),
	// unique, positions increasing; tokens in bounds and increasing, none equal to a term of
	// the part; the document's length fits 32 bits.
	static bool validate(const validate_context& vc, const part& p) {
		thread_local std::vector<std::uint32_t> ids;  // sorted, to find repeats
		ids.clear();
		for (const auto& f : p.fields) {
			const auto id = vc.field(f.field);
			if (!id || vc.is_key_field(*id)) { return false; }
			ids.push_back(*id);
			if (!valid_field(f)) { return false; }
		}
		if (ids.size() > 1) {
			std::sort(ids.begin(), ids.end());
			if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) { return false; }
		}
		return document_length(p) <= std::numeric_limits<std::uint32_t>::max();
	}
	// Rewrites every field reference to the id it resolved to (the buffer stores ids).
	static void resolve(part& p, const field_resolution& r) {
		for (auto& f : p.fields) {
			if (const auto id = r.resolve(f.field)) { f.field = field_ref::id(*id); }
		}
	}
	// Buffer memory and an overestimate of disk bytes.
	static std::uint64_t bytes(const part& p) {
		std::uint64_t n = 0;
		for (const auto& f : p.fields) {
			n += sizeof(field_part) + f.text.size() + sizeof(text_token) * f.tokens.size();
			for (const auto& t : f.terms) { n += sizeof(term_entry) + t.term.size() + 4 * t.positions.size(); }
		}
		return n;
	}
	static std::uint64_t disk_estimate(const part& p) {
		std::uint64_t n = 0;
		for (const auto& f : p.fields) {
			for (const auto& t : f.terms) { n += t.term.size() + 12 + 4 * t.positions.size(); }
			for (const auto& tk : f.tokens) { n += tk.size + 16; }  // as a term of its own
			if (f.split_text) {
				const text_detail::word_counts w = text_detail::count_words(f.text);
				n += w.bytes + 16 * w.words;
			}
		}
		return n;
	}
	// Its builder (search/writer.hh) and the buffer that inverts documents as they arrive at
	// one flush thread (search/inverter.hh).
	using builder = inverted_builder;
	using inverter = inverting_buffer;
	// Norms are a sub-structure of inverted: a segment cannot hold them without it (F4).
	static bool consistent(format::structure_bits s) noexcept { return !(s & format::bit::norms) || (s & format::bit::inverted); }
	// A document as the index buffers it: a text split at spaces has its length counted now.
	static void buffered(part& p) {
		for (auto& f : p.fields) {
			if (f.split_text) { f.length = std::uint32_t(text_detail::count_words(f.text).words); }
		}
	}
	// The bytes of its statistics at the head of term_index (F7).
	static constexpr std::size_t statistics_size = 20;
	// Its sections are F3's rows owned by it; its bits are F4's (L4).
	static constexpr format::owner owner = format::owner::inverted;
	static constexpr format::structure_bits structure = format::bit::inverted;
	static constexpr format::structure_bits structures = format::bit::inverted | format::bit::norms;

	class reader {
	public:
		explicit reader(const open_context& c) : state_(std::make_unique<state>()), doc_count_(c.doc_count) {
			if (!c.present) {
				open_absent(c);
				return;
			}
			// term_index: [total length u64][minimum length u32][maximum length u32][most
			// positions u32], then the dictionary's index (F7), all under its section CRC.
			const std::string_view term_index = c.section(format::section::term_index).bytes;
			if (term_index.size() < statistics_size) { corrupt("term index statistics"); }
			total_length_ = format::get<std::uint64_t>(term_index.data());
			min_length_ = format::get<std::uint32_t>(term_index.data() + 8);
			max_length_ = format::get<std::uint32_t>(term_index.data() + 12);
			max_positions_ = format::get<std::uint32_t>(term_index.data() + 16);
			state_->terms = c.section(format::section::terms);
			postings_ = c.section(format::section::postings);
			positions_ = c.section(format::section::positions);
			lengths_ = c.section(format::section::lengths).bytes;
			if (const auto* e = c.core.entry(format::section::lengths)) { lengths_crc_ = e->crc; }
			if (const auto* e = c.core.entry(format::section::postings)) { postings_shift_ = e->region_shift; }
			norms_ = c.section(format::section::norms).bytes;
			// lengths' bytes, its width included, are read on first use only (F7, T7).
			if (lengths_.empty()) { corrupt("lengths"); }
			// Norms not built (F4) are derived from the exact lengths on first use, so an open
			// reads no byte of lengths (T7; F13-R36).
			if (c.core.structures() & format::bit::norms) {
				if (norms_.size() != doc_count_) { corrupt("norms size"); }
				norms_at_ = norms_.data();
			}
			// The term index is copied out of the mapping (verified at open), so later lookups
			// never read bytes that could change or fault under the FST's unchecked traversal.
			state_->pinned_index.assign(term_index.substr(statistics_size));
			// The directory's header is checked here; a field's dictionary opens (and its FST is
			// validated) on first use (fields-and-keys T6).
			dict_.emplace(state_->terms.bytes, state_->pinned_index, &verify_terms, state_.get());
		}

		// A document's encode_length byte: the norms section's, or derived once from lengths
		// when the segment has none, which throws corrupt_segment if lengths is damaged.
		std::uint8_t norm(ordinal_t ord) const {
			const char* n = norms_at_;
			if (!n) [[unlikely]] { n = derived_norms(); }
			return std::uint8_t(n[ord]);
		}
		void verify_postings(std::uint64_t offset, std::uint64_t length) const {
			if (!postings_.verify(offset, length)) { corrupt("postings checksum"); }
		}
		std::uint8_t postings_region_shift() const noexcept { return postings_shift_; }
		void verify_positions(std::uint64_t offset, std::uint64_t length) const {
			if (!positions_.verify(offset, length)) { corrupt("positions checksum"); }
		}
		std::uint32_t max_positions() const noexcept { return max_positions_; }
		// The shortest and longest document length written into the segment (0 for none); deleted documents count.
		std::uint32_t min_length() const noexcept { return min_length_; }
		std::uint32_t max_length() const noexcept { return max_length_; }
		std::uint64_t total_length() const noexcept { return total_length_; }
		double average_length() const noexcept { return doc_count_ ? double(total_length_) / double(doc_count_) : 0.0; }

		// Document length by ordinal: one lane extraction from the bit-packed block. The
		// section is verified whole on the first call (the default scheme reads norms only).
		std::uint32_t length(ordinal_t ord) const {
			if (state_->lengths_state.load(std::memory_order_acquire) != 1) [[unlikely]] { verify_lengths(); }
			const unsigned w = state_->length_width.load(std::memory_order_relaxed);
			if (w == 0) { return 0; }
			const char* block = lengths_.data() + 1 + std::size_t(ord / 128) * bitpack::packed_bytes(w);
			const unsigned j = ord % 128, lane = j & 3, row = j >> 2;
			const unsigned start = row * w, word = start / 32, shift = start % 32;
			std::uint64_t v = format::get<std::uint32_t>(block + 4 * (4 * word + lane)) >> shift;
			if (shift + w > 32) { v |= std::uint64_t(format::get<std::uint32_t>(block + 4 * (4 * (word + 1) + lane))) << (32 - shift); }
			return std::uint32_t(v & (w == 32 ? 0xffffffffu : ((1u << w) - 1)));
		}

		// A term's info, its offsets resolved from its dictionary run and, for a term without
		// skip blocks, its competitive pairs computed from its postings (at most 127).
		std::optional<format::term_info> term(std::uint32_t field, std::string_view key) const {
			auto info = find_term(field, key);
			if (info) { complete_impacts(*info); }
			return info;
		}
		// Computes the competitive pairs find_term leaves out (a term below 128 postings).
		void complete_impacts(format::term_info& info) const {
			if (info.df < format::block) { info.impacts = tail_impacts(info); }
		}
		// Competitive pairs of a term without skip blocks, from its postings.
		std::vector<format::impact> tail_impacts(const format::term_info& info) const;
		// The same as term() from one dictionary walk, leaving a term without skip blocks with
		// no competitive pairs: its df and offsets, for statistics and counts.
		std::optional<format::term_info> find_term(std::uint32_t field, std::string_view key) const {
			// The run's values end with key's: the earlier ones only move the cursor.
			format::term_run run, before;
			std::string_view last;
			const auto v = dict_->find_run(field, key, [&](std::string_view value) {
				before = run;
				(void)format::decode_term_info(value, run, false);
				last = value;
			});
			if (!v) { return std::nullopt; }
			auto info = format::decode_term_info(last, before);
			if (info.df > doc_count_) { corrupt("document frequency"); }
			return info;
		}

		// Calls f(field, term, info) for every term in (field, term) order, until f returns false.
		template <class F>
		void scan_terms(F&& f) const {
			fielddict::reader::cursor c(*dict_);
			while (c.next()) {
				if (c.info().df > doc_count_) { corrupt("document frequency"); }
				if (!f(c.field(), c.term(), c.info())) { break; }
			}
		}
		// Calls f(term, value) for each term of `field` from `from` on, until f returns false;
		// the value is the dictionary's raw bytes.
		template <class F>
		void scan_dictionary(std::uint32_t field, std::string_view from, F&& f) const {
			dict_->scan(field, from, std::forward<F>(f));
		}
		// Whether any term of `field` is in the segment.
		bool has_field(std::uint32_t field) const { return dict_->has_field(field); }

		std::string_view postings_bytes() const noexcept { return postings_.bytes; }
		std::string_view positions_bytes() const noexcept { return positions_.bytes; }
		const fielddict::reader& dictionary() const noexcept { return *dict_; }

		// Every term in (field, term) order with its info (impacts left empty below 128
		// postings), pulled one at a time, for k-way merges. Neither copied nor moved.
		class term_cursor {
		public:
			explicit term_cursor(const reader& r) : r_(&r), c_(r.dictionary()) {}
			term_cursor(const term_cursor&) = delete;
			term_cursor& operator=(const term_cursor&) = delete;
			// Moves to the next term; false past the last.
			bool next() {
				if (!c_.next()) { return false; }
				if (c_.info().df > r_->doc_count()) { corrupt("document frequency"); }
				return true;
			}
			std::uint32_t field() const noexcept { return c_.field(); }
			std::string_view term() const noexcept { return c_.term(); }
			const format::term_info& info() const noexcept { return c_.info(); }

		private:
			const reader* r_;
			fielddict::reader::cursor c_;
		};
		std::uint64_t doc_count() const noexcept { return doc_count_; }

		// Every byte of the sections checked on first read.
		void verify_all() const {
			for (const section_handle* h : {static_cast<const section_handle*>(&state_->terms), &postings_, &positions_}) {
				if (!h->verify(0, h->bytes.size())) { corrupt("section checksum"); }
			}
			verify_lengths();
		}

	private:
		const char* derived_norms() const {
			if (!state_->norms_ready.load(std::memory_order_acquire)) {
				std::lock_guard lock(state_->norms_mutex);
				if (!state_->norms_ready.load(std::memory_order_relaxed)) {
					std::string n(doc_count_, '\0');
					for (ordinal_t o = 0; o < doc_count_; ++o) { n[o] = char(format::encode_length(length(o))); }
					state_->owned_norms = std::move(n);
					state_->norms_ready.store(true, std::memory_order_release);
				}
			}
			return state_->owned_norms.data();
		}
		// Checks lengths whole on first use: its CRC, then its width against its size.
		void verify_lengths() const {
			auto& st = state_->lengths_state;
			if (st.load(std::memory_order_acquire) == 0) {
				const unsigned w = std::uint8_t(lengths_[0]);
				const bool good = ::kronuz::crc32c::crc32c(lengths_.data(), lengths_.size()) == lengths_crc_ && w <= 32 &&
					lengths_.size() == 1 + (doc_count_ + 127) / 128 * bitpack::packed_bytes(w);
				state_->length_width.store(std::uint8_t(w), std::memory_order_relaxed);
				st.store(good ? 1 : 2, std::memory_order_release);
			}
			if (st.load(std::memory_order_acquire) != 1) { corrupt("lengths checksum or size"); }
		}
		static bool verify_terms(void* context, std::uint64_t offset, std::uint64_t length) {
			return static_cast<state*>(context)->terms.verify(offset, length);
		}
		// A segment without the structure (F4): no terms, every length 0, norms 0, from
		// owned stand-ins, so every read path stays the one it is.
		void open_absent(const open_context& c) {
			// Empty handles that still verify through the segment's region state.
			state_->terms = c.section(format::section::terms);
			postings_ = c.section(format::section::postings);
			positions_ = c.section(format::section::positions);
			state_->pinned_index.assign(4, '\0');  // an empty directory: no own dictionary, no pool
			state_->owned_lengths.assign(1, '\0');  // width 0
			lengths_ = state_->owned_lengths;
			lengths_crc_ = ::kronuz::crc32c::crc32c(lengths_.data(), lengths_.size());
			dict_.emplace(state_->owned_blocks, state_->pinned_index);  // owned_blocks stays empty
		}
		// Heap-held, so the dictionary's verifier context survives moving the segment.
		struct state {
			section_handle terms;
			std::string pinned_index;  // the term index, copied out of the mapping
			std::string owned_blocks, owned_lengths, owned_norms;  // stand-ins for absent sections
			std::atomic<std::uint8_t> lengths_state{0};  // 0 unverified, 1 good, 2 bad
			std::atomic<std::uint8_t> length_width{0};   // lengths' width, set by verify_lengths
			std::mutex norms_mutex;                      // derives norms once, when not built
			std::atomic<bool> norms_ready{false};
		};
		std::unique_ptr<state> state_;
		section_handle postings_, positions_;
		std::string_view lengths_, norms_;
		const char* norms_at_ = nullptr;  // the norms section's bytes; null until derived
		std::optional<fielddict::reader> dict_;
		std::uint64_t doc_count_ = 0, total_length_ = 0;
		std::uint32_t max_positions_ = 0, min_length_ = 0, max_length_ = 0, lengths_crc_ = 0;
		std::uint8_t postings_shift_ = 0;
	};
	// L6: rebuilds the plugin's sections for a merge's output (merge.hh).
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink);
	// The length statistics the core's manifest records (L9): the segment's total, and a
	// document's.
	static std::uint64_t total_length(const reader& r) { return r.total_length(); }
	static std::uint64_t length(const reader& r, ordinal_t o) { return r.length(o); }
	// What a check counts (check.hh).
	struct report {
		std::uint64_t terms = 0, postings = 0, positions = 0;
	};
	// Decodes every term's postings (exactly its document frequency, by the iterator's
	// construction) and positions, and reads every document's length.
	// The core still passes the index's key prefix (F3 removes it); terms are never checked against it.
	static void check(const reader& inverted, const core_segment& core, report& r, std::string_view);
	// Its digest records (L8, F21): per document a length record, length 0 included, and per
	// posting a P record; over a segment's live ordinals, or a buffered document.
	template <class Live, class Emit>
	static void records(const reader& r, const core_segment& core, Live&& live, Emit&& emit);
	template <class Emit>
	static void records(std::uint64_t docid, const part& p, Emit&& emit);
	// A document's length record and a posting's record.
	template <class Emit>
	static void length_record(std::uint64_t docid, std::uint32_t length, Emit& emit) {
		digest_record rec('N');
		rec.u64(docid);
		rec.u32(length);
		emit(rec.data());
	}
	template <class Emit>
	static void posting_record(std::uint64_t docid, std::string_view term, std::uint32_t wdf, std::span<const std::uint32_t> positions, Emit& emit) {
		digest_record rec('P');
		rec.u64(docid);
		rec.bytes(term);
		rec.u32(wdf);
		rec.u32(std::uint32_t(positions.size()));
		for (auto q : positions) { rec.u32(q); }
		emit(rec.data());
	}

private:
	// One field part's checks (see validate).
	static bool valid_field(const field_part& f) {
		thread_local std::vector<std::string_view> terms;  // sorted, to find repeats
		terms.clear();
		std::array<std::uint64_t, 4> firsts{};  // first bytes of the terms a token could equal
		for (const auto& t : f.terms) {
			if (t.term.size() > format::max_term_bytes) { return false; }
			for (std::size_t i = 1; i < t.positions.size(); ++i) {
				if (t.positions[i] <= t.positions[i - 1]) { return false; }
			}
			terms.push_back(t.term);
			if (!t.term.empty()) { firsts[std::uint8_t(t.term[0]) >> 6] |= std::uint64_t(1) << (std::uint8_t(t.term[0]) & 63); }
		}
		if (terms.size() > 1) {
			std::sort(terms.begin(), terms.end());
			if (std::adjacent_find(terms.begin(), terms.end()) != terms.end()) { return false; }
		}
		const bool any_firsts = (firsts[0] | firsts[1] | firsts[2] | firsts[3]) != 0;
		const std::string_view text = f.text;
		if (f.split_text) { return valid_words(f, terms, firsts, any_firsts); }
		const std::uint64_t n = text.size();
		// Bounds, sizes, and order folded into one flag per token, no branch.
		bool ok = true;
		std::uint64_t prev = 0;
		for (std::size_t i = 0; i < f.tokens.size(); ++i) {
			const text_token& tk = f.tokens[i];
			ok &= tk.size != 0 && std::uint64_t(tk.offset) + tk.size <= n && (i == 0 || tk.position > prev);
			prev = tk.position;
		}
		if (!ok) { return false; }
		if (!any_firsts) { return true; }
		for (const text_token& tk : f.tokens) {
			const std::uint8_t first = std::uint8_t(text[tk.offset]);
			if ((firsts[first >> 6] >> (first & 63) & 1) && std::binary_search(terms.begin(), terms.end(), text.substr(tk.offset, tk.size))) { return false; }
		}
		return true;
	}
	// valid_field's token checks for a text split at spaces: no tokens and no length given, and
	// no word equals one of the part's terms.
	static bool valid_words(const field_part& f, const std::vector<std::string_view>& terms, const std::array<std::uint64_t, 4>& firsts, bool any_firsts) {
		const std::string_view text = f.text;
		if (!f.tokens.empty() || f.length != 0 || !text_detail::splittable(text, format::max_term_bytes)) { return false; }
		if (!any_firsts) { return true; }
		bool ok = true;
		text_detail::split_words(text, [&](std::size_t offset, std::size_t size) {
			const std::uint8_t first = std::uint8_t(text[offset]);
			if (ok && (firsts[first >> 6] >> (first & 63) & 1) && std::binary_search(terms.begin(), terms.end(), text.substr(offset, size))) { ok = false; }
		});
		return ok;
	}
};

using document = basic_document<search_layout>;
namespace detail {
struct document_fields {  // the document's fields, with the inverter's text inputs
	std::string key;
	std::vector<inverted_plugin::field_part> fields;
	std::optional<std::uint32_t> length;
	std::string stored;
	std::vector<slot_value> values;
};
}  // namespace detail
// doc_bytes reads it at more than one flush thread, so those flush points depend on it; at
// one, the charge is the tuple buffer's (R30).
static_assert(sizeof(document) == sizeof(detail::document_fields), "doc_bytes at more than one flush thread depends on it");

// Readers hold views of the file and pointers into heap-held state, so a segment moves
// without leaving one dangling (layers.md L3).
static_assert(std::is_nothrow_move_constructible_v<segment>);


// fill's fast paths (step-1 ranges, fused unpack, summed-mask scatter) are on; a build defining
// PROSPECT_FILL_SLOW turns them off so tests can check the fallback agrees with them.
#ifdef PROSPECT_FILL_SLOW
inline constexpr bool fill_fast = false;
#else
inline constexpr bool fill_fast = true;
#endif

// 1 << i, a load instead of a variable shift.
inline constexpr auto bit_masks = [] {
	std::array<std::uint64_t, 64> t{};
	for (unsigned i = 0; i < 64; ++i) { t[i] = std::uint64_t(1) << i; }
	return t;
}();

// Sanitizer builds allocate every block fresh, so ASan and MSan see a use after free.
#if defined(__SANITIZE_ADDRESS__)
inline constexpr bool recycle_blocks = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
inline constexpr bool recycle_blocks = false;
#else
inline constexpr bool recycle_blocks = true;
#endif
#else
inline constexpr bool recycle_blocks = true;
#endif

// Base for the large objects a query builds and drops (iterators, readers, scorers): their
// blocks are kept on a per-thread stack and reused, which skips the allocator's large-block path.
// A block kept after the thread's stack is destroyed is never freed (CR3); no such delete exists.
template <class T>
struct recycled {
	static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= 16, "blocks come from the default allocation function");
	static void* operator new(std::size_t n) {
		if constexpr (!recycle_blocks) { return ::operator new(n); }
		auto& s = cache();
		if (n == sizeof(T) && s.count) { return s.blocks[--s.count]; }
		return ::operator new(n);
	}
	static void operator delete(void* p, std::size_t n) noexcept {
		if constexpr (!recycle_blocks) { return ::operator delete(p); }
		auto& s = cache();
		if (n == sizeof(T) && s.count < keep) {
			s.blocks[s.count++] = p;
			return;
		}
		::operator delete(p);
	}

private:
	static constexpr std::size_t keep = 8;
	struct stack {
		void* blocks[keep];
		std::size_t count = 0;
		~stack() {
			while (count) { ::operator delete(blocks[--count]); }
		}
	};
	static stack& cache() {
		static thread_local stack s;
		return s;
	}
};

// Iterates one term's postings: ordinals in increasing order with their wdf. Satisfies
// docset::doc_iterator; scorers build on it. Skip data is read in place (F10): locating a
// block reads its group's advance entries without checking them, and a block's entry is
// checked when the block is decoded.
class postings_iterator : public recycled<postings_iterator> {
public:
	postings_iterator(const inverted_plugin::reader& seg, const format::term_info& info) : seg_(&seg), info_(info), df_(info.df) {
		if (info.flags & format::single_posting) {
			if (info.postings >= seg.doc_count()) { corrupt("single posting ordinal"); }
			buf_ords_[0] = ordinal_t(info.postings);
			buf_wdfs_[0] = info.single_wdf;
			buf_len_ = 1;
			blocks_ = 0;
			return;
		}
		auto bytes = seg.postings_bytes();
		if (info.postings > bytes.size() || info.postings_length > bytes.size() - info.postings) { corrupt("postings offset"); }
		p_ = bytes.data() + info.postings;
		end_ = p_ + info.postings_length;  // every read stays inside the term's range
		// Bytes are verified as they are first read (P15): the header here; level 1 and the
		// pairs below; each group's advance entries when located, its record's rest when its
		// pairs or positions are read, each block and the tail when decoded.
		need(p_, std::min<std::uint64_t>(128, info.postings_length));
		blocks_ = df_ / format::block;
		with_positions_ = !(info.flags & format::no_positions);
		with_counts_ = with_positions_ && !(info.flags & format::counts_are_wdf);
		if (blocks_) {
			const std::uint64_t groups = format::get_leb(&p_, end_);
			if (groups != (blocks_ + format::skip::group_blocks - 1) / format::skip::group_blocks) { corrupt("skip group count"); }
			if (p_ == end_) { corrupt("skip flags"); }
			const std::uint8_t flags = std::uint8_t(*p_++);
			codec_width_ = flags >> format::skip::codec_shift & 3;
			if ((flags & ~format::skip::known) || codec_width_ > unsigned(std::bit_width(unsigned(format::max_block_codec)))) { corrupt("skip flags"); }
			offset_width_ = flags & format::skip::wide ? 8 : 4;
			if (with_positions_) {
				tail_first_position_ = format::get_leb(&p_, end_);
				tail_pos_offset_ = format::get_leb(&p_, end_);
			}
			if (!(info.flags & format::no_wdf)) {  // threshold seeds (F13)
				for (; seeds_n_ < 3 && format::seed_ks[seeds_n_] <= df_; ++seeds_n_) {
					auto& s = seeds_[seeds_n_];
					s.code_wdf = leb32();
					if (p_ == end_) { corrupt("threshold seeds"); }
					s.code = std::uint8_t(*p_++);
					s.exact_wdf = leb32();
					s.exact = leb32();
				}
			}
			// Fixed-width level 1, then the groups' pairs, records, blocks, and tail; each
			// region is checked against the term's range here, each group when located.
			const std::uint64_t pairs_bytes = format::get_leb(&p_, end_), records_bytes = format::get_leb(&p_, end_),
								blocks_bytes = format::get_leb(&p_, end_);
			entry_size_ = 8 + offset_width_ * (with_positions_ ? 5 : 2);
			groups_n_ = groups;
			std::uint64_t range = std::uint64_t(end_ - p_);
			if (groups > range / entry_size_) { corrupt("skip groups bounds"); }
			level1_ = p_;
			p_ += groups * entry_size_;
			range = std::uint64_t(end_ - p_);
			if (pairs_bytes > range || records_bytes > range - pairs_bytes || blocks_bytes > range - pairs_bytes - records_bytes) {
				corrupt("skip data bounds");
			}
			need(level1_, std::uint64_t(p_ - level1_) + pairs_bytes);
			pairs_ = std::string_view(p_, pairs_bytes);
			records_ = p_ + pairs_bytes;
			records_bytes_ = records_bytes;
			blocks_begin_ = records_ + records_bytes;
			tail_ = blocks_begin_ + blocks_bytes;
		} else {
			tail_ = p_;
		}
	}

	ordinal_t doc() const noexcept { return doc_; }
	std::uint32_t wdf() const { return wdfs()[pos_]; }
	// The stored seed pairs (F13) for the smallest stored k at or above k, or null.
	const format::seed_pairs* seeds(std::size_t k) const noexcept {
		for (std::size_t j = 0; j < seeds_n_; ++j) {
			if (k <= format::seed_ks[j]) { return &seeds_[j]; }
		}
		return nullptr;
	}
	std::size_t seed_count() const noexcept { return seeds_n_; }
	const format::seed_pairs& seed_at(std::size_t j) const noexcept { return seeds_[j]; }
	std::uint64_t cost() const noexcept { return df_; }

	ordinal_t next() {
		if (doc_ == no_more_docs) { return doc_; }
		if (started_ && ++pos_ < buf_len_) { return doc_ = ords()[pos_]; }
		started_ = true;
		return load_next_block();
	}

	ordinal_t advance(ordinal_t target) {
		if (started_ && doc_ != no_more_docs && doc_ >= target) { return doc_; }
		// Inside the decoded block: search it without touching skip data.
		if (started_ && buf_len_ && target <= buf_last()) {
			if (bits_pending_) { return advance_bits(target); }
			pos_ += count_below(buf_ords_ + pos_, buf_len_ - pos_, target);
			return doc_ = buf_ords_[pos_];
		}
		// Skip whole blocks using skip data before decoding.
		if (blocks_) { skip_to_block_containing(target); }
		while (true) {
			if (!started_ || pos_ + 1 >= buf_len_ || target > buf_last()) {
				started_ = true;
				if (load_next_block() == no_more_docs) { return doc_; }
			} else {
				doc_ = ords()[++pos_];
			}
			// Search inside the block.
			if (doc_ < target) {
				if (target > buf_last()) { continue; }
				if (bits_pending_) { return advance_bits(target); }
				pos_ += count_below(buf_ords_ + pos_, buf_len_ - pos_, target);
				return doc_ = buf_ords_[pos_];
			}
			return doc_;
		}
	}

	// Skip data, read without decoding postings. Blocks are numbered 0..blocks(); block
	// blocks() is the tail, whose pairs are the term-level ones. Groups hold 32 blocks.

	// The block holding target: the first whose last ordinal is >= target (blocks() for
	// the tail or past the end). The search compares only entries it reads, so the block
	// found ends at or after target and the one before it ends before target.
	std::size_t block_index_for(ordinal_t target) {
		if (!blocks_) { return 0; }
		const std::size_t g = group_of(target);
		if (g == groups_n_) { return blocks_; }
		const group& gr = group_at(g);
		const std::uint64_t rel = std::uint64_t(target) - gr.base;  // target > base: group_of's neighbor
		std::size_t lo = 0, hi = gr.n - 1;                         // the last block ends at the group's last
		while (lo < hi) {
			const std::size_t mid = lo + (hi - lo) / 2;
			if (stored_last(gr, mid) < rel) { lo = mid + 1; } else { hi = mid; }
		}
		return g * format::skip::group_blocks + lo;
	}
	// The last ordinal of block b (no_more_docs for an empty tail).
	ordinal_t block_last(std::size_t b) {
		if (b >= blocks_) { return df_ % format::block ? tail_last() : no_more_docs; }
		const group& gr = group_at(b / format::skip::group_blocks);
		return ordinal_t(std::min<std::uint64_t>(entry_last(gr, b % format::skip::group_blocks), gr.last));
	}
	// Competitive pairs; the span is valid until the next impacts call.
	// Whether any of block b's pairs satisfies pred, decoding them only until one does.
	template <class Pred>
	bool any_block_impact(std::size_t b, Pred&& pred) {
		if (b >= blocks_) { return std::any_of(info_.impacts.begin(), info_.impacts.end(), pred); }
		return format::any_sized_impact(block_pairs(b), pred);
	}

	std::span<const format::impact> block_impacts_at(std::size_t b) {
		if (b >= blocks_) { return info_.impacts; }
		impacts_.clear();
		format::any_sized_impact(block_pairs(b), [&](const format::impact& i) {
			impacts_.push_back(i);
			return false;
		});
		return impacts_;
	}
	std::size_t groups() const noexcept { return groups_n_; }
	// The group holding target, without decoding its blocks (groups() past the end). The
	// located group and the one after it are tried before a binary search.
	std::size_t group_of(ordinal_t target) const noexcept {
		const std::size_t n = groups_n_;
		std::size_t g = cur_.g;
		if (g < n) {
			if (target <= group_last(g) && (g == 0 || target > group_last(g - 1))) { return g; }
			if (g + 1 < n && target > group_last(g) && target <= group_last(g + 1)) { return g + 1; }
		}
		std::size_t lo = 0, hi = n;  // the first group whose last is >= target
		while (lo < hi) {
			const std::size_t mid = lo + (hi - lo) / 2;
			if (group_last(mid) < target) { lo = mid + 1; } else { hi = mid; }
		}
		return lo;
	}
	// The first ordinal a group can hold.
	ordinal_t group_first(std::size_t g) const noexcept { return g ? group_last(g - 1) + 1 : 0; }
	ordinal_t group_last(std::size_t g) const noexcept { return format::get<std::uint32_t>(level1_ + g * entry_size_); }
	std::span<const format::impact> group_impacts(std::size_t g) {
		const std::uint64_t at = format::get<std::uint32_t>(level1_ + g * entry_size_ + 4);
		const std::uint64_t end = g + 1 < groups_n_ ? format::get<std::uint32_t>(level1_ + (g + 1) * entry_size_ + 4) : pairs_.size();
		if (at >= end || end > pairs_.size()) { corrupt("group pairs bounds"); }
		return decode(pairs_.substr(at, end - at));
	}

	// The last ordinal of the block that holds target (no_more_docs past the end), and that
	// block's competitive pairs.
	ordinal_t block_end(ordinal_t target) {
		const std::size_t b = block_index_for(target);
		const ordinal_t last = block_last(b);
		return last != no_more_docs && target <= last ? last : no_more_docs;
	}
	std::span<const format::impact> block_impacts(ordinal_t target) { return block_impacts_at(block_index_for(target)); }

	const format::term_info& info() const noexcept { return info_; }

	// Postings block holding the current document (blocks_ for the tail) and its index.
	std::size_t current_block() const noexcept { return in_tail() ? blocks_ : next_block_ - 1; }
	std::size_t index_in_block() const noexcept { return pos_; }
	bool in_tail() const noexcept { return tail_done_; }
	std::size_t blocks() const noexcept { return blocks_; }

	// Where the current postings block's positions start: the index of its first position
	// in the term's stream and the byte offset of the position block holding it; and the
	// byte offset of its count block (counts differ from wdf). Zero for terms without skip
	// data, whose positions start at the beginning.
	struct position_start {
		std::uint64_t first_index = 0, block_offset = 0, count_offset = 0;
	};
	position_start positions_start() {
		if (in_tail()) { return {tail_first_position_, tail_pos_offset_, 0}; }
		const std::size_t b = next_block_ - 1;
		load_positional(b / format::skip::group_blocks);
		return positional_[b % format::skip::group_blocks];
	}

	// Index of the current document's first position in the term's position stream, given
	// the per-document counts of the current block (its wdf when counts equal wdf). Sums
	// forward from the last document asked about in the same block.
	std::uint64_t position_index(const std::uint32_t* counts) {
		const std::size_t block = current_block();
		if (sum_block_ != block || sum_pos_ > pos_) {
			sum_block_ = block;
			sum_pos_ = 0;
			sum_ = positions_start().first_index;
		}
		std::uint64_t add = 0;  // at most 128 counts below 2^32 each: no overflow, and it vectorizes
		for (std::size_t i = sum_pos_; i < pos_; ++i) { add += counts[i]; }
		if (sum_ + add < sum_) { corrupt("position index"); }  // wraps past 2^64
		sum_ += add;
		sum_pos_ = pos_;
		return sum_;
	}

	// The decoded block's wdfs, decoded on first use (counting never needs them).
	const std::uint32_t* wdfs() const {
		if (wdf_pending_) {
			if (!bitpack::decode(reinterpret_cast<const std::uint8_t*>(wdf_pending_), wdf_size_, buf_wdfs_)) { corrupt("wdf block"); }
			wdf_pending_ = nullptr;
		}
		return buf_wdfs_;
	}

	// Calls f(d, wdf) for every posting d in [doc(), end), then stops at the first posting
	// >= end.
	template <class F>
	void for_each_until(ordinal_t end, F&& f) {
		if (!started_ && next() == no_more_docs) { return; }
		while (doc_ < end) {
			std::size_t i = pos_;
			const ordinal_t* o = ords();
			const std::uint32_t* w = wdfs();
			for (; i < buf_len_ && o[i] < end; ++i) { f(o[i], w[i]); }
			if (i < buf_len_) {
				pos_ = i;
				doc_ = buf_ords_[i];
				return;
			}
			pos_ = buf_len_ - 1;
			if (load_next_block() == no_more_docs) { return; }
		}
	}

	// Sets bit (d - base) for every posting d in [doc(), end), then stops at the first
	// posting >= end (see scorer::fill).
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) {
		assert(end - base <= 4096 && base <= end);
		if (!started_ && next() == no_more_docs) { return; }
		assert(base <= doc_);
		while (doc_ < end) {
			std::size_t i = pos_;
			ords();
			const std::size_t stop = buf_ords_[buf_len_ - 1] < end ? buf_len_ : std::size_t(std::lower_bound(buf_ords_ + pos_, buf_ords_ + buf_len_, end) - buf_ords_);
			if (i < stop) {
				std::uint32_t o[format::block];
				for (std::size_t k = i; k < stop; ++k) { o[k - i] = buf_ords_[k] - base; }
				scatter(o, stop - i, bits);
				i = stop;
			}
			if (i < buf_len_) {
				pos_ = i;
				doc_ = buf_ords_[i];
				return;
			}
			// Whole blocks inside the window go straight into its bits, without decoding to ordinals.
			fill_blocks(base, end, bits);
			pos_ = buf_len_ - 1;
			if (load_next_block() == no_more_docs) { return; }
		}
	}

	// ORs bitset block b into a window starting at base; the last word is masked to the
	// block's span, and the block must hold exactly 128 documents.
	void or_bitset(std::size_t b, ordinal_t base, std::uint64_t* bits) {
		const block_ref r = block_at(b);
		if (r.codec != format::block_codec::bitset) { corrupt("bitset block codec"); }
		or_bitset(r, base, bits);
	}

private:
	std::span<const format::impact> decode(std::string_view list) {
		impacts_.clear();
		const char* p = list.data();
		format::decode_impacts(&p, list.data() + list.size(), impacts_);
		if (p != list.data() + list.size()) { corrupt("impacts length"); }
		return impacts_;
	}

	std::uint32_t leb32() {
		const std::uint64_t v = format::get_leb(&p_, end_);
		if (v > 0xffffffffu) { corrupt("threshold seeds"); }
		return std::uint32_t(v);
	}

	// A group located through level 1: its fields checked against its neighbors and the
	// regions, its advance entries verified, their widths derived (format.hh, "skip data").
	struct group {
		std::size_t g = std::numeric_limits<std::size_t>::max();
		ordinal_t last = 0, base = 0;  // base: the previous group's last, 0 for the first
		std::size_t n = 0;             // blocks in the group
		unsigned wl = 0, wo = 0;
		const char* adv = nullptr;  // advance entries, then pairs and positional deltas to end
		const char* end = nullptr;
		std::uint64_t adv_bytes = 0;
		std::uint64_t blocks_begin = 0, blocks_end = 0;  // from blocks_begin_
		std::uint64_t first_position = 0, pos_offset = 0, count_offset = 0;
	};
	// A block's checked entry: base is the previous block's last (max before the term's
	// first block, so base + 1 is its first possible ordinal).
	struct block_ref {
		ordinal_t last = 0, base = 0;
		std::uint64_t offset = 0, bytes = 0;  // from blocks_begin_
		format::block_codec codec = format::block_codec::deltas;
	};

	std::uint64_t offset_at(const char* e, std::size_t i) const noexcept {
		const char* at = e + 8 + offset_width_ * i;
		return offset_width_ == 8 ? format::get<std::uint64_t>(at) : format::get<std::uint32_t>(at);
	}
	const group& group_at(std::size_t g) {
		if (cur_.g != g) { load_group(g); }
		return cur_;
	}
	void load_group(std::size_t g) {
		const char* e = level1_ + g * entry_size_;
		group gr;
		gr.last = format::get<std::uint32_t>(e);
		gr.base = g ? group_last(g - 1) : 0;
		gr.n = std::min<std::size_t>(format::skip::group_blocks, blocks_ - g * format::skip::group_blocks);
		const std::uint64_t record_begin = offset_at(e, 0), record_end = g + 1 < groups_n_ ? offset_at(e + entry_size_, 0) : records_bytes_;
		gr.blocks_begin = offset_at(e, 1);
		gr.blocks_end = g + 1 < groups_n_ ? offset_at(e + entry_size_, 1) : std::uint64_t(tail_ - blocks_begin_);
		if ((g && gr.last <= gr.base) || gr.last >= seg_->doc_count() || record_begin > record_end || record_end > records_bytes_ ||
			gr.blocks_begin > gr.blocks_end || gr.blocks_end > std::uint64_t(tail_ - blocks_begin_) || gr.blocks_end - gr.blocks_begin < gr.n) {
			corrupt("skip group");
		}
		gr.wl = unsigned(std::bit_width(gr.last - gr.base));
		gr.wo = unsigned(std::bit_width(gr.blocks_end - gr.blocks_begin - 1));
		gr.adv_bytes = format::advance_bytes(gr.n, gr.wl, gr.wo, codec_width_);
		if (gr.adv_bytes > record_end - record_begin) { corrupt("advance entries bounds"); }
		gr.adv = records_ + record_begin;
		gr.end = records_ + record_end;
		need(gr.adv, gr.adv_bytes);
		if (with_positions_) {
			gr.first_position = offset_at(e, 2);
			gr.pos_offset = offset_at(e, 3);
			gr.count_offset = offset_at(e, 4);
		}
		gr.g = g;
		cur_ = gr;
	}
	// Entry i's stored last (relative to the group's base; i < n - 1), offset (0 < i), and code.
	std::uint64_t stored_last(const group& gr, std::size_t i) const noexcept { return format::get_bits(gr.adv, gr.adv_bytes, i * gr.wl, gr.wl); }
	std::uint64_t entry_last(const group& gr, std::size_t i) const noexcept {
		return i + 1 < gr.n ? std::uint64_t(gr.base) + stored_last(gr, i) : std::uint64_t(gr.last);
	}
	std::uint64_t entry_offset(const group& gr, std::size_t i) const noexcept {
		return i ? format::get_bits(gr.adv, gr.adv_bytes, (gr.n - 1) * gr.wl + (i - 1) * gr.wo, gr.wo) : 0;
	}
	unsigned entry_codec(const group& gr, std::size_t i) const noexcept {
		return codec_width_ ? format::get_bits(gr.adv, gr.adv_bytes, (gr.n - 1) * (gr.wl + gr.wo) + i * codec_width_, codec_width_) : 0;
	}
	// Block b's entry, checked (F10): its last above the previous block's and at most its
	// group's, its bytes inside the group's.
	// The last block checked is kept: a walk reads its successor's base and start from it.
	block_ref block_at(std::size_t b) {
		if (b == checked_b_) { return checked_; }
		const group& gr = group_at(b / format::skip::group_blocks);
		const std::size_t i = b % format::skip::group_blocks;
		const bool next = i && checked_b_ + 1 == b;
		const std::uint64_t last = entry_last(gr, i), base = next ? checked_.last : i ? entry_last(gr, i - 1) : gr.base;
		const std::uint64_t group_bytes = gr.blocks_end - gr.blocks_begin, start = next ? checked_.offset + checked_.bytes - gr.blocks_begin : entry_offset(gr, i),
							end = i + 1 < gr.n ? entry_offset(gr, i + 1) : group_bytes;
		const unsigned code = entry_codec(gr, i);
		// The codec width the skip flags allow keeps every code a kept one.
		if (last > gr.last || (b && last <= base) || start >= end || end > group_bytes) { corrupt("advance entry"); }
		checked_ = {ordinal_t(last), b ? ordinal_t(base) : std::numeric_limits<ordinal_t>::max(), gr.blocks_begin + start, end - start, format::block_codec(code)};
		checked_b_ = b;
		return checked_;
	}

	// Block b's pairs: the group's lists are walked once, then kept as views.
	std::string_view block_pairs(std::size_t b) {
		load_pairs(b / format::skip::group_blocks);
		return block_pairs_[b % format::skip::group_blocks];
	}
	void load_pairs(std::size_t g) {
		if (pairs_g_ == g) { return; }
		const group& gr = group_at(g);
		const char* p = gr.adv + gr.adv_bytes;
		need(p, std::uint64_t(gr.end - p));
		for (std::size_t i = 0; i < gr.n; ++i) { block_pairs_[i] = format::skip_sized_impacts(&p, gr.end); }
		if (!with_positions_ && p != gr.end) { corrupt("skip record bytes"); }
		positional_at_ = p;
		pairs_g_ = g;
	}
	// Each block's positional fields: level 1's for the group's first, deltas for the rest.
	void load_positional(std::size_t g) {
		if (positional_g_ == g) { return; }
		load_pairs(g);
		const group& gr = group_at(g);
		const char* p = positional_at_;
		position_start at{gr.first_position, gr.pos_offset, gr.count_offset};
		positional_[0] = at;
		for (std::size_t i = 1; i < gr.n; ++i) {
			const std::uint64_t delta = format::get_leb(&p, gr.end);
			if (at.first_index + delta < at.first_index) { corrupt("position index"); }
			at.first_index += delta;
			at.block_offset += format::get_leb(&p, gr.end);
			if (with_counts_) { at.count_offset += format::get_leb(&p, gr.end); }
			positional_[i] = at;
		}
		if (p != gr.end) { corrupt("skip record bytes"); }
		positional_g_ = g;
	}

	ordinal_t tail_last() {
		if (info_.flags & format::single_posting) { return ordinal_t(info_.postings); }
		if (tail_last_ == no_more_docs) {
			// Decode lazily the last ordinal of the list: the tail's sum.
			if (df_ % format::block == 0) {
				tail_last_ = blocks_ ? group_last(groups_n_ - 1) : no_more_docs;
			} else {
				need(tail_, std::uint64_t(end_ - tail_));
				const char* p = tail_;
				ordinal_t prev = blocks_ ? group_last(groups_n_ - 1) : std::numeric_limits<ordinal_t>::max();
				for (std::size_t i = 0; i < df_ % format::block; ++i) { prev += ordinal_t(format::get_leb(&p, end_)); }
				tail_last_ = prev;
			}
		}
		return tail_last_;
	}

	void skip_to_block_containing(ordinal_t target) {
		// The next block holds it (targets only move forward past loaded blocks): no search.
		if (next_block_ < blocks_ && target <= block_last(next_block_)) { return; }
		std::size_t b = block_index_for(target);
		if (b > next_block_ && b <= blocks_) {
			next_block_ = b;
			buf_len_ = 0;
			pos_ = 0;
			bits_pending_ = false;
		}
	}

	ordinal_t load_next_block() {
		pos_ = 0;
		bits_pending_ = false;
		if (next_block_ < blocks_) {
			const block_ref cur = block_at(next_block_);
			const char* p = blocks_begin_ + cur.offset;  // block_at checked offset + bytes against the group's
			need(p, cur.bytes);
			std::size_t used;
			if (cur.codec == format::block_codec::bitset) {
				used = load_bitset(p, cur);
			} else {
				// Gaps - 1 (M23): ordinals increase within (base, last] by construction.
				used = format::decode_gaps(reinterpret_cast<const std::uint8_t*>(p), cur.bytes, cur.base, cur.last, next_block_ == 0, buf_ords_);
				if (!used) { corrupt("ordinal block"); }
			}
			if (info_.flags & format::no_wdf) {
				std::fill(buf_wdfs_, buf_wdfs_ + format::block, 0u);
				wdf_pending_ = nullptr;
			} else {
				if (used >= cur.bytes) { corrupt("wdf block"); }
				wdf_pending_ = p + used;
				wdf_size_ = cur.bytes - used;
			}
			buf_len_ = format::block;
			++next_block_;
			return doc_ = bits_pending_ ? bits_doc0_ : buf_ords_[0];
		}
		if (next_block_ == blocks_ && !tail_done_ && df_ % format::block) {
			if (info_.flags & format::single_posting) {
				tail_done_ = true;
				buf_len_ = 1;
				return doc_ = buf_ords_[0];
			}
			need(tail_, std::uint64_t(end_ - tail_));
			const char* p = tail_;
			const std::size_t n = df_ % format::block;
			ordinal_t prev = blocks_ ? group_last(groups_n_ - 1) : std::numeric_limits<ordinal_t>::max();
			const ordinal_t base = prev;
			for (std::size_t i = 0; i < n; ++i) { buf_ords_[i] = prev += ordinal_t(format::get_leb(&p, end_)); }
			for (std::size_t i = 0; i < n; ++i) { buf_wdfs_[i] = (info_.flags & format::no_wdf) ? 0 : std::uint32_t(format::get_leb(&p, end_)); }
			wdf_pending_ = nullptr;
			buf_len_ = n;
			check_increasing(base);
			tail_done_ = true;
			return doc_ = buf_ords_[0];
		}
		buf_len_ = 0;
		return doc_ = no_more_docs;
	}

	// Keeps a bitset block's words, masked to its span, for advance_bits and ords; returns
	// its bytes. Exactly 128 bits must be set, the last at the block's last ordinal.
	std::size_t load_bitset(const char* p, const block_ref& r) {
		const ordinal_t first = r.base + 1;  // <= r.last: block_at checked last > base
		const std::uint64_t span = std::uint64_t(r.last) - first + 1, bytes = (span + 7) / 8;
		if (bytes > r.bytes || bytes > sizeof bits_) { corrupt("bitset block bounds"); }
		const std::size_t words = std::size_t((bytes + 7) / 8);
		std::size_t n = 0;
		for (std::size_t w = 0; w < words; ++w) {
			std::uint64_t word = 0;
			std::memcpy(&word, p + w * 8, std::min<std::uint64_t>(8, bytes - w * 8));
			if ((w + 1) * 64 > span && span % 64) { word &= (std::uint64_t(1) << (span % 64)) - 1; }
			bits_[w] = word;
			n += std::size_t(std::popcount(word));
		}
		if (n != format::block || !(bits_[words - 1] >> ((span - 1) % 64) & 1)) { corrupt("bitset block count"); }
		std::size_t w = 0;
		while (!bits_[w]) { ++w; }
		bits_first_ = first;
		bits_doc0_ = first + ordinal_t(w * 64 + std::size_t(std::countr_zero(bits_[w])));
		bits_last_ = r.last;
		bits_pending_ = true;
		return std::size_t(bytes);
	}

	// ORs a checked bitset block into a window starting at base.
	void or_bitset(const block_ref& r, ordinal_t base, std::uint64_t* bits) {
		const ordinal_t first = r.base + 1;
		if (first < base) { corrupt("bitset block span"); }
		const std::uint64_t span = std::uint64_t(r.last) - first + 1, bytes = (span + 7) / 8, words = (bytes + 7) / 8;
		const char* p = blocks_begin_ + r.offset;
		if (bytes > r.bytes) { corrupt("bitset block bounds"); }
		need(p, r.bytes);
		const std::uint64_t at = first - base, full = std::min<std::uint64_t>(words, r.bytes / 8);
		assert(at + span <= 4096);
		const unsigned shift = unsigned(at % 64);
		std::uint64_t* out = bits + at / 64;
		std::uint64_t carry = 0;  // the previous word's bits shifted past its word
		std::size_t n = 0;
		for (std::uint64_t w = 0; w < words; ++w) {
			std::uint64_t word = 0;
			if (w < full) {
				std::memcpy(&word, p + w * 8, 8);  // inside the block, whose wdfs follow the bitset
			} else {
				std::memcpy(&word, p + w * 8, std::size_t(bytes - w * 8));
			}
			if (w + 1 == words && span % 64) { word &= (std::uint64_t(1) << (span % 64)) - 1; }
			n += std::size_t(std::popcount(word));
			out[w] |= (word << shift) | carry;
			carry = shift ? word >> (64 - shift) : 0;
		}
		if (carry) { out[words] |= carry; }
		if (n != format::block) { corrupt("bitset block count"); }
	}

	// Blocks from next_block_ whose last ordinal is below end, straight into the window's bits:
	// a bitset block is OR'd in, a FOR block is unpacked and scattered without an ordinal array
	// or a per-posting read-modify-write. Entries are walked a group at a time from the first,
	// checked (F10), instead of locating each block anew. The block holding end is left to
	// load_next_block, with its entry already checked.
	// Bounds: every write lands in the window's 64 words because a block taken has last < end
	// (end <= base + 4,096) and first > its entry's base >= base (or_bitset and fill_for check
	// it), and its ordinals are verified to end at last and rise (decode_gaps; the fused unpack's
	// widths of at most 25 bits cannot wrap). Asserts state this and cost nothing in release.
	void fill_blocks(ordinal_t base, ordinal_t end, std::uint64_t* bits) {
		while (next_block_ > 0 && next_block_ < blocks_) {
			block_ref r = block_at(next_block_);
			if (r.last >= end) { return; }
			const group& gr = group_at(next_block_ / format::skip::group_blocks);
			const std::uint64_t group_bytes = gr.blocks_end - gr.blocks_begin;
			std::size_t i = next_block_ % format::skip::group_blocks;
			for (;;) {
				need(blocks_begin_ + r.offset, r.bytes);
				if (r.codec == format::block_codec::bitset) {
					or_bitset(r, base, bits);
				} else {
					fill_for(r, base, bits);
				}
				++next_block_;
				if (++i == gr.n) { break; }
				const std::uint64_t last = entry_last(gr, i), start = r.offset + r.bytes - gr.blocks_begin, stop = i + 1 < gr.n ? entry_offset(gr, i + 1) : group_bytes;
				if (last > gr.last || last <= r.last || start >= stop || stop > group_bytes) { corrupt("advance entry"); }
				const block_ref next{ordinal_t(last), r.last, gr.blocks_begin + start, stop - start, format::block_codec(entry_codec(gr, i))};
				checked_ = next;
				checked_b_ = next_block_;
				if (next.last >= end) { return; }
				r = next;
			}
		}
	}

	// ORs a checked FOR block lying wholly inside the window [base, base + 4096) into bits.
	void fill_for(const block_ref& r, ordinal_t base, std::uint64_t* bits) {
		assert(r.last > r.base);
		const std::uint8_t* in = reinterpret_cast<const std::uint8_t*>(blocks_begin_ + r.offset);
		if (std::uint64_t(r.base) + 1 < base) { corrupt("ordinal block span"); }
		const std::uint8_t head = in[0];
		const unsigned width = head & 0x3f;
		// A constant block of step 1 is a run of 128 consecutive ordinals: a range of bits.
		if (fill_fast && head == bitpack::constant_flag && r.bytes >= 5 && r.last - r.base == format::block && !std::memcmp(in + 1, "\0\0\0\0", 4)) {
			if (!(info_.flags & format::no_wdf) && r.bytes == 5) { corrupt("wdf block"); }
			set_range(r.base + 1 - base, r.last - base, bits);
			return;
		}
		std::uint32_t o[format::block];
		std::size_t used;
		if ((head & (bitpack::exceptions_flag | bitpack::constant_flag)) || width > 25 || !fill_fast || r.bytes < 1 + bitpack::packed_bytes(width)) {
			// Constant, exceptional, or wide blocks decode to ordinals first.
			used = format::decode_gaps(in, r.bytes, r.base, r.last, false, o);
			if (!used) { corrupt("ordinal block"); }
			for (std::size_t i = 0; i < format::block; ++i) { o[i] -= base; }
		} else {
			used = 1 + bitpack::packed_bytes(width);
			bitpack::unpack_delta(in + 1, width, r.base - base, o);
			for (std::size_t i = 0; i < format::block; ++i) { o[i] += std::uint32_t(i + 1); }
			if (o[format::block - 1] != r.last - base) { corrupt("ordinal block"); }
		}
		if (!(info_.flags & format::no_wdf) && used >= r.bytes) { corrupt("wdf block"); }
		scatter(o, format::block, bits);
	}

	// Sets bits lo..hi, both inside the window.
	static void set_range(std::uint32_t lo, std::uint32_t hi, std::uint64_t* bits) noexcept {
		assert(lo <= hi && hi < 4096);
		const std::uint32_t wl = lo >> 6, wh = hi >> 6;
		const std::uint64_t ml = ~std::uint64_t(0) << (lo & 63), mh = ~std::uint64_t(0) >> (63 - (hi & 63));
		if (wl == wh) {
			bits[wl] |= ml & mh;
			return;
		}
		bits[wl] |= ml;
		for (std::uint32_t w = wl + 1; w < wh; ++w) { bits[w] = ~std::uint64_t(0); }
		bits[wh] |= mh;
	}

	// Sets bit o[i] for i < n: the o[i] increase and the last is below 4,096. Postings close
	// together share words, and consecutive read-modify-writes of one word wait on each other, so
	// a dense run sums a mask per posting (32-bit words: the sum is positive and never wraps) into
	// the slot of its word. A word's total is the sum at its last posting less the sum before its
	// first; slots start at zero, so untouched words add nothing. A sparse run sets its bits
	// directly.
	static void scatter(const std::uint32_t* o, std::size_t n, std::uint64_t* bits) noexcept {
		assert(n && o[0] <= o[n - 1] && o[n - 1] < 4096);
		if (fill_fast && o[n - 1] - o[0] == n - 1) {  // consecutive
			set_range(o[0], o[n - 1], bits);
			return;
		}
		if (!fill_fast || n < 16 || o[n - 1] - o[0] >= 768) {
			for (std::size_t i = 0; i < n; ++i) { bits[o[i] >> 6] |= bit_masks[o[i] & 63]; }
			return;
		}
		// A span under 768 bits touches at most 25 words.
		std::uint64_t sums[25] = {};
		const std::uint32_t w0 = o[0] >> 5;
		std::uint64_t c = 0;
		for (std::size_t i = 0; i < n; ++i) {
			const std::uint32_t d = o[i];
			c += bit_masks[d & 31];
			sums[(d >> 5) - w0] = c;
		}
		std::uint64_t prev = 0;
		for (std::uint32_t k = 0, hi = (o[n - 1] >> 5) - w0; k <= hi; ++k) {
			const std::uint64_t x = sums[k], delta = (x - prev) & -std::uint64_t(x != 0);
			bits[(w0 + k) >> 1] |= delta << (32 * ((w0 + k) & 1));
			prev += delta;
		}
	}

	// The current block's ordinals, expanding a bitset block on first use.
	const ordinal_t* ords() {
		if (bits_pending_) {
			std::size_t n = 0;
			for (std::size_t w = 0; n < format::block; ++w) {
				for (std::uint64_t word = bits_[w]; word; word &= word - 1) { buf_ords_[n++] = bits_first_ + ordinal_t(w * 64 + std::size_t(std::countr_zero(word))); }
			}
			bits_pending_ = false;
		}
		return buf_ords_;
	}
	ordinal_t buf_last() const noexcept { return bits_pending_ ? bits_last_ : buf_ords_[buf_len_ - 1]; }

	// Advances inside an unexpanded bitset block to the first posting >= target, which
	// must be in (doc(), the block's last]: the next set bit, ranked by popcount.
	ordinal_t advance_bits(ordinal_t target) {
		const std::uint64_t at = target - bits_first_;
		std::size_t w = std::size_t(at / 64);
		std::uint64_t word = bits_[w] & (~std::uint64_t(0) << (at % 64));
		while (!word) { word = bits_[++w]; }
		const unsigned bit = unsigned(std::countr_zero(word));
		std::size_t rank = std::size_t(std::popcount(bits_[w] & ((std::uint64_t(1) << bit) - 1)));
		for (std::size_t i = 0; i < w; ++i) { rank += std::size_t(std::popcount(bits_[i])); }
		pos_ = rank;
		return doc_ = bits_first_ + ordinal_t(w * 64 + bit);
	}

	// How many of n sorted values are below target: a few linear probes (dense lists'
	// targets are usually close), then a branch-free binary search.
	static std::size_t count_below(const ordinal_t* v, std::size_t n, ordinal_t target) noexcept {
		std::size_t i = 0;
		for (const std::size_t probe = std::min<std::size_t>(n, 4); i < probe; ++i) {
			if (v[i] >= target) { return i; }
		}
		if (i == n) { return n; }
		const ordinal_t* base = v + i;
		for (std::size_t len = n - i; len > 1;) {
			const std::size_t half = len / 2;
			base += (base[half] < target) * half;
			len -= half;
		}
		return std::size_t(base - v) + (*base < target);
	}

	// The tail's ordinals strictly increase from base (none before the first block) and stay
	// below the document count; branch-free so it vectorizes.
	void check_increasing(ordinal_t base) const {
		const std::size_t n = buf_len_ ? buf_len_ : std::min<std::size_t>(format::block, df_);
		if (n == 0) { return; }
		std::uint32_t bad = base != std::numeric_limits<ordinal_t>::max() && buf_ords_[0] <= base;
		for (std::size_t i = 1; i < n; ++i) { bad |= buf_ords_[i] <= buf_ords_[i - 1]; }
		if (bad) { corrupt("ordinals not increasing"); }
		if (buf_ords_[n - 1] >= seg_->doc_count()) { corrupt("ordinal out of range"); }
	}

	// Verifies [p, p + n) of the postings section before it is read.
	// Reads stay inside the regions the last call verified, so most calls end at the compare.
	void need(const char* p, std::uint64_t n) const {
		const char* base = seg_->postings_bytes().data();
		const auto from = std::uint64_t(p - base), to = from + n;
		if (from >= verified_from_ && to <= verified_to_) { return; }
		seg_->verify_postings(from, n);
		const std::uint8_t shift = seg_->postings_region_shift();
		verified_from_ = from >> shift << shift;
		verified_to_ = std::min<std::uint64_t>(((to + (std::uint64_t(1) << shift) - 1) >> shift) << shift, seg_->postings_bytes().size());
	}

	const inverted_plugin::reader* seg_;
	format::term_info info_;
	std::uint32_t df_;
	mutable std::uint64_t verified_from_ = 0, verified_to_ = 0;  // postings offsets known good
	const char* p_ = nullptr;
	const char* end_ = nullptr;
	const char* records_ = nullptr;
	const char* blocks_begin_ = nullptr;
	const char* tail_ = nullptr;
	std::size_t blocks_ = 0, next_block_ = 0;
	const char* level1_ = nullptr;  // fixed-width group entries
	std::size_t groups_n_ = 0, entry_size_ = 0, offset_width_ = 4;
	unsigned codec_width_ = 0;  // bits of each block's codec code
	format::seed_pairs seeds_[3];
	std::size_t seeds_n_ = 0;
	std::string_view pairs_;    // the groups' competitive pairs
	std::uint64_t records_bytes_ = 0;
	group cur_;
	block_ref checked_;
	std::size_t checked_b_ = std::numeric_limits<std::size_t>::max();
	std::string_view block_pairs_[format::skip::group_blocks];
	const char* positional_at_ = nullptr;  // the pairs' group's positional deltas
	position_start positional_[format::skip::group_blocks];
	std::size_t pairs_g_ = std::numeric_limits<std::size_t>::max(), positional_g_ = std::numeric_limits<std::size_t>::max();
	std::vector<format::impact> impacts_;
	std::uint64_t tail_first_position_ = 0, tail_pos_offset_ = 0;
	std::size_t sum_block_ = std::numeric_limits<std::size_t>::max(), sum_pos_ = 0;
	std::uint64_t sum_ = 0;
	bool with_positions_ = false, with_counts_ = false;
	ordinal_t buf_ords_[format::block] = {};
	std::uint64_t bits_[64] = {};  // the current bitset block while bits_pending_
	ordinal_t bits_first_ = 0, bits_doc0_ = 0, bits_last_ = 0;
	bool bits_pending_ = false;  // buf_ords_ not yet expanded from bits_
	mutable std::uint32_t buf_wdfs_[format::block] = {};
	mutable const char* wdf_pending_ = nullptr;  // the current block's undecoded wdf block
	std::size_t wdf_size_ = 0;
	std::size_t buf_len_ = 0, pos_ = 0;
	ordinal_t doc_ = 0, tail_last_ = no_more_docs;
	bool started_ = false, tail_done_ = false;
};

// Reads one term's position data: per-document counts (when they differ from wdf) and the
// position stream, decoding one 128-position block at a time. Opening reads a fixed header;
// blocks are found from the postings skip data and stepped over by their headers.
inline std::vector<format::impact> inverted_plugin::reader::tail_impacts(const format::term_info& info) const {
	std::vector<format::impact> pairs;
	postings_iterator it(*this, info);
	for (ordinal_t d = it.next(); d != no_more_docs; d = it.next()) { pairs.push_back({it.wdf(), norm(d)}); }
	return format::competitive(std::move(pairs));
}

class positions_reader : public recycled<positions_reader> {
public:
	positions_reader(const inverted_plugin::reader& seg, const format::term_info& info) {
		if (info.flags & format::no_positions) {
			none_ = true;  // a term indexed without positions: every count is 0
			return;
		}
		auto bytes = seg.positions_bytes();
		if (info.positions > bytes.size() || info.positions_length > bytes.size() - info.positions) { corrupt("positions offset"); }
		seg_ = &seg;
		const char* p = bytes.data() + info.positions;
		end_ = p + info.positions_length;  // every read stays inside the term's range
		max_count_ = seg.max_positions();
		// Bytes are verified as they are first read (P15): the headers here, each count and
		// position block when decoded or stepped over, the tail on first use.
		if (!(info.flags & format::counts_are_wdf)) {
			need(p, std::min<std::uint64_t>(10, std::uint64_t(end_ - p)));
			const std::uint64_t n = format::get_leb(&p, end_);
			if (n > std::uint64_t(end_ - p)) { corrupt("count blocks bounds"); }
			count_blocks_ = p;
			count_blocks_bytes_ = n;
			p += n;
		}
		need(p, std::min<std::uint64_t>(format::block * 10 + 20, std::uint64_t(end_ - p)));  // count tail and both lengths
		if (!(info.flags & format::counts_are_wdf)) {
			for (std::size_t i = 0; i < info.df % format::block; ++i) { count_tail_.push_back(std::uint32_t(format::get_leb(&p, end_))); }
		}
		full_blocks_ = format::get_leb(&p, end_);
		blocks_bytes_ = format::get_leb(&p, end_);
		if (blocks_bytes_ > std::uint64_t(end_ - p) || full_blocks_ > blocks_bytes_) { corrupt("position blocks bounds"); }
		blocks_ = p;
		tail_ = p + blocks_bytes_;
	}

	// The number of positions of the iterator's current document, without decoding them.
	std::uint32_t count(postings_iterator& it) {
		if (none_) { return 0; }
		if (it.info().flags & format::counts_are_wdf) { return it.wdf(); }
		return counts(it.in_tail(), it.positions_start().count_offset)[it.index_in_block()];
	}

	// The positions of the iterator's current document, ascending.
	void read(postings_iterator& it, std::vector<std::uint32_t>& out) {
		const auto got = read(it);
		out.assign(got.begin(), got.end());
	}

	// The same, in a buffer the reader owns until its next read.
	std::span<const std::uint32_t> read(postings_iterator& it) {
		if (none_) { return {}; }
		const auto start = it.positions_start();
		const std::uint32_t* counts = it.info().flags & format::counts_are_wdf ? it.wdfs() : this->counts(it.in_tail(), start.count_offset);
		const std::uint64_t first = it.position_index(counts);
		const std::uint32_t count = counts[it.index_in_block()];
		// No document has more positions than the segment's maximum; full blocks hold 128
		// positions each and a tail position takes at least a byte.
		const std::uint64_t limit = full_blocks_ * format::block + std::uint64_t(end_ - tail_);
		if (count > max_count_ || first > limit || count > limit - first) { corrupt("position count"); }
		if (count > out_size_) {
			out_size_ = std::max<std::size_t>(count, 2 * out_size_);
			out_ = std::make_unique_for_overwrite<std::uint32_t[]>(out_size_);
		}
		std::uint32_t at = 0;
		std::uint64_t i = first;
		std::uint32_t* o = out_.get();
		// Spans of full blocks, then the tail.
		while (i < first + count && i / format::block < full_blocks_) {
			const std::uint64_t j = i / format::block;
			if (decoded_block_ != j) { decode_block(j, start); }
			const std::uint64_t stop = std::min<std::uint64_t>(first + count, (j + 1) * format::block);
			for (const std::uint32_t* v = block_buf_ + i % format::block; i < stop; ++i) { *o++ = at += *v++; }
		}
		for (; i < first + count; ++i) { *o++ = at += tail_value(i); }
		return {out_.get(), count};
	}

private:
	const std::uint32_t* counts(bool tail, std::uint64_t offset) {
		if (tail) { return count_tail_.data(); }
		if (decoded_counts_ != offset) {
			if (offset < count_blocks_bytes_) { need_block(count_blocks_ + offset, count_blocks_bytes_ - offset); }
			if (offset >= count_blocks_bytes_ ||
				!bitpack::decode(reinterpret_cast<const std::uint8_t*>(count_blocks_ + offset), count_blocks_bytes_ - offset, counts_buf_)) {
				corrupt("count block");
			}
			decoded_counts_ = offset;
		}
		return counts_buf_;
	}

	// Position i of the stream's tail (i at or past the full blocks).
	std::uint32_t tail_value(std::uint64_t i) {
		const std::uint64_t t = i - full_blocks_ * format::block;
		while (tail_values_.size() <= t) {
			if (tail_p_ == nullptr) {
				need(tail_, std::uint64_t(end_ - tail_));
				tail_p_ = tail_;
				tail_values_.reserve(std::min<std::uint64_t>(format::block, std::uint64_t(end_ - tail_)));  // fewer than a block's positions
			}
			tail_values_.push_back(std::uint32_t(format::get_leb(&tail_p_, end_)));
		}
		return tail_values_[t];
	}

	// Steps from the nearest known block at or before j (the decoded one, or the postings
	// block's first) over block headers, then decodes block j.
	void decode_block(std::uint64_t j, const postings_iterator::position_start& start) {
		std::uint64_t at = start.first_index / format::block, offset = start.block_offset;
		if (decoded_block_ < j && decoded_block_ >= at) {
			at = decoded_block_;
			offset = decoded_offset_;
		}
		for (; at < j; ++at) {  // j >= at: position indexes never wrap (position_index, decode_group)
			if (offset >= blocks_bytes_) { corrupt("position block bounds"); }
			need_block(blocks_ + offset, blocks_bytes_ - offset);
			const std::size_t n = bitpack::encoded_size(reinterpret_cast<const std::uint8_t*>(blocks_ + offset), blocks_bytes_ - offset);
			if (!n) { corrupt("position block"); }
			offset += n;
		}
		if (offset < blocks_bytes_) { need_block(blocks_ + offset, blocks_bytes_ - offset); }
		if (offset >= blocks_bytes_ ||
			!bitpack::decode(reinterpret_cast<const std::uint8_t*>(blocks_ + offset), blocks_bytes_ - offset, block_buf_)) {
			corrupt("position block");
		}
		decoded_block_ = j;
		decoded_offset_ = offset;
	}

	// Verifies [p, p + n) of the positions section before it is read.
	void need(const char* p, std::uint64_t n) const { seg_->verify_positions(std::uint64_t(p - seg_->positions_bytes().data()), n); }
	// Verifies what reading one bitpack block at p can touch: at most a block's largest
	// encoding (header, 32-bit values, 128 exceptions with 32 high bits).
	void need_block(const char* p, std::uint64_t avail) const {
		constexpr std::uint64_t largest = 2 + 4 * format::block + format::block + 1 + 4 * format::block;
		need(p, std::min(largest, avail));
	}

	const inverted_plugin::reader* seg_ = nullptr;
	const char* end_ = nullptr;
	const char* count_blocks_ = nullptr;
	const char* blocks_ = nullptr;
	const char* tail_ = nullptr;
	const char* tail_p_ = nullptr;
	std::uint64_t count_blocks_bytes_ = 0, full_blocks_ = 0, blocks_bytes_ = 0;
	std::uint32_t max_count_ = 0;
	bool none_ = false;
	std::vector<std::uint32_t> count_tail_, tail_values_;
	std::unique_ptr<std::uint32_t[]> out_;
	std::size_t out_size_ = 0;
	std::uint32_t counts_buf_[format::block] = {}, block_buf_[format::block] = {};
	std::uint64_t decoded_counts_ = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t decoded_block_ = std::numeric_limits<std::uint64_t>::max(), decoded_offset_ = 0;
};

template <class Live, class Emit>
void inverted_plugin::records(const reader& r, const core_segment& core, Live&& live, Emit&& emit) {
	for (ordinal_t o = 0; o < core.doc_count(); ++o) {
		if (live(o)) { length_record(core.docid(o), r.length(o), emit); }
	}
	std::string key;  // a posting's term as the digest names it: the field id's varint, then the term
	r.scan_terms([&](std::uint32_t field, std::string_view term, const format::term_info& info) {
		key.clear();
		fielddict::put_field(key, field);
		key += term;
		postings_iterator it(r, info);
		positions_reader pr(r, info);
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
			if (live(o)) { posting_record(core.docid(o), key, it.wdf(), pr.read(it), emit); }
		}
		return true;
	});
}

template <class Emit>
void inverted_plugin::records(std::uint64_t docid, const part& p, Emit&& emit) {
	// A split text counts its words; the references were resolved to ids when the part was buffered.
	std::uint64_t split_length = 0;
	for (const auto& f : p.fields) {
		if (f.split_text) { split_length += text_detail::count_words(f.text).words; }
	}
	length_record(docid, std::uint32_t(p.length ? *p.length : split_length + [&] {
		std::uint64_t n = 0;
		for (const auto& f : p.fields) { n += f.split_text ? 0 : f.length; }
		return n;
	}()), emit);
	std::map<std::string, std::pair<std::uint32_t, std::vector<std::uint32_t>>> postings;  // the digest's key: field varint, term
	std::string key;
	auto key_of = [&](const field_part& f, std::string_view term) {
		key.clear();
		fielddict::put_field(key, f.field.n);
		key += term;
		return key;
	};
	for (const auto& f : p.fields) {
		for (const auto& t : f.terms) { postings[key_of(f, t.term)] = {t.wdf, t.positions}; }
		const std::string_view text = f.text;
		std::uint32_t position = 0;
		if (f.split_text) {
			text_detail::split_words(text, [&](std::size_t offset, std::size_t size) {
				auto& e = postings[key_of(f, text.substr(offset, size))];
				++e.first;
				e.second.push_back(++position);
			});
		}
		for (const auto& tk : f.tokens) {
			auto& e = postings[key_of(f, text.substr(tk.offset, tk.size))];
			++e.first;
			e.second.push_back(tk.position);
		}
	}
	for (const auto& [term, e] : postings) { posting_record(docid, term, e.first, e.second, emit); }
}

inline void inverted_plugin::check(const reader& inverted, const core_segment& core, report& r, std::string_view) {
	for (ordinal_t o = 0; o < core.doc_count(); ++o) { (void)inverted.length(o); }
	inverted.dictionary().check();
	inverted.scan_terms([&](std::uint32_t, std::string_view, const format::term_info& info) {
		++r.terms;
		postings_iterator it(inverted, info);
		positions_reader pr(inverted, info);
		std::vector<std::pair<std::uint32_t, std::uint32_t>> postings;  // (wdf, length), for the seeds
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
			++r.postings;
			r.positions += pr.read(it).size();
			if (it.seed_count()) { postings.emplace_back(it.wdf(), inverted.length(o)); }
		}
		// Each threshold seed pair dominates at least its k postings (F13).
		for (std::size_t j = 0; j < it.seed_count(); ++j) {
			const auto& p = it.seed_at(j);
			std::uint32_t by_code = 0, exact = 0;
			for (const auto& [wdf, length] : postings) {
				by_code += wdf >= p.code_wdf && format::encode_length(length) <= p.code;
				exact += wdf >= p.exact_wdf && length <= p.exact;
			}
			if (by_code < format::seed_ks[j] || exact < format::seed_ks[j]) { corrupt("threshold seed"); }
		}
		return true;
	});
}

}  // namespace prospect
