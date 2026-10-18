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

// Reading a segment: open-time verification, term lookup, and the postings iterator
// with lazily decoded skip groups. Every offset read from the file is bounds-checked;
// a violation throws corrupt_segment, which fences the segment.

#pragma once

#include "bitpack.hh"
#include "compressor_zstd.h"
#include "docset.hh"
#include "fusefilter.hh"
#include "hashes.hh"
#include "residency.hh"
#include "strata/docids.hh"
#include "strata/format.hh"
#include "strata/keys.hh"
#include "strata/section_io.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace strata {

using docset::no_more_docs;

// --- layouts (layers.md, L2) -----------------------------------------------------------

// What stage-1 checks may consult (L4); Phase 4 adds the slot table.
struct validate_context {
	std::string_view key_prefix;  // the index's (F8); empty in a keyless index
};

// One digest record (L8): a tag, then little-endian fixed-width fields; strings carry their
// length as u32.
class digest_record {
public:
	explicit digest_record(char tag) : rec_(1, tag) {}
	void u32(std::uint32_t v) { format::put<std::uint32_t>(rec_, v); }
	void u64(std::uint64_t v) { format::put<std::uint64_t>(rec_, v); }
	void bytes(std::string_view b) {
		u32(std::uint32_t(b.size()));
		rec_ += b;
	}
	std::string_view data() const noexcept { return rec_; }

private:
	std::string rec_;
};

// The core's fields of a document (L10): its key, empty for none (F8).
struct core_part {
	std::string key;
};

// One term of a document as the host builds it; terms are unique within a document.
struct term_entry {
	std::string term;
	std::uint32_t wdf = 1;
	std::vector<std::uint32_t> positions;
};

template <class... Entries>
struct layout {};
template <class... Ts>
struct type_list {};

// A segment's live documents: a bit per ordinal set when deleted (null: none deleted).
struct live_bits {
	const std::uint64_t* deleted = nullptr;
	bool live(ordinal_t o) const noexcept { return !deleted || !((deleted[o >> 6] >> (o & 63)) & 1); }
};
// Format 12 only: where the core's docids section sits between plugins in file order.
struct docids_section {};
struct inverted_plugin;
struct keys_plugin;
struct merge_context;
struct stored_plugin;
using search_layout_12 = layout<inverted_plugin, docids_section, keys_plugin, stored_plugin>;

template <class Layout>
class basic_segment;
using segment = basic_segment<search_layout_12>;

// The core of an open segment (L7): the prefix first (F2), then the footer, table of
// contents, and header; the structure bits against those this binary reads (F4); every
// pinned section's checksum; stats' document count; docids; and the one verified-region
// state that every regions-checked section shares.
class core_segment {
public:
	core_segment(std::string_view file, std::uint32_t readable) : file_(file) {
		version_ = format::read_version(file);
		footer_ = format::decode_footer(file);
		sections_ = format::decode_toc(file, footer_);
		if (format::get<std::uint32_t>(file.data() + 16) != 0) { corrupt("header flags"); }
		format::check_structures(sections_, footer_.structures, readable);
		// Each regions-checked section's CRCs move to its span; others are dropped. The
		// table's rank order makes every kind appear at most once.
		std::vector<residency::verified_regions::span> spans;
		for (auto& e : sections_) {
			const format::check c = format::rule_of(e.kind)->verify;
			if (c == format::check::regions) {
				spans.push_back({e.offset, e.length, e.region_shift, std::move(e.region_crcs)});
			} else {
				std::vector<std::uint32_t>().swap(e.region_crcs);
			}
			if (c == format::check::pinned) {
				if (hashes::crc32c(file.data() + e.offset, e.length) != e.crc) { corrupt("section checksum"); }
			}
			if (e.kind == format::section::docids) { docids_ = file.substr(e.offset, e.length); }
			if (e.kind == format::section::stats) { stats_ = file.substr(e.offset, e.length); }
		}
		if (stats_.size() != 8) { corrupt("stats"); }
		doc_count_ = format::get<std::uint64_t>(stats_.data());
		if (doc_count_ != footer_.doc_count) { corrupt("document count"); }
		std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
		try {
			regions_->regions = residency::verified_regions(
				std::span<const std::byte>(reinterpret_cast<const std::byte*>(file.data()), file.size()), std::move(spans));
		} catch (const std::invalid_argument&) {
			corrupt("section regions");  // spans out of bounds, overlapping, or miscounted
		}
		validate_docids();
	}

	// The structure bits the core reads (F4): none until index_sort's reader (F18).
	static constexpr std::uint32_t reads = 0;

	// The handle for a section, or an empty one when the segment lacks it.
	section_handle section(format::section kind) const {
		for (const auto& e : sections_) {
			if (e.kind == kind) { return {file_.substr(e.offset, e.length), e.offset, &regions_->regions}; }
		}
		return {{}, 0, &regions_->regions};
	}
	const format::section_entry* entry(format::section kind) const {
		for (const auto& e : sections_) {
			if (e.kind == kind) { return &e; }
		}
		return nullptr;
	}

	std::uint64_t doc_count() const noexcept { return doc_count_; }
	std::uint32_t version() const noexcept { return version_; }
	// The footer's structure bits (F4).
	std::uint32_t structures() const noexcept { return footer_.structures; }
	std::string_view stats() const noexcept { return stats_; }
	const std::vector<format::section_entry>& sections() const noexcept { return sections_; }
	std::uint64_t min_docid() const noexcept { return footer_.min_docid; }
	std::uint64_t max_docid() const noexcept { return footer_.max_docid; }
	std::size_t verified_regions() const noexcept { return regions_->regions.verified(); }
	bool verify(std::uint64_t offset, std::uint64_t length) const { return regions_->regions.verify({offset, length}); }

	// The host docid of an ordinal.
	std::uint64_t docid(ordinal_t ord) const { return ids_.docid(ord); }
	// The ordinal holding docid, if any.
	std::optional<ordinal_t> ordinal_of(std::uint64_t docid) const { return ids_.ordinal_of(docid); }
	// Whether ordinal order is docid order: false only for a reordering merge's output.
	bool docid_ordered() const noexcept { return ids_.docid_ordered(); }

private:
	// docids: see docids.hh; validated whole at open.
	void validate_docids() { ids_ = docids::reader(docids_, doc_count_, footer_.min_docid, footer_.max_docid); }

	struct shared {
		residency::verified_regions regions;
	};
	std::string_view file_, docids_, stats_;
	docids::reader ids_;
	format::footer footer_;
	std::vector<format::section_entry> sections_;
	std::unique_ptr<shared> regions_ = std::make_unique<shared>();  // stable address for handles
	std::uint64_t doc_count_ = 0;
	std::uint32_t version_ = 0;
};

