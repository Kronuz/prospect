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

// The term-lists plugin's side of a segment's writing (term_lists.hh holds the format): the
// builder, which transposes the postings the inverted builder writes, and the merge, which
// copies the live documents' records.
#pragma once

#include "prospect/strata.hh"
#include "prospect/term_lists.hh"
#include "strata/core/merge.hh"
#include "strata/core/writer.hh"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prospect {

class inverted_builder;

// A segment's term lists. The inverted builder reports each term it writes, in order, with its
// postings (observe); the lists are the transposition, one record per document, encoded at
// finish() once every term is in. Working memory is 16 bytes per posting plus the distinct
// terms, charged to the flush; the records are held encoded until the section is written.
class term_lists_builder {
public:
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;

	void configure(const builder_settings& s) {
		frames_.configure(s);
		memory = s.memory;
		cancel = s.cancel;
		enabled_ = (s.structures & format::bit::term_lists) != 0;
	}
	// Hands the writer's builders over (strata's link hook): the inverted builder reports its
	// terms here when the segment builds term lists.
	template <class Builders>
	void link(Builders& all) noexcept;  // writer.hh, once inverted_builder is complete

	// The next term in strictly increasing order, with the ordinals (ascending) and wdfs of
	// its postings.
	void observe(std::string_view term, std::span<const std::uint32_t> ords, std::span<const std::uint32_t> wdfs) {
		if (ords.size() != wdfs.size() || term.empty() || term.size() > format::max_term_bytes || ids_ >= 0xFFFFFFFFull) {
			throw std::invalid_argument("strata: term lists: a term's postings");
		}
		term_ends_.push_back(term_bytes_.size() + term.size());
		term_bytes_.append(term);
		const std::uint32_t id = std::uint32_t(ids_++);
		const std::uint64_t docs = this->docs();
		for (std::size_t i = 0; i < ords.size(); ++i) {
			if (ords[i] >= docs) { throw std::invalid_argument("strata: term lists: a posting past the documents"); }
			ords_.push_back(ords[i]);
			ids_of_.push_back(id);
			wdfs_.push_back(wdfs[i]);
		}
		detail::charge_to(memory, (ords_.capacity() + ids_of_.capacity() + wdfs_.capacity()) * sizeof(std::uint32_t) + term_bytes_.capacity() + term_ends_.capacity() * sizeof(std::uint64_t), charged_);
	}

	void finish(section_sink& sink) {
		// Counting sort of the postings by ordinal, stable: each document's entries stay in
		// term order.
		const std::uint64_t docs = this->docs();
		std::vector<std::uint32_t> start(docs + 1, 0);
		for (const std::uint32_t o : ords_) { ++start[std::size_t(o) + 1]; }
		for (std::size_t d = 0; d < docs; ++d) { start[d + 1] += start[d]; }
		std::vector<std::uint32_t> place(ords_.size());
		{
			std::vector<std::uint32_t> at(start.begin(), start.end() - 1);
			for (std::size_t i = 0; i < ords_.size(); ++i) { place[at[ords_[i]]++] = std::uint32_t(i); }
		}
		detail::charge_to(memory, (ords_.capacity() + ids_of_.capacity() + wdfs_.capacity() + place.capacity() + 2 * start.capacity()) * sizeof(std::uint32_t) + term_bytes_.capacity() +
			term_ends_.capacity() * sizeof(std::uint64_t), charged_);
		std::string record;
		for (std::size_t d = 0; d < docs; ++d) {
			if ((d & 1023) == 0 && cancel && cancel->requested()) { throw task::cancelled{}; }
			record.clear();
			const std::uint32_t n = start[d + 1] - start[d];
			if (n) {
				format::put_leb(record, n);
				std::string_view previous;
				for (std::uint32_t k = start[d]; k < start[d + 1]; ++k) {
					const std::uint32_t i = place[k];
					const std::string_view term = term_of(ids_of_[i]);
					std::size_t shared = 0;
					while (shared < previous.size() && shared < term.size() && previous[shared] == term[shared]) { ++shared; }
					format::put_leb(record, shared);
					format::put_leb(record, term.size() - shared);
					record.append(term, shared, std::string_view::npos);
					format::put_leb(record, wdfs_[i]);
					previous = term;
				}
			}
			if (record.size() > format::max_stored_record) { throw std::invalid_argument("strata: term list too large"); }
			frames_.add(record);
		}
		frames_.finish(sink, format::section::term_lists);
	}

private:
	std::string_view term_of(std::uint32_t id) const noexcept {
		const std::uint64_t begin = id ? term_ends_[id - 1] : 0;
		return std::string_view(term_bytes_).substr(begin, term_ends_[id] - begin);
	}
	frame_builder frames_;
	std::uint64_t docs() const;  // the segment's documents, from the inverted builder
	inverted_builder* inverted_ = nullptr;
	bool enabled_ = false;
	std::uint64_t ids_ = 0, charged_ = 0;
	std::string term_bytes_;                  // the distinct terms, in order
	std::vector<std::uint64_t> term_ends_;    // [term id]: its end in term_bytes_
	std::vector<std::uint32_t> ords_, ids_of_, wdfs_;  // per posting, in term order
};

// A merge copies each live document's record in output order, which strata's frame merge
// does for every plugin of this layout; the inputs must all hold lists.
inline built term_lists_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	for (const reader* r : in) {
		if (!r->present()) { throw std::invalid_argument("strata: term lists: a merge input without them"); }
	}
	detail::merge_frames(m, in, sink, format::section::term_lists);
	return {format::bit::term_lists};
}

}  // namespace prospect
