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

// The term-lists plugin (xapiand-on-strata P2; docs/mechanisms/term-lists.md, format-13 F17):
// each document's terms with their wdf, in term order, so a document's terms read back by
// ordinal and by prefix without the postings. Opt-in per index (`opt_in`). The lists are
// derived: the flush transposes the postings it writes, a merge copies the live documents'
// records, so the plugin has no document part and no digest records. This header holds the
// reader, the check, and the record format; term_lists_writer.hh the builder and the merge.
//
// The section is strata's frame layout (frame_builder), one record per ordinal. A record is
// empty for a document without terms; otherwise
//   [term count LEB, at least 1] then per term, in strictly increasing byte order:
//   [shared prefix with the previous term LEB][suffix length LEB, at least 1][suffix][wdf LEB]
// The first term shares nothing. Positions are not here: they stay in `positions`.
#pragma once

#include "prospect/strata.hh"
#include "strata/core/reader.hh"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace prospect {

class term_lists_builder;

// One document's terms, as a view of its record. The record is a view into a decoded frame:
// valid until the frame cache decodes another frame.
class term_list {
public:
	term_list() = default;
	explicit term_list(std::string_view record) : record_(record) {}

	bool empty() const noexcept { return record_.empty(); }
	// The number of terms; reads only the record's first bytes (a damaged record throws
	// corrupt_segment when read through a cursor, not here).
	std::uint64_t size() const {
		if (record_.empty()) { return 0; }
		const char* p = record_.data();
		return format::get_leb(&p, p + record_.size());
	}
	std::string_view bytes() const noexcept { return record_; }

	// Terms in increasing byte order with their wdf. A cursor starts before the first term;
	// next() moves to the next and returns false past the last. Damage in the record throws
	// corrupt_segment from next().
	class cursor {
	public:
		cursor() = default;
		// With a prefix, only terms that start with it: next() and skip_to() end at the first
		// term past them.
		explicit cursor(std::string_view record, std::string_view prefix = {}) : prefix_(prefix) {
			if (record.empty()) { return; }
			p_ = record.data();
			end_ = p_ + record.size();
			remaining_ = format::get_leb(&p_, end_);
			// Each term takes four bytes at least (shared, length, one suffix byte, wdf).
			if (remaining_ == 0 || remaining_ > std::uint64_t(end_ - p_) / 4) { corrupt("term list count"); }
		}
		// Terms not yet read.
		std::uint64_t remaining() const noexcept { return remaining_; }
		bool next() {
			while (step()) {
				if (term_ < prefix_) { continue; }
				return in_prefix();
			}
			return false;
		}
		// The first term at or after target (the current one if it is), false past the last
		// (or past the prefix). Moves forward only.
		bool skip_to(std::string_view target) {
			if (started_ && !done_ && term_ >= target && in_prefix()) { return true; }
			while (step()) {
				if (term_ < target || term_ < prefix_) { continue; }
				return in_prefix();
			}
			return false;
		}
		std::string_view term() const noexcept { return term_; }
		std::uint32_t wdf() const noexcept { return wdf_; }

	private:
		bool in_prefix() {
			if (term_.starts_with(prefix_)) { return true; }
			done_ = true;  // terms only grow: none after this one has the prefix
			return false;
		}
		// Decodes the next term into term_.
		bool step() {
			started_ = true;
			if (done_ || remaining_ == 0) {
				if (!done_ && p_ != end_) { corrupt("term list length"); }
				done_ = true;
				return false;
			}
			const std::uint64_t shared = format::get_leb(&p_, end_);
			const std::uint64_t suffix = format::get_leb(&p_, end_);
			const bool first = term_.empty();  // a term is never empty: the first shares nothing
			if (shared > term_.size() || suffix == 0 || suffix > std::uint64_t(end_ - p_) || shared + suffix > format::max_term_bytes) { corrupt("term list term"); }
			// Strictly increasing: a longer term with the previous as its prefix, or a larger byte
			// where they first differ.
			if (!first && shared < term_.size() && std::uint8_t(p_[0]) <= std::uint8_t(term_[shared])) { corrupt("term list order"); }
			term_.resize(std::size_t(shared));
			term_.append(p_, std::size_t(suffix));
			p_ += suffix;
			const std::uint64_t wdf = format::get_leb(&p_, end_);
			if (wdf > 0xFFFFFFFFull) { corrupt("term list wdf"); }
			wdf_ = std::uint32_t(wdf);
			--remaining_;
			return true;
		}
		const char* p_ = nullptr;
		const char* end_ = nullptr;
		std::uint64_t remaining_ = 0;
		std::string term_;
		std::string_view prefix_;
		std::uint32_t wdf_ = 0;
		bool started_ = false, done_ = false;
	};
	cursor begin() const { return cursor(record_); }
	// The terms that start with prefix (the view must outlive the cursor).
	cursor prefixed(std::string_view prefix) const { return cursor(record_, prefix); }

private:
	std::string_view record_;
};

struct term_lists_plugin {
	static constexpr std::string_view id = "term_lists";
	static constexpr format::owner owner = format::owner::term_lists;
	static constexpr format::structure_bits structure = format::bit::term_lists;
	static constexpr format::structure_bits structures = format::bit::term_lists;
	static constexpr bool opt_in = true;  // an index builds it only when asked (F17)
	using builder = term_lists_builder;

	class reader : public detail::frame_reader {
	public:
		using frame = detail::frame_reader::frame;
		explicit reader(const open_context& c) : frame_reader(c.section(format::section::term_lists), c.doc_count), doc_count_(c.doc_count), present_(c.present) {}

		// Whether this segment holds term lists (its index builds them).
		bool present() const noexcept { return present_; }
		// The document's terms, through `cache`: a reader in ordinal order decodes each frame
		// once. The view lives until the cache's next decode. Throws std::logic_error in a
		// segment without term lists and std::out_of_range past its documents.
		term_list list(ordinal_t ord, frame& cache) const {
			if (!present_) { throw std::logic_error("strata: term lists: the segment has none"); }
			return term_list(record(ord, cache));
		}
		std::uint64_t doc_count() const noexcept { return doc_count_; }

	private:
		std::uint64_t doc_count_;
		bool present_;
	};

	// L6: rebuilds the plugin's section for a merge's output (term_lists_writer.hh).
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink);

	// What a check counts (check.hh): every document's list is read whole.
	struct report {
		std::uint64_t listed_documents = 0, listed_terms = 0, list_bytes = 0;
	};
	// Every frame decodes, the records cover the ordinals exactly, and every list is
	// well-formed (the cursor's checks). Cross-checking the lists against the postings needs
	// the inverted reader: term_lists_check.hh.
	static void check(const reader& r, const core_segment& core, report& rep, std::string_view) {
		if (!r.present()) { return; }
		reader::frame frame;
		const std::uint32_t frames = r.frames();
		ordinal_t next = 0;
		for (std::uint32_t f = 0; f < frames; ++f) {
			r.decode(f, frame);
			if (frame.first != next) { corrupt("term list frames"); }
			for (ordinal_t o = frame.first; o < frame.first + frame.count; ++o) {
				const term_list l(frame.record(o));
				rep.list_bytes += l.bytes().size();
				std::uint64_t n = 0;
				for (auto c = l.begin(); c.next();) { ++n; }
				if (n != l.size()) { corrupt("term list count"); }
				rep.listed_terms += n;
				++rep.listed_documents;
			}
			next = frame.first + frame.count;
		}
		if (next != core.doc_count()) { corrupt("term list documents"); }
	}
};

}  // namespace prospect