// What a plugin reader's constructor sees (L7): `present` is its structure bit, which the
// core read from the footer before any reader opened.
struct open_context {
	std::uint32_t version;
	std::uint64_t doc_count;
	const core_segment& core;
	bool present = true;
	section_handle section(format::section kind) const { return core.section(kind); }
};

// Terms, postings, positions, lengths, and norms.
struct inverted_plugin {
	static constexpr std::string_view id = "inverted";

	// What a document carries for it (L10).
	struct part {
		std::vector<term_entry> terms;
		std::uint32_t length = 0;
	};
	// Stage-1 checks (D10): terms non-empty, within the term limit, unique, positions
	// increasing, and none with the index's key prefix, which names keys (F8).
	static bool validate(const validate_context& vc, const part& p) {
		std::vector<std::string_view> seen;
		seen.reserve(p.terms.size());
		for (const auto& t : p.terms) {
			if (t.term.empty() || t.term.size() > format::max_term_bytes) { return false; }
			if (!vc.key_prefix.empty() && t.term.starts_with(vc.key_prefix)) { return false; }
			seen.push_back(t.term);
			for (std::size_t i = 1; i < t.positions.size(); ++i) {
				if (t.positions[i] <= t.positions[i - 1]) { return false; }
			}
		}
		std::sort(seen.begin(), seen.end());
		return std::adjacent_find(seen.begin(), seen.end()) == seen.end();
	}
	// Buffer memory and an overestimate of disk bytes.
	static std::uint64_t bytes(const part& p) {
		std::uint64_t n = 0;
		for (const auto& t : p.terms) { n += sizeof(term_entry) + t.term.size() + 4 * t.positions.size(); }
		return n;
	}
	static std::uint64_t disk_estimate(const part& p) {
		std::uint64_t n = 0;
		for (const auto& t : p.terms) { n += t.term.size() + 12 + 4 * t.positions.size(); }
		return n;
	}
	// The bytes of its statistics at the head of term_index (F7).
	static constexpr std::size_t statistics_size = 20;
	// Its sections are F3's rows owned by it; its bits are F4's (L4).
	static constexpr format::owner owner = format::owner::inverted;
	static constexpr std::uint32_t structure = format::bit::inverted;
	static constexpr std::uint32_t structures = format::bit::inverted | format::bit::norms;

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
			max_positions_ = format::get<std::uint32_t>(term_index.data() + 16);
			state_->terms = c.section(format::section::terms);
			postings_ = c.section(format::section::postings);
			positions_ = c.section(format::section::positions);
			lengths_ = c.section(format::section::lengths).bytes;
			if (const auto* e = c.core.entry(format::section::lengths)) { lengths_crc_ = e->crc; }
			if (const auto* e = c.core.entry(format::section::postings)) { postings_shift_ = e->region_shift; }
			norms_ = c.section(format::section::norms).bytes;
			if (lengths_.empty()) { corrupt("lengths"); }
			length_width_ = std::uint8_t(lengths_[0]);
			if (length_width_ > 32 || lengths_.size() != 1 + (doc_count_ + 127) / 128 * bitpack::packed_bytes(length_width_)) {
				corrupt("lengths size");
			}
			if (!(c.core.structures() & format::bit::norms)) {
				// Norms not built (F4): each document's from its exact length, verified first.
				verify_lengths();
				state_->owned_norms.resize(doc_count_);
				for (ordinal_t o = 0; o < doc_count_; ++o) { state_->owned_norms[o] = char(format::encode_length(length(o))); }
				norms_ = state_->owned_norms;
			}
			if (norms_.size() != doc_count_) { corrupt("norms size"); }
			// The term index is copied out of the mapping (verified at open), so later lookups
			// never read bytes that could change or fault under the FST's unchecked traversal.
			state_->pinned_index.assign(term_index.substr(statistics_size));
			try {
				dict_.emplace(state_->terms.bytes, state_->pinned_index, &verify_terms, state_.get());  // validates the FST index
			} catch (const termdict::corrupt&) {
				corrupt("term index");
			} catch (const std::invalid_argument&) {
				corrupt("term index");  // the FST's own check
			}
		}

		std::uint8_t norm(ordinal_t ord) const noexcept { return std::uint8_t(norms_[ord]); }
		void verify_postings(std::uint64_t offset, std::uint64_t length) const {
			if (!postings_.verify(offset, length)) { corrupt("postings checksum"); }
		}
		std::uint8_t postings_region_shift() const noexcept { return postings_shift_; }
		void verify_positions(std::uint64_t offset, std::uint64_t length) const {
			if (!positions_.verify(offset, length)) { corrupt("positions checksum"); }
		}
		std::uint32_t max_positions() const noexcept { return max_positions_; }
		std::uint64_t total_length() const noexcept { return total_length_; }
		double average_length() const noexcept { return doc_count_ ? double(total_length_) / double(doc_count_) : 0.0; }

		// Document length by ordinal: one lane extraction from the bit-packed block. The
		// section is verified whole on the first call (the default scheme reads norms only).
		std::uint32_t length(ordinal_t ord) const {
			if (state_->lengths_state.load(std::memory_order_acquire) != 1) [[unlikely]] { verify_lengths(); }
			const unsigned w = length_width_;
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
		std::optional<format::term_info> term(std::string_view key) const {
			auto info = find_term(key);
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
		std::optional<format::term_info> find_term(std::string_view key) const {
			// The run's values end with key's: the earlier ones only move the cursor.
			format::term_run run, before;
			std::string_view last;
			std::optional<std::string_view> v;
			try {
				v = dict_->find_run(key, [&](std::string_view value) {
					before = run;
					(void)format::decode_term_info(value, run, false);
					last = value;
				});
			} catch (const termdict::corrupt&) {
				corrupt("terms checksum");  // a dictionary block failed its region check
			}
			if (!v) { return std::nullopt; }
			auto info = format::decode_term_info(last, before);
			if (info.df > doc_count_) { corrupt("document frequency"); }
			return info;
		}

		template <class F>
		void scan_terms(F&& f) const {
			format::term_run run;
			try {
				dict_->scan("", [&](std::string_view key, std::string_view value) {
					const auto info = format::decode_term_info(value, run);
					if (info.df > doc_count_) { corrupt("document frequency"); }
					return f(key, info);
				});
			} catch (const termdict::corrupt&) {
				corrupt("terms checksum");
			}
		}
		template <class F>
		void scan_dictionary(std::string_view from, F&& f) const {
			try {
				dict_->scan(from, std::forward<F>(f));
			} catch (const termdict::corrupt&) {
				corrupt("terms checksum");
			}
		}

		std::string_view postings_bytes() const noexcept { return postings_.bytes; }
		std::string_view positions_bytes() const noexcept { return positions_.bytes; }
		const termdict::reader& dictionary() const noexcept { return *dict_; }

		// Every term in order with its info (impacts left empty below 128 postings), pulled
		// one at a time, for k-way merges. Neither copied nor moved.
		class term_cursor {
		public:
			explicit term_cursor(const reader& r) : r_(&r), c_(r.dictionary()) {}
			term_cursor(const term_cursor&) = delete;
			term_cursor& operator=(const term_cursor&) = delete;
			// Moves to the next term; false past the last.
			bool next() {
				try {
					if (!c_.next()) { return false; }
					info_ = format::decode_term_info(c_.value(), run_);
				} catch (const termdict::corrupt&) {
					corrupt("terms checksum");
				}
				if (info_.df > r_->doc_count()) { corrupt("document frequency"); }
				return true;
			}
			std::string_view term() const noexcept { return c_.key(); }
			const format::term_info& info() const noexcept { return info_; }

		private:
			const reader* r_;
			termdict::reader::cursor c_;
			format::term_run run_;
			format::term_info info_;
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
		void verify_lengths() const {
			auto& st = state_->lengths_state;
			if (st.load(std::memory_order_acquire) == 0) { st.store(hashes::crc32c(lengths_.data(), lengths_.size()) == lengths_crc_ ? 1 : 2, std::memory_order_release); }
			if (st.load(std::memory_order_acquire) != 1) { corrupt("lengths checksum"); }
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
			auto tables = termdict::builder().finish();
			state_->owned_blocks = std::move(tables.blocks);
			state_->pinned_index = std::move(tables.index);
			state_->owned_lengths.assign(1, '\0');  // width 0
			lengths_ = state_->owned_lengths;
			lengths_crc_ = hashes::crc32c(lengths_.data(), lengths_.size());
			state_->owned_norms.assign(doc_count_, '\0');
			norms_ = state_->owned_norms;
			dict_.emplace(state_->owned_blocks, state_->pinned_index);
		}
		// Heap-held, so the dictionary's verifier context survives moving the segment.
		struct state {
			section_handle terms;
			std::string pinned_index;  // the term index, copied out of the mapping
			std::string owned_blocks, owned_lengths, owned_norms;  // stand-ins for absent sections
			std::atomic<std::uint8_t> lengths_state{0};  // 0 unverified, 1 good, 2 bad
		};
		std::unique_ptr<state> state_;
		section_handle postings_, positions_;
		std::string_view lengths_, norms_;
		std::optional<termdict::reader> dict_;
		std::uint64_t doc_count_ = 0, total_length_ = 0;
		std::uint32_t max_positions_ = 0, lengths_crc_ = 0;
		std::uint8_t length_width_ = 0, postings_shift_ = 0;
	};
	// L6: rebuilds the plugin's sections for a merge's output (merge.hh).
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink);
	// Its digest records (L8, F21): per document a length record, length 0 included, and per
	// posting a P record; over a segment's live ordinals, or a buffered document.
	template <class Live, class Emit>
	static void records(const reader& r, const core_segment& core, Live&& live, Emit&& emit);
	template <class Emit>
	static void records(std::uint64_t docid, const part& p, Emit&& emit);
};

// The document's key in its own dictionary (F8), and versions (F9).
struct keys_plugin {
	static constexpr std::string_view id = "keys";

	// A document's key is the core's field (L10), held exactly as given. Without a key
	// prefix the index is keyless and has none (F13-R33); with one, a key that lacks it is
	// invalid (F13-R25 item 2).
	static std::optional<std::string> key_of(const validate_context& vc, const core_part& d, bool& valid) {
		valid = true;
		if (vc.key_prefix.empty() || d.key.empty()) { return std::nullopt; }
		if (!d.key.starts_with(vc.key_prefix)) { valid = false; }
		return d.key;
	}
	static constexpr format::owner owner = format::owner::keys;
	static constexpr std::uint32_t structure = format::bit::keys;
	static constexpr std::uint32_t structures = format::bit::keys;

	// Keys are this plugin's own section, so its reader needs no other (L3).
	using depends_on = type_list<>;
	struct deps {};

	class reader {
	public:
		explicit reader(const open_context& c)
			: state_(std::make_unique<state>()), versions_(c.section(format::section::versions).bytes), doc_count_(c.doc_count) {
			state_->keys = c.section(format::section::keys);  // empty when absent, verifying as any handle
			if (!c.present) { return; }  // no keys; every version 0 (F4, F9)
			validate_versions();
			keys_ = keys::reader(state_->keys.bytes, c.section(format::section::key_index).bytes, doc_count_, &verify_keys, state_.get());
		}
		// The document's version (0 when the segment's writer recorded none).
		std::uint64_t version(ordinal_t ord) const {
			if (ord >= doc_count_) { throw std::out_of_range("strata: ordinal out of range"); }
			if (versions_.empty()) { return 0; }
			const unsigned width = std::uint8_t(versions_[8]);
			std::uint64_t v = 0;
			for (unsigned b = 0; b < width; ++b) {
				const std::uint64_t at = std::uint64_t(ord) * width + b;
				v |= std::uint64_t(std::uint8_t(versions_[9 + (at >> 3)]) >> (at & 7) & 1) << b;
			}
			return format::get<std::uint64_t>(versions_.data()) + v;
		}
		void verify_all() const {
			if (!state_->keys.verify(0, state_->keys.bytes.size())) { corrupt("section checksum"); }
		}
		// Whether the segment may hold this key: inside its key range and in its filter
		// (false positives possible, false negatives not).
		bool may_hold(std::string_view key) const { return keys_.may_hold(key); }
		// The key's ordinal, if the segment holds it and it is live.
		std::optional<ordinal_t> find(std::string_view key, live_bits live) const {
			const auto ord = keys_.find(key);
			if (ord && !live.live(*ord)) { return std::nullopt; }
			return ord;
		}
		// Calls f(key, ord) for every key >= from, in order, deleted copies included, until f
		// returns false (F8; PLAN.md:47-48).
		template <class F>
		void scan(std::string_view from, F&& f) const {
			keys_.scan(from, std::forward<F>(f));
		}
		// The keys' count, and every key in order with its ordinal, checked as walked.
		std::uint64_t key_count() const noexcept { return keys_.size(); }
		keys::reader::cursor cursor() const { return keys::reader::cursor(keys_); }

	private:
		// versions: [min u64][width u8][packed bits]; exactly the bits doc_count needs, and no
		// version past 2^64 - 1.
		void validate_versions() const {
			if (versions_.size() < 9) { corrupt("versions"); }
			const unsigned width = std::uint8_t(versions_[8]);
			const std::uint64_t lo = format::get<std::uint64_t>(versions_.data());
			const std::uint64_t mask = width >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << width) - 1;
			if (width > 64 || versions_.size() != 9 + (doc_count_ * width + 7) / 8 || lo > ~std::uint64_t(0) - mask) {
				corrupt("versions");
			}
		}
		static bool verify_keys(void* context, std::uint64_t offset, std::uint64_t length) {
			return static_cast<state*>(context)->keys.verify(offset, length);
		}
		struct state {  // heap-held, so the dictionary's verifier context survives a move
			section_handle keys;
		};
		std::unique_ptr<state> state_;
		std::string_view versions_;
		keys::reader keys_;
		std::uint64_t doc_count_;
	};
	// L6: rebuilds the plugin's sections for a merge's output (merge.hh).
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink);
};

// Stored records in zstd frames.
struct stored_plugin {
	static constexpr std::string_view id = "stored";

	struct part {
		std::string stored;
	};
	static bool validate(const validate_context&, const part& p) { return p.stored.size() <= format::max_stored_record; }
	static std::uint64_t bytes(const part& p) { return p.stored.size(); }
	static std::uint64_t disk_estimate(const part& p) { return p.stored.size() + p.stored.size() / 16; }
	static constexpr format::owner owner = format::owner::stored;
	static constexpr std::uint32_t structure = format::bit::stored;
	static constexpr std::uint32_t structures = format::bit::stored;

	class reader {
	public:
		explicit reader(const open_context& c) : stored_(c.section(format::section::stored)), doc_count_(c.doc_count) {}

		// One decoded stored frame: its first ordinal and each record's place in `raw`.
		struct stored_frame {
			std::uint32_t index = ~std::uint32_t(0);
			ordinal_t first = 0;
			std::string raw;
			std::vector<std::uint64_t> starts;  // records + 1 offsets into raw
			std::string_view record(ordinal_t ord) const { return std::string_view(raw).substr(starts[ord - first], starts[ord - first + 1] - starts[ord - first]); }
			bool holds(ordinal_t ord) const noexcept { return index != ~std::uint32_t(0) && ord >= first && ord - first + 1 < starts.size(); }
		};
		// The number of stored frames (0 when no document has a record).
		std::uint32_t stored_frames() const {
			const std::string_view s = stored_.bytes;
			if (s.size() < 4) { return 0; }
			const std::uint32_t frames = format::get<std::uint32_t>(s.data() + s.size() - 4);
			if (std::uint64_t(frames) * 16 > s.size() - 4) { corrupt("stored index"); }
			return frames;
		}
		// Decodes frame f into out. Its range is verified before it is decompressed, into
		// exactly its recorded size.
		void decode_stored_frame(std::uint32_t f, stored_frame& out) const {
			const std::string_view s = stored_.bytes;
			const std::uint32_t frames = stored_frames();
			const std::uint64_t index_at = s.size() - 4 - std::uint64_t(frames) * 16;
			if (!stored_.verify(index_at, s.size() - index_at)) { corrupt("stored checksum"); }
			if (f >= frames) { corrupt("stored index"); }
			const char* e = s.data() + index_at + 16 * std::uint64_t(f);
			const std::uint32_t first = format::get<std::uint32_t>(e);
			const std::uint32_t raw_size = format::get<std::uint32_t>(e + 4);
			if (raw_size > format::max_stored_frame) { corrupt("stored frame size"); }
			const std::uint64_t begin = format::get<std::uint64_t>(e + 8);
			const std::uint64_t end = f + 1 < frames ? format::get<std::uint64_t>(e + 24) : index_at;
			if (begin > end || end > index_at) { corrupt("stored frame bounds"); }
			if (f + 1 < frames && format::get<std::uint32_t>(e + 16) <= first) { corrupt("stored index"); }
			if (!stored_.verify(begin, end - begin)) { corrupt("stored checksum"); }
			try {
				out.raw = decompress_zstd(s.substr(begin, end - begin), raw_size);
			} catch (const std::exception&) {
				corrupt("stored frame");
			}
			const char* p = out.raw.data();
			const char* rend = p + out.raw.size();
			const std::uint64_t n = format::get_leb(&p, rend);
			if (n > doc_count_ || first > doc_count_ - n) { corrupt("stored frame count"); }
			out.starts.assign(1, 0);
			for (std::uint64_t i = 0; i < n; ++i) {
				const std::uint64_t len = format::get_leb(&p, rend);
				if (len > out.raw.size()) { corrupt("stored record length"); }
				out.starts.push_back(out.starts.back() + len);
			}
			const std::uint64_t header = std::uint64_t(p - out.raw.data());
			if (out.starts.back() > std::uint64_t(rend - p)) { corrupt("stored record bounds"); }
			for (auto& v : out.starts) { v += header; }
			out.index = f;
			out.first = first;
		}
		// The stored frame whose ordinals start at or before ord, or none without a stored section.
		std::optional<std::uint32_t> stored_frame_of(ordinal_t ord) const {
			const std::string_view s = stored_.bytes;
			if (s.size() < 4) { return std::nullopt; }
			const std::uint32_t frames = stored_frames();
			const std::uint64_t index_at = s.size() - 4 - std::uint64_t(frames) * 16;
			if (!stored_.verify(index_at, s.size() - index_at)) { corrupt("stored checksum"); }
			if (frames == 0) { corrupt("stored index"); }
			auto first_of = [&](std::uint32_t f) { return format::get<std::uint32_t>(s.data() + index_at + 16 * std::uint64_t(f)); };
			std::uint32_t lo = 0, hi = frames;  // the last frame whose first ordinal <= ord
			while (hi - lo > 1) {
				const std::uint32_t mid = lo + (hi - lo) / 2;
				if (first_of(mid) <= ord) { lo = mid; } else { hi = mid; }
			}
			if (first_of(lo) > ord) { corrupt("stored index"); }
			return lo;
		}
		// The document's stored record (empty when it has none), through `cache`: a reader in
		// ordinal order decodes each frame once.
		std::string_view stored(ordinal_t ord, stored_frame& cache) const {
			if (ord >= doc_count_) { throw std::out_of_range("strata: ordinal out of range"); }
			if (cache.holds(ord)) { return cache.record(ord); }
			const auto f = stored_frame_of(ord);
			if (!f) { return {}; }
			decode_stored_frame(*f, cache);
			if (!cache.holds(ord)) { corrupt("stored frame count"); }
			return cache.record(ord);
		}
		// The document's stored record (empty when it has none); decodes its frame.
		std::string stored(ordinal_t ord) const {
			stored_frame f;
			return std::string(stored(ord, f));
		}
		void verify_all() const {
			if (!stored_.verify(0, stored_.bytes.size())) { corrupt("section checksum"); }
		}

	private:
		section_handle stored_;
		std::uint64_t doc_count_;
	};
	// L6: rebuilds the plugin's sections for a merge's output (merge.hh).
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink);
	// Its digest records (L8, F21): a record of a document's stored bytes, none for an empty one.
	template <class Live, class Emit>
	static void records(const reader& r, const core_segment& core, Live&& live, Emit&& emit) {
		reader::stored_frame cache;
		for (ordinal_t o = 0; o < core.doc_count(); ++o) {
			if (live(o)) { record(core.docid(o), r.stored(o, cache), emit); }
		}
	}
	template <class Emit>
	static void records(std::uint64_t docid, const part& p, Emit&& emit) {
		record(docid, p.stored, emit);
	}
	template <class Emit>
	static void record(std::uint64_t docid, std::string_view bytes, Emit& emit) {
		if (bytes.empty()) { return; }
		digest_record rec('R');
		rec.u64(docid);
		rec.bytes(bytes);
		emit(rec.data());
	}
};

// --- documents (L10) --------------------------------------------------------------------

template <class E>
struct empty_part {};  // one distinct type per layout entry, so empty bases never repeat
namespace detail {
template <class E>
struct plugin_part {
	using type = empty_part<E>;
};
template <class E>
	requires requires { typename E::part; }
struct plugin_part<E> {
	using type = typename E::part;
};
template <class E>
using plugin_part_t = typename plugin_part<E>::type;
}  // namespace detail

template <class Layout>
struct basic_document;
// The document is its parts: `d.terms`, `d.length`, and `d.stored` name the plugins'.
template <class... Entries>
struct basic_document<layout<Entries...>> : core_part, detail::plugin_part_t<Entries>... {
	// Stage-1 checks in layout order; false on the first that fails.
	bool valid(const validate_context& vc) const { return (valid_part<Entries>(vc) && ...); }
	// Buffer memory beyond the document object, and the parts' disk estimate.
	std::uint64_t part_bytes() const { return (part_bytes_of<Entries>() + ... + 0); }
	std::uint64_t disk_estimate() const { return (disk_estimate_of<Entries>() + ... + 0); }

private:
	template <class E>
	bool valid_part(const validate_context& vc) const {
		if constexpr (requires { typename E::part; }) {
			return E::validate(vc, static_cast<const typename E::part&>(*this));
		} else {
			return true;
		}
	}
	template <class E>
	std::uint64_t part_bytes_of() const {
		if constexpr (requires { typename E::part; }) {
			return E::bytes(static_cast<const typename E::part&>(*this));
		} else {
			return 0;
		}
	}
	template <class E>
	std::uint64_t disk_estimate_of() const {
		if constexpr (requires { typename E::part; }) {
			return E::disk_estimate(static_cast<const typename E::part&>(*this));
		} else {
			return 0;
		}
	}
};
using document = basic_document<search_layout_12>;
namespace detail {
struct document_12 {  // format 12's document, before the parts, and its key (F8)
	std::string key;
	std::vector<term_entry> terms;
	std::uint32_t length = 0;
	std::string stored;
};
}  // namespace detail
static_assert(sizeof(document) == sizeof(detail::document_12), "doc_bytes and every flush trigger depend on it");

namespace detail {
template <class E>
struct reader_of {
	using type = typename E::reader;
};
template <>
struct reader_of<docids_section> {  // the core's section: no reader of its own
	struct type {
		explicit type(const open_context&) {}
	};
};
// The structure bits a layout entry reads, and its own bit (none for the core's docids).
template <class E>
constexpr std::uint32_t structures_of() {
	if constexpr (std::is_same_v<E, docids_section>) {
		return 0;
	} else {
		return E::structures;
	}
}
template <class E>
constexpr std::uint32_t structure_of() {
	if constexpr (std::is_same_v<E, docids_section>) {
		return 0;
	} else {
		return E::structure;
	}
}
// No two plugins of a layout share an owner, so none claims another's kinds (F3), and no
// two read one bit (F4).
template <class... Entries>
consteval bool owners_distinct() {
	std::uint32_t bits = core_segment::reads;
	bool ok = true;
	(..., [&] {
		ok = ok && !(bits & structures_of<Entries>());
		bits |= structures_of<Entries>();
	}());
	std::array<int, 8> owners{};
	(..., [&] {
		if constexpr (!std::is_same_v<Entries, docids_section>) { ok = ok && owners[std::size_t(Entries::owner)]++ == 0; }
	}());
	return ok;
}
}  // namespace detail

// A plugin's shape (L4): its owner in F3's table and its bits in F4's, its own bit among
// them; the builder joins it at the writer façade's split.
template <class P>
concept section_plugin = requires(const typename P::reader& r) {
	{ P::id } -> std::convertible_to<std::string_view>;
	{ P::owner } -> std::convertible_to<format::owner>;
	{ P::structure } -> std::convertible_to<std::uint32_t>;
	{ P::structures } -> std::convertible_to<std::uint32_t>;
	requires ((P::structures & P::structure) == P::structure) && (std::popcount(P::structure) == 1);
	r.verify_all();
} && std::constructible_from<typename P::reader, const open_context&> &&
	requires(const merge_context& m, std::span<const typename P::reader* const> in, section_sink& sink) {
		{ P::merge(m, in, sink) } -> std::same_as<built>;
	} &&
	(!requires { typename P::part; } || requires(const validate_context& vc, const typename P::part& part) {
		{ P::validate(vc, part) } -> std::same_as<bool>;
		{ P::bytes(part) } -> std::same_as<std::uint64_t>;
		{ P::disk_estimate(part) } -> std::same_as<std::uint64_t>;
	});

// An open segment of a layout: the core, then each plugin's reader in layout order.
// Callers name the reader that owns each concern (L11).
template <class... Entries>
class basic_segment<layout<Entries...>> {
	static_assert(((std::is_same_v<Entries, docids_section> || section_plugin<Entries>) && ...), "every layout entry is a section plugin");
	static_assert((std::size_t(std::is_same_v<Entries, docids_section>) + ... + 0) <= 1, "docids_section appears at most once");
	static_assert(detail::owners_distinct<Entries...>(), "two plugins share an owner or a structure bit");
	// The bits this layout reads (F4; layers R16): the core's and each entry's.
	static constexpr std::uint32_t readable = (core_segment::reads | ... | detail::structures_of<Entries>());

public:
	// Verifies the footer, the table of contents, the header, and the pinned sections in
	// full; other sections are verified per region as they are read. The file's bytes
	// must outlive the segment; a temporary string cannot be one.
	explicit basic_segment(std::string&&) = delete;
	explicit basic_segment(std::string_view file) : core_(file, readable), present_(presence(core_)), readers_(open(core_, present_)) {}

	// The core's view: docids, ordinals, the document count, the table of contents.
	const core_segment& core() const noexcept { return core_; }
	// Plugin P's reader: a compile-time offset into the reader tuple (L11).
	template <class P>
	const auto& get() const noexcept {
		static_assert(index_of<P>() < sizeof...(Entries), "the plugin is not in this segment's layout");
		return std::get<typename detail::reader_of<P>::type>(readers_);
	}
	// What P's reader needs from the readers it depends on, built at each call (L3).
	template <class P>
	typename P::deps deps() const noexcept { return deps_of<P>(typename P::depends_on{}); }
	// Whether P is in this layout, at compile time (L11): guard get<P>() with `if constexpr`.
	template <class P>
	static constexpr bool contains() noexcept {
		return index_of<P>() < sizeof...(Entries);
	}
	// Whether this segment holds P's structure (L11): a bit per layout entry, set at open;
	// false, with no load, for a plugin outside the layout.
	template <class P>
	bool holds() const noexcept {
		if constexpr (index_of<P>() == sizeof...(Entries)) {
			return false;
		} else {
			return (present_ >> index_of<P>()) & 1;
		}
	}
	// Verifies every byte of the sections checked on first read; throws corrupt_segment at
	// the first one that differs.
	void verify_all() const {
		(..., verify_all_of<Entries>());
	}

private:
	using readers = std::tuple<typename detail::reader_of<Entries>::type...>;
	static_assert(sizeof...(Entries) <= 32, "a layout's presence mask holds 32 entries");
	// Bit i: entry i's own structure bit is set in the footer (the core's docids always).
	static std::uint32_t presence(const core_segment& core) noexcept {
		std::uint32_t out = 0, i = 0;
		(..., (out |= std::uint32_t(!detail::structure_of<Entries>() || (core.structures() & detail::structure_of<Entries>())) << i++));
		return out;
	}
	static readers open(const core_segment& core, std::uint32_t present) {
		std::uint32_t i = 0;
		auto context = [&](std::uint32_t at) { return open_context{core.version(), core.doc_count(), core, bool((present >> at) & 1)}; };
		return readers{typename detail::reader_of<Entries>::type(context(i++))...};
	}
	template <class P, class... Ds>
	typename P::deps deps_of(type_list<Ds...>) const noexcept {
		static_assert(((index_of<Ds>() < index_of<P>()) && ...), "a plugin's dependencies come earlier in its layout");
		return typename P::deps{get<Ds>()...};
	}
	template <class T>
	static constexpr std::size_t index_of() {  // sizeof...(Entries) when T is not in the layout
		constexpr bool same[] = {std::is_same_v<T, Entries>...};
		for (std::size_t i = 0; i < sizeof...(Entries); ++i) {
			if (same[i]) { return i; }
		}
		return sizeof...(Entries);
	}
	template <class E>
	void verify_all_of() const {
		if constexpr (!std::is_same_v<E, docids_section>) { get<E>().verify_all(); }
	}

	core_segment core_;
	std::uint32_t present_;  // bit i: layout entry i is present; set before the readers open (F4)
	readers readers_;
};

// Readers hold views of the file and pointers into heap-held state, so a segment moves
// without leaving one dangling (layers.md L3).
static_assert(std::is_nothrow_move_constructible_v<segment>);

// Iterates one term's postings: ordinals in increasing order with their wdf. Satisfies
// docset::doc_iterator; scorers build on it. Skip groups are decoded on demand.
class postings_iterator {
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
		// pairs below; each group's level 0, each block, and the tail when decoded.
		need(p_, std::min<std::uint64_t>(64, info.postings_length));
		blocks_ = df_ / format::block;
		with_positions_ = !(info.flags & format::no_positions);
		with_counts_ = with_positions_ && !(info.flags & format::counts_are_wdf);
		if (blocks_) {
			const std::uint64_t groups = format::get_leb(&p_, end_);
			if (groups != (blocks_ + 31) / 32) { corrupt("skip group count"); }
			if (p_ == end_ || std::uint8_t(*p_) > 1) { corrupt("skip width"); }
			offset_width_ = *p_++ ? 8 : 4;
			if (with_positions_) {
				tail_first_position_ = format::get_leb(&p_, end_);
				tail_pos_offset_ = format::get_leb(&p_, end_);
			}
			// Fixed-width level 1, then the groups' pairs, level 0, blocks, and tail; each
			// region is checked against the term's range here, each group when decoded.
			const std::uint64_t pairs_bytes = format::get_leb(&p_, end_), level0_bytes = format::get_leb(&p_, end_),
								blocks_bytes = format::get_leb(&p_, end_);
			entry_size_ = 8 + offset_width_ * (with_positions_ ? 5 : 2);
			groups_n_ = groups;
			std::uint64_t range = std::uint64_t(end_ - p_);
			if (groups > range / entry_size_) { corrupt("skip groups bounds"); }
			level1_ = p_;
			p_ += groups * entry_size_;
			range = std::uint64_t(end_ - p_);
			if (pairs_bytes > range || level0_bytes > range - pairs_bytes || blocks_bytes > range - pairs_bytes - level0_bytes) {
				corrupt("skip data bounds");
			}
			need(level1_, std::uint64_t(p_ - level1_) + pairs_bytes);
			pairs_ = std::string_view(p_, pairs_bytes);
			level0_ = p_ + pairs_bytes;
			level0_bytes_ = level0_bytes;
			blocks_begin_ = level0_ + level0_bytes;
			tail_ = blocks_begin_ + blocks_bytes;
		} else {
			tail_ = p_;
		}
	}

	ordinal_t doc() const noexcept { return doc_; }
	std::uint32_t wdf() const { return wdfs()[pos_]; }
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
	// the tail or past the end).
	std::size_t block_index_for(ordinal_t target) {
		if (!blocks_) { return 0; }
		std::size_t gi = group_of(target);
		if (gi == groups_n_) { return blocks_; }
		decode_group(gi);
		auto e = std::lower_bound(entries_, entries_ + entries_n_, target, [](const entry& en, ordinal_t t) { return en.last < t; });
		return gi * 32 + std::size_t(e - entries_);
	}
	// The last ordinal of block b (no_more_docs for an empty tail).
	ordinal_t block_last(std::size_t b) {
		if (b >= blocks_) { return df_ % format::block ? tail_last() : no_more_docs; }
		return level0_entry(b).last;
	}
	// Competitive pairs; the span is valid until the next impacts call.
	// Whether any of block b's pairs satisfies pred, decoding them only until one does.
	template <class Pred>
	bool any_block_impact(std::size_t b, Pred&& pred) {
		if (b >= blocks_) { return std::any_of(info_.impacts.begin(), info_.impacts.end(), pred); }
		return format::any_sized_impact(level0_entry(b).impacts, pred);
	}

	std::span<const format::impact> block_impacts_at(std::size_t b) {
		if (b >= blocks_) { return info_.impacts; }
		impacts_.clear();
		format::any_sized_impact(level0_entry(b).impacts, [&](const format::impact& i) {
			impacts_.push_back(i);
			return false;
		});
		return impacts_;
	}
	std::size_t groups() const noexcept { return groups_n_; }
	// The group holding target, without decoding its blocks (groups() past the end). The
	// decoded group and the one after it are tried before a binary search.
	std::size_t group_of(ordinal_t target) const noexcept {
		const std::size_t n = groups_n_;
		std::size_t g = decoded_group_;
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
		const entry& e = level0_entry(next_block_ - 1);
		return {e.first_position, e.pos_offset, e.count_offset};
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
		for (; sum_pos_ < pos_; ++sum_pos_) {
			if (sum_ + counts[sum_pos_] < sum_) { corrupt("position index"); }  // wraps past 2^64
			sum_ += counts[sum_pos_];
		}
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
		if (!started_ && next() == no_more_docs) { return; }
		while (doc_ < end) {
			std::size_t i = pos_;
			ords();
			const std::size_t stop = buf_ords_[buf_len_ - 1] < end ? buf_len_ : std::size_t(std::lower_bound(buf_ords_ + pos_, buf_ords_ + buf_len_, end) - buf_ords_);
			for (; i < stop; ++i) {
				const ordinal_t d = buf_ords_[i] - base;
				bits[d >> 6] |= std::uint64_t(1) << (d & 63);
			}
			if (i < buf_len_) {
				pos_ = i;
				doc_ = buf_ords_[i];
				return;
			}
			// Whole bitset blocks inside the window are OR'd in without decoding.
			while (next_block_ < blocks_) {
				if (!level0_entry(next_block_).bitset || level0_entry(next_block_).last >= end) { break; }
				or_bitset(next_block_, base, bits);
				++next_block_;
			}
			pos_ = buf_len_ - 1;
			if (load_next_block() == no_more_docs) { return; }
		}
	}

	// ORs bitset block b into a window starting at base; the last word is masked to the
	// block's span, and the block must hold exactly 128 documents.
	void or_bitset(std::size_t b, ordinal_t base, std::uint64_t* bits) {
		const entry& e = level0_entry(b);
		const ordinal_t first = b ? e.base + 1 : 0;  // <= e.last: level-0 lasts increase
		if (first < base) { corrupt("bitset block span"); }
		const std::uint64_t span = std::uint64_t(e.last) - first + 1, bytes = (span + 7) / 8;
		const char* p = blocks_begin_ + e.offset;
		if (bytes > e.bytes) { corrupt("bitset block bounds"); }
		need(p, e.bytes);
		const std::uint64_t at = first - base;
		const unsigned shift = unsigned(at % 64);
		std::size_t n = 0;
		for (std::uint64_t w = 0; w * 8 < bytes; ++w) {
			std::uint64_t word = 0;
			std::memcpy(&word, p + w * 8, std::min<std::uint64_t>(8, bytes - w * 8));
			if ((w + 1) * 64 > span) { word &= span % 64 ? (std::uint64_t(1) << (span % 64)) - 1 : ~std::uint64_t(0); }
			n += std::size_t(std::popcount(word));
			const std::uint64_t k = at / 64 + w;
			bits[k] |= word << shift;
			if (shift && (word >> (64 - shift))) { bits[k + 1] |= word >> (64 - shift); }
		}
		if (n != format::block) { corrupt("bitset block count"); }
	}

private:
	std::span<const format::impact> decode(std::string_view list) {
		impacts_.clear();
		const char* p = list.data();
		format::decode_impacts(&p, list.data() + list.size(), impacts_);
		if (p != list.data() + list.size()) { corrupt("impacts length"); }
		return impacts_;
	}

	struct entry {
		ordinal_t last = 0;
		std::uint64_t offset = 0;  // from blocks_begin_
		std::uint64_t bytes = 0;
		std::uint64_t first_position = 0;  // index of the block's first position in the term's stream
		std::uint64_t pos_offset = 0;      // byte offset of the position block holding it
		std::uint64_t count_offset = 0;    // byte offset of the block's count block
		ordinal_t base = 0;                // the previous block's last ordinal
		bool bitset = false;               // ordinals as a bitset over (base, last]
		std::string_view impacts;          // encoded, decoded on demand
	};

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

	// A group's fixed level-1 fields, checked against its neighbors and the regions.
	struct group_fields {
		ordinal_t last, prev_last;
		std::uint64_t level0_begin, level0_end, blocks_begin, blocks_end;
		std::uint64_t first_position = 0, pos_offset = 0, count_offset = 0;
	};
	std::uint64_t offset_at(const char* e, std::size_t i) const noexcept {
		const char* at = e + 8 + offset_width_ * i;
		return offset_width_ == 8 ? format::get<std::uint64_t>(at) : format::get<std::uint32_t>(at);
	}
	group_fields read_group(std::size_t g) const {
		const char* e = level1_ + g * entry_size_;
		group_fields f{};
		f.last = format::get<std::uint32_t>(e);
		f.prev_last = g ? group_last(g - 1) : 0;
		f.level0_begin = offset_at(e, 0);
		f.blocks_begin = offset_at(e, 1);
		f.level0_end = g + 1 < groups_n_ ? offset_at(e + entry_size_, 0) : level0_bytes_;
		f.blocks_end = g + 1 < groups_n_ ? offset_at(e + entry_size_, 1) : std::uint64_t(tail_ - blocks_begin_);
		if (with_positions_) {
			f.first_position = offset_at(e, 2);
			f.pos_offset = offset_at(e, 3);
			f.count_offset = offset_at(e, 4);
		}
		if ((g && f.last <= f.prev_last) || f.last >= seg_->doc_count() || f.level0_begin > f.level0_end || f.level0_end > level0_bytes_ ||
			f.blocks_begin > f.blocks_end || f.blocks_end > std::uint64_t(tail_ - blocks_begin_)) {
			corrupt("skip group");
		}
		return f;
	}

	void decode_group(std::size_t g) {
		if (decoded_group_ == g) { return; }
		decoded_group_ = std::numeric_limits<std::size_t>::max();  // entries_ is partial until the end
		const group_fields gf = read_group(g);
		need(level0_ + gf.level0_begin, gf.level0_end - gf.level0_begin);
		const char* p = level0_ + gf.level0_begin;
		const char* end = level0_ + gf.level0_end;
		entries_n_ = 0;
		ordinal_t last = gf.prev_last;
		std::uint64_t offset = gf.blocks_begin;
		const std::uint64_t group_end = gf.blocks_end;
		// Level-0 deltas restart per group.
		std::uint64_t first_position = gf.first_position, pos_offset = gf.pos_offset, count_offset = gf.count_offset;
		const std::size_t n = std::min<std::size_t>(32, blocks_ - g * 32);
		for (std::size_t i = 0; i < n; ++i) {
			entry& e = entries_[entries_n_++];
			// Lasts increase within the group and end at its last; block bytes stay inside it.
			const std::uint64_t delta = format::get_leb(&p, end);
			if (((g || i) && delta == 0) || delta > std::uint64_t(gf.last) - last) { corrupt("block last ordinal"); }
			e.last = last + ordinal_t(delta);
			const std::uint64_t bytes = format::get_leb(&p, end);
			e.bytes = bytes >> 1;
			e.bitset = bytes & 1;
			e.base = last;  // the previous block's last ordinal (none before the first)
			if (with_positions_) {
				const std::uint64_t delta = format::get_leb(&p, end);
				if (first_position + delta < first_position) { corrupt("position index"); }
				first_position += delta;
				pos_offset += format::get_leb(&p, end);
				if (with_counts_) { count_offset += format::get_leb(&p, end); }
			}
			e.first_position = first_position;
			e.pos_offset = pos_offset;
			e.count_offset = count_offset;
			e.impacts = format::skip_sized_impacts(&p, end);
			e.offset = offset;
			if (e.bytes == 0 || e.bytes > group_end - offset) { corrupt("block bytes"); }
			offset += e.bytes;
			last = e.last;
		}
		if (offset != group_end) { corrupt("group bytes"); }
		if (p != end) { corrupt("level-0 bytes"); }
		if (entries_[entries_n_ - 1].last != gf.last) { corrupt("skip group last ordinal"); }
		decoded_group_ = g;
	}

	const entry& level0_entry(std::size_t b) {
		decode_group(b / 32);
		return entries_[b % 32];
	}

	void skip_to_block_containing(ordinal_t target) {
		// The next block holds it (targets only move forward past loaded blocks): no search.
		if (next_block_ < blocks_ && target <= level0_entry(next_block_).last) { return; }
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
			const entry& cur = level0_entry(next_block_);
			const char* p = blocks_begin_ + cur.offset;  // decode_group checked offset + bytes against the region
			need(p, cur.bytes);
			const ordinal_t base = next_block_ ? cur.base : std::numeric_limits<ordinal_t>::max();
			std::size_t used;
			if (cur.bitset) {
				used = load_bitset(p, cur);
			} else {
				used = bitpack::decode_delta(reinterpret_cast<const std::uint8_t*>(p), cur.bytes, base, buf_ords_);
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
			if (cur.bitset) { return doc_ = bits_doc0_; }
			if (buf_ords_[format::block - 1] != cur.last || buf_ords_[0] == no_more_docs) { corrupt("block last ordinal"); }
			check_increasing(base);
			return doc_ = buf_ords_[0];
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
	std::size_t load_bitset(const char* p, const entry& e) {
		const ordinal_t first = next_block_ ? e.base + 1 : 0;  // <= e.last: level-0 lasts increase
		const std::uint64_t span = std::uint64_t(e.last) - first + 1, bytes = (span + 7) / 8;
		if (bytes > e.bytes || bytes > sizeof bits_) { corrupt("bitset block bounds"); }
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
		bits_last_ = e.last;
		bits_pending_ = true;
		return std::size_t(bytes);
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

	// Ordinals strictly increase from base (none before the first block) and stay below
	// the document count; branch-free so it vectorizes.
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
	const char* level0_ = nullptr;
	const char* blocks_begin_ = nullptr;
	const char* tail_ = nullptr;
	std::size_t blocks_ = 0, next_block_ = 0;
	const char* level1_ = nullptr;  // fixed-width group entries
	std::size_t groups_n_ = 0, entry_size_ = 0, offset_width_ = 4;
	std::string_view pairs_;  // the groups' competitive pairs
	std::uint64_t level0_bytes_ = 0;
	entry entries_[32];
	std::size_t entries_n_ = 0;
	std::vector<format::impact> impacts_;
	std::size_t decoded_group_ = std::numeric_limits<std::size_t>::max();
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

class positions_reader {
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
		if (!live(o)) { continue; }
		digest_record rec('N');
		rec.u64(core.docid(o));
		rec.u32(r.length(o));
		emit(rec.data());
	}
	r.scan_terms([&](std::string_view term, const format::term_info& info) {
		postings_iterator it(r, info);
		positions_reader pr(r, info);
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
			if (!live(o)) { continue; }
			const auto positions = pr.read(it);
			digest_record rec('P');
			rec.u64(core.docid(o));
			rec.bytes(term);
			rec.u32(it.wdf());
			rec.u32(std::uint32_t(positions.size()));
			for (auto q : positions) { rec.u32(q); }
			emit(rec.data());
		}
		return true;
	});
}

template <class Emit>
void inverted_plugin::records(std::uint64_t docid, const part& p, Emit&& emit) {
	digest_record len('N');
	len.u64(docid);
	len.u32(p.length);
	emit(len.data());
	for (const auto& t : p.terms) {
		digest_record rec('P');
		rec.u64(docid);
		rec.bytes(t.term);
		rec.u32(t.wdf);
		rec.u32(std::uint32_t(t.positions.size()));
		for (auto q : t.positions) { rec.u32(q); }
		emit(rec.data());
	}
}

}  // namespace strata
