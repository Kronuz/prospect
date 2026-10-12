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
#include "residency.hh"
#include "docset.hh"
#include "strata/format.hh"
#include "termdict.hh"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

using docset::no_more_docs;

class segment {
public:
	// Verifies the footer, the table of contents, the header, and the pinned sections
	// (term index, lengths, docids, stats) in full; other sections are verified per region
	// as they are read.
	explicit segment(std::string_view file) : file_(file) {
		footer_ = format::decode_footer(file);
		sections_ = format::decode_toc(file, footer_);
		if (sections_.empty() || sections_[0].kind != format::section::header || sections_[0].offset != 0 ||
			sections_[0].length != format::header_size) {
			throw corrupt_segment("header entry");
		}
		if (std::memcmp(file.data(), format::header_magic, 8) != 0 || format::get<std::uint32_t>(file.data() + 8) != format::version) {
			throw corrupt_segment("header magic or version");
		}
		for (const auto& e : sections_) {
			auto bytes = file.substr(e.offset, e.length);
			switch (e.kind) {
			case format::section::header:
			case format::section::term_index:
			case format::section::lengths:
			case format::section::norms:
			case format::section::docids:
			case format::section::stats:
				if (hashes::crc32c(bytes.data(), bytes.size()) != e.crc) { throw corrupt_segment("section checksum"); }
				break;
			default:
				break;
			}
			switch (e.kind) {
			case format::section::terms: terms_ = bytes; break;
			case format::section::term_index: term_index_ = bytes; break;
			case format::section::postings: postings_ = bytes; break;
			case format::section::positions: positions_ = bytes; break;
			case format::section::lengths: lengths_ = bytes; break;
			case format::section::docids: docids_ = bytes; break;
			case format::section::stats: stats_ = bytes; break;
			case format::section::stored: stored_ = bytes; break;
			case format::section::norms: norms_ = bytes; break;
			default: break;
			}
		}
		if (stats_.size() != 28) { throw corrupt_segment("stats"); }
		max_positions_ = format::get<std::uint32_t>(stats_.data() + 24);
		doc_count_ = format::get<std::uint64_t>(stats_.data());
		total_length_ = format::get<std::uint64_t>(stats_.data() + 8);
		min_length_ = format::get<std::uint32_t>(stats_.data() + 16);
		max_length_ = format::get<std::uint32_t>(stats_.data() + 20);
		if (doc_count_ != footer_.doc_count) { throw corrupt_segment("document count"); }
		if (lengths_.empty()) { throw corrupt_segment("lengths"); }
		length_width_ = std::uint8_t(lengths_[0]);
		if (length_width_ > 32 || lengths_.size() != 1 + (doc_count_ + 127) / 128 * bitpack::packed_bytes(length_width_)) {
			throw corrupt_segment("lengths size");
		}
		// Terms, postings, and positions are verified per 64 KiB region on first read.
		std::vector<residency::verified_regions::region> regions;
		for (const auto& e : sections_) {
			if (e.kind != format::section::terms && e.kind != format::section::postings && e.kind != format::section::positions &&
				e.kind != format::section::stored) {
				continue;
			}
			for (std::size_t r = 0; r < e.region_crcs.size(); ++r) {
				const std::uint64_t at = std::uint64_t(r) * format::region_size;
				regions.push_back({e.offset + at, std::min<std::uint64_t>(format::region_size, e.length - at), e.region_crcs[r]});
			}
			if (e.kind == format::section::terms) { integrity_->terms_offset = e.offset; }
			if (e.kind == format::section::postings) { integrity_->postings_offset = e.offset; }
			if (e.kind == format::section::positions) { integrity_->positions_offset = e.offset; }
			if (e.kind == format::section::stored) { integrity_->stored_offset = e.offset; }
		}
		std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
		integrity_->regions = residency::verified_regions(
			std::span<const std::byte>(reinterpret_cast<const std::byte*>(file.data()), file.size()), std::move(regions));
		if (norms_.size() != doc_count_) { throw corrupt_segment("norms size"); }
		if (docids_.empty() || (docids_[0] == 0 ? docids_.size() != 9
		                                        : docids_[0] != 1 || docids_.size() != 9 + 8 * doc_count_ ||
		                                              format::get<std::uint64_t>(docids_.data() + 1) != doc_count_)) {
			throw corrupt_segment("docids");
		}
		// The term index is copied out of the mapping (verified above), so later lookups never
		// read bytes that could change or fault under the FST's unchecked traversal.
		integrity_->pinned_index.assign(term_index_);
		dict_.emplace(terms_, integrity_->pinned_index, &verify_terms, integrity_.get());  // validates the FST index
	}

	// The host docid of an ordinal.
	std::uint64_t docid(ordinal_t ord) const {
		if (ord >= doc_count_) { throw std::out_of_range("strata: ordinal out of range"); }
		if (docids_[0] == 0) { return format::get<std::uint64_t>(docids_.data() + 1) + ord; }
		return format::get<std::uint64_t>(docids_.data() + 9 + 8 * std::uint64_t(ord));
	}

	// The document's length quantized by format::encode_length (one byte, verified at open).
	std::uint8_t norm(ordinal_t ord) const noexcept { return std::uint8_t(norms_[ord]); }

	// Verifies a range of the postings or positions section before it is read; throws
	// corrupt_segment on a checksum mismatch.
	void verify_postings(std::uint64_t offset, std::uint64_t length) const {
		if (!integrity_->regions.verify({integrity_->postings_offset + offset, length})) { throw corrupt_segment("postings checksum"); }
	}
	void verify_positions(std::uint64_t offset, std::uint64_t length) const {
		if (!integrity_->regions.verify({integrity_->positions_offset + offset, length})) { throw corrupt_segment("positions checksum"); }
	}
	const std::vector<format::section_entry>& sections() const noexcept { return sections_; }

	// The document's stored record (empty when it has none). Each zstd frame's range is
	// verified before it is decompressed, into exactly its recorded size.
	std::string stored(ordinal_t ord) const {
		if (ord >= doc_count_) { throw std::out_of_range("strata: ordinal out of range"); }
		if (stored_.size() < 4) { return {}; }
		const std::uint32_t frames = format::get<std::uint32_t>(stored_.data() + stored_.size() - 4);
		if (std::uint64_t(frames) * 16 > stored_.size() - 4) { throw corrupt_segment("stored index"); }
		const std::uint64_t index_at = stored_.size() - 4 - std::uint64_t(frames) * 16;
		if (!integrity_->regions.verify({integrity_->stored_offset + index_at, stored_.size() - index_at})) { throw corrupt_segment("stored checksum"); }
		auto first_of = [&](std::uint32_t f) { return format::get<std::uint32_t>(stored_.data() + index_at + 16 * std::uint64_t(f)); };
		std::uint32_t lo = 0, hi = frames;  // the last frame whose first ordinal <= ord
		while (hi - lo > 1) {
			const std::uint32_t mid = lo + (hi - lo) / 2;
			if (first_of(mid) <= ord) { lo = mid; } else { hi = mid; }
		}
		if (frames == 0 || first_of(lo) > ord) { throw corrupt_segment("stored index"); }
		const char* e = stored_.data() + index_at + 16 * std::uint64_t(lo);
		const std::uint32_t raw_size = format::get<std::uint32_t>(e + 4);
		const std::uint64_t begin = format::get<std::uint64_t>(e + 8);
		const std::uint64_t end = lo + 1 < frames ? format::get<std::uint64_t>(e + 24) : index_at;
		if (begin > end || end > index_at) { throw corrupt_segment("stored frame bounds"); }
		if (!integrity_->regions.verify({integrity_->stored_offset + begin, end - begin})) { throw corrupt_segment("stored checksum"); }
		std::string raw;
		try {
			raw = decompress_zstd(stored_.substr(begin, end - begin), raw_size);
		} catch (const std::exception&) {
			throw corrupt_segment("stored frame");
		}
		const char* p = raw.data();
		const char* rend = p + raw.size();
		const std::uint64_t n = format::get_leb(&p, rend);
		if (ord - first_of(lo) >= n) { throw corrupt_segment("stored frame count"); }
		std::uint64_t skip = 0, length = 0;
		for (std::uint64_t i = 0; i < n; ++i) {
			const std::uint64_t len = format::get_leb(&p, rend);
			if (len > raw.size()) { throw corrupt_segment("stored record length"); }
			if (i < ord - first_of(lo)) { skip += len; }
			if (i == ord - first_of(lo)) { length = len; }
		}
		if (skip > std::uint64_t(rend - p) || length > std::uint64_t(rend - p) - skip) { throw corrupt_segment("stored record bounds"); }
		return std::string(p + skip, length);
	}
	// How many 64 KiB regions have been verified.
	std::size_t verified_regions() const noexcept { return integrity_->regions.verified(); }

	std::uint64_t doc_count() const noexcept { return doc_count_; }
	// The most positions any document has for one term.
	std::uint32_t max_positions() const noexcept { return max_positions_; }
	std::uint64_t total_length() const noexcept { return total_length_; }
	double average_length() const noexcept { return doc_count_ ? double(total_length_) / double(doc_count_) : 0.0; }
	std::uint32_t min_length() const noexcept { return min_length_; }
	std::uint32_t max_length() const noexcept { return max_length_; }

	// Document length by ordinal: one lane extraction from the bit-packed block.
	std::uint32_t length(ordinal_t ord) const noexcept {
		const unsigned w = length_width_;
		if (w == 0) { return 0; }
		const char* block = lengths_.data() + 1 + std::size_t(ord / 128) * bitpack::packed_bytes(w);
		const unsigned j = ord % 128, lane = j & 3, row = j >> 2;
		const unsigned start = row * w, word = start / 32, shift = start % 32;
		std::uint64_t v = format::get<std::uint32_t>(block + 4 * (4 * word + lane)) >> shift;
		if (shift + w > 32) { v |= std::uint64_t(format::get<std::uint32_t>(block + 4 * (4 * (word + 1) + lane))) << (32 - shift); }
		return std::uint32_t(v & (w == 32 ? 0xffffffffu : ((1u << w) - 1)));
	}

	std::optional<format::term_info> term(std::string_view key) const {
		auto v = dict_->find(key);
		if (!v) { return std::nullopt; }
		auto info = format::decode_term_info(*v);
		if (info.df > doc_count_) { throw corrupt_segment("document frequency"); }
		return info;
	}

	std::string_view postings_bytes() const noexcept { return postings_; }
	std::string_view positions_bytes() const noexcept { return positions_; }
	const termdict::reader& dictionary() const noexcept { return *dict_; }

private:
	std::string_view file_, terms_, term_index_, postings_, positions_, lengths_, docids_, stats_, stored_, norms_;
	format::footer footer_;
	std::vector<format::section_entry> sections_;
	struct integrity {
		residency::verified_regions regions;
		std::uint64_t terms_offset = 0, postings_offset = 0, positions_offset = 0, stored_offset = 0;
		std::string pinned_index;  // the term index, copied out of the mapping
	};
	static bool verify_terms(void* context, std::uint64_t offset, std::uint64_t length) {
		auto* i = static_cast<integrity*>(context);
		return i->regions.verify({i->terms_offset + offset, length});
	}
	std::unique_ptr<integrity> integrity_ = std::make_unique<integrity>();  // stable address for dict_
	std::optional<termdict::reader> dict_;
	std::uint64_t doc_count_ = 0, total_length_ = 0;
	std::uint32_t min_length_ = 0, max_length_ = 0, max_positions_ = 0;
	std::uint8_t length_width_ = 0;
};

// Iterates one term's postings: ordinals in increasing order with their wdf. Satisfies
// docset::doc_iterator; scorers build on it. Skip groups are decoded on demand.
class postings_iterator {
public:
	postings_iterator(const segment& seg, const format::term_info& info) : seg_(&seg), info_(info), df_(info.df) {
		if (info.flags & format::single_posting) {
			if (info.postings >= seg.doc_count()) { throw corrupt_segment("single posting ordinal"); }
			buf_ords_[0] = ordinal_t(info.postings);
			buf_wdfs_[0] = info.single_wdf;
			buf_len_ = 1;
			blocks_ = 0;
			return;
		}
		auto bytes = seg.postings_bytes();
		if (info.postings > bytes.size() || info.postings_length > bytes.size() - info.postings) { throw corrupt_segment("postings offset"); }
		seg.verify_postings(info.postings, info.postings_length);
		p_ = bytes.data() + info.postings;
		end_ = p_ + info.postings_length;  // everything read stays inside the verified range
		blocks_ = df_ / format::block;
		with_positions_ = !(info.flags & format::no_positions);
		with_counts_ = with_positions_ && !(info.flags & format::counts_are_wdf);
		if (blocks_) {
			const std::uint64_t groups = format::get_leb(&p_, end_);
			if (groups != (blocks_ + 31) / 32) { throw corrupt_segment("skip group count"); }
			if (with_positions_) {
				tail_first_position_ = format::get_leb(&p_, end_);
				tail_pos_offset_ = format::get_leb(&p_, end_);
			}
			groups_.resize(groups);
			ordinal_t last = 0;
			std::uint64_t level0 = 0, block_bytes = 0, first_position = 0, pos_offset = 0, count_offset = 0;
			for (std::size_t g = 0; g < groups; ++g) {
				auto& gr = groups_[g];
				const std::uint64_t delta = format::get_leb(&p_, end_);
				if ((g && delta == 0) || delta >= seg.doc_count() - last) { throw corrupt_segment("skip group last ordinal"); }
				last += ordinal_t(delta);
				gr.last = last;
				if (with_positions_) {
					first_position += format::get_leb(&p_, end_);
					pos_offset += format::get_leb(&p_, end_);
					if (with_counts_) { count_offset += format::get_leb(&p_, end_); }
				}
				gr.first_position = first_position;
				gr.pos_offset = pos_offset;
				gr.count_offset = count_offset;
				gr.level0_offset = level0;
				gr.blocks_offset = block_bytes;
				level0 += format::get_leb(&p_, end_);
				block_bytes += format::get_leb(&p_, end_);
				gr.impacts = format::skip_sized_impacts(&p_, end_);
			}
			level0_ = p_;
			if (level0 > std::uint64_t(end_ - p_)) { throw corrupt_segment("level 0 bounds"); }
			blocks_begin_ = p_ + level0;
			if (block_bytes > std::uint64_t(end_ - blocks_begin_)) { throw corrupt_segment("blocks bounds"); }
			tail_ = blocks_begin_ + block_bytes;
		} else {
			tail_ = p_;
		}
	}

	ordinal_t doc() const noexcept { return doc_; }
	std::uint32_t wdf() const { return wdfs()[pos_]; }
	std::uint64_t cost() const noexcept { return df_; }

	ordinal_t next() {
		if (doc_ == no_more_docs) { return doc_; }
		if (started_ && ++pos_ < buf_len_) { return doc_ = buf_ords_[pos_]; }
		started_ = true;
		return load_next_block();
	}

	ordinal_t advance(ordinal_t target) {
		if (started_ && doc_ != no_more_docs && doc_ >= target) { return doc_; }
		// Inside the decoded block: search it without touching skip data.
		if (started_ && buf_len_ && target <= buf_ords_[buf_len_ - 1]) {
			pos_ += count_below(buf_ords_ + pos_, buf_len_ - pos_, target);
			return doc_ = buf_ords_[pos_];
		}
		// Skip whole blocks using skip data before decoding.
		if (blocks_) { skip_to_block_containing(target); }
		while (true) {
			if (!started_ || pos_ + 1 >= buf_len_) {
				started_ = true;
				if (load_next_block() == no_more_docs) { return doc_; }
			} else {
				++pos_;
				doc_ = buf_ords_[pos_];
			}
			// Search inside the decoded block.
			if (doc_ < target) {
				pos_ += count_below(buf_ords_ + pos_, buf_len_ - pos_, target);
				if (pos_ < buf_len_) { return doc_ = buf_ords_[pos_]; }
				pos_ = buf_len_ - 1;
				continue;
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
		if (gi == groups_.size()) { return blocks_; }
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
		const std::string_view list = level0_entry(b).impacts;
		const char* p = list.data();
		const char* end = p + list.size();
		const std::uint64_t n = format::get_leb(&p, end);
		if (n == 0 || n > 256) { throw corrupt_segment("impact count"); }
		for (std::uint64_t k = 0; k < n; ++k) {
			format::impact i;
			i.wdf = std::uint32_t(format::get_leb(&p, end));
			if (p == end) { throw corrupt_segment("impacts truncated"); }
			i.length = std::uint8_t(*p++);
			if (pred(i)) { return true; }
		}
		return false;
	}

	std::span<const format::impact> block_impacts_at(std::size_t b) {
		if (b >= blocks_) { return info_.impacts; }
		return decode(level0_entry(b).impacts);
	}
	std::size_t groups() const noexcept { return groups_.size(); }
	// The group holding target, without decoding its blocks (groups() past the end). The
	// decoded group and the one after it are tried before a binary search.
	std::size_t group_of(ordinal_t target) const noexcept {
		const std::size_t n = groups_.size();
		std::size_t g = decoded_group_;
		if (g < n) {
			if (target <= groups_[g].last && (g == 0 || target > groups_[g - 1].last)) { return g; }
			if (g + 1 < n && target > groups_[g].last && target <= groups_[g + 1].last) { return g + 1; }
		}
		return std::size_t(std::lower_bound(groups_.begin(), groups_.end(), target, [](const group& gr, ordinal_t t) { return gr.last < t; }) - groups_.begin());
	}
	// The first ordinal a group can hold.
	ordinal_t group_first(std::size_t g) const noexcept { return g ? groups_[g - 1].last + 1 : 0; }
	ordinal_t group_last(std::size_t g) const noexcept { return groups_[g].last; }
	std::span<const format::impact> group_impacts(std::size_t g) { return decode(groups_[g].impacts); }

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
	std::size_t tail_count() const noexcept { return df_ % format::block; }

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
		for (; sum_pos_ < pos_; ++sum_pos_) { sum_ += counts[sum_pos_]; }
		return sum_;
	}

	// The decoded block's wdfs, decoded on first use (counting never needs them).
	const std::uint32_t* wdfs() const {
		if (wdf_pending_) {
			if (!bitpack::decode(reinterpret_cast<const std::uint8_t*>(wdf_pending_), wdf_size_, buf_wdfs_)) { throw corrupt_segment("wdf block"); }
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
			const std::uint32_t* w = wdfs();
			for (; i < buf_len_ && buf_ords_[i] < end; ++i) { f(buf_ords_[i], w[i]); }
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
		const ordinal_t first = b ? e.base + 1 : 0;
		if (e.last < first || first < base) { throw corrupt_segment("bitset block span"); }
		const std::uint64_t span = std::uint64_t(e.last) - first + 1, bytes = (span + 7) / 8;
		const char* p = blocks_begin_ + e.offset;
		if (bytes > e.bytes) { throw corrupt_segment("bitset block bounds"); }
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
		if (n != format::block) { throw corrupt_segment("bitset block count"); }
	}

private:
	std::span<const format::impact> decode(std::string_view list) {
		impacts_.clear();
		const char* p = list.data();
		format::decode_impacts(&p, list.data() + list.size(), impacts_);
		if (p != list.data() + list.size()) { throw corrupt_segment("impacts length"); }
		return impacts_;
	}

	struct group {
		ordinal_t last = 0;
		std::uint64_t first_position = 0, pos_offset = 0, count_offset = 0;
		std::uint64_t level0_offset = 0, blocks_offset = 0;
		std::string_view impacts;  // encoded, decoded on demand
	};
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
				tail_last_ = blocks_ ? groups_.back().last : no_more_docs;
			} else {
				const char* p = tail_;
				ordinal_t prev = blocks_ ? groups_.back().last : std::numeric_limits<ordinal_t>::max();
				for (std::size_t i = 0; i < df_ % format::block; ++i) { prev += ordinal_t(format::get_leb(&p, end_)); }
				tail_last_ = prev;
			}
		}
		return tail_last_;
	}

	void decode_group(std::size_t g) {
		if (decoded_group_ == g) { return; }
		const char* p = level0_ + groups_[g].level0_offset;
		const char* end = (g + 1 < groups_.size()) ? level0_ + groups_[g + 1].level0_offset : blocks_begin_;
		entries_n_ = 0;
		ordinal_t last = g ? groups_[g - 1].last : 0;
		std::uint64_t offset = groups_[g].blocks_offset;
		const std::uint64_t group_end = g + 1 < groups_.size() ? groups_[g + 1].blocks_offset : std::uint64_t(tail_ - blocks_begin_);
		if (group_end < offset) { throw corrupt_segment("group bytes"); }
		// Level-0 deltas restart per group.
		std::uint64_t first_position = groups_[g].first_position, pos_offset = groups_[g].pos_offset, count_offset = groups_[g].count_offset;
		const std::size_t n = std::min<std::size_t>(32, blocks_ - g * 32);
		for (std::size_t i = 0; i < n; ++i) {
			entry& e = entries_[entries_n_++];
			// Lasts increase within the group and end at its last; block bytes stay inside it.
			const std::uint64_t delta = format::get_leb(&p, end);
			if (((g || i) && delta == 0) || delta > std::uint64_t(groups_[g].last) - last) { throw corrupt_segment("block last ordinal"); }
			e.last = last + ordinal_t(delta);
			const std::uint64_t bytes = format::get_leb(&p, end);
			e.bytes = bytes >> 1;
			e.bitset = bytes & 1;
			e.base = last;  // the previous block's last ordinal (none before the first)
			if (with_positions_) {
				first_position += format::get_leb(&p, end);
				pos_offset += format::get_leb(&p, end);
				if (with_counts_) { count_offset += format::get_leb(&p, end); }
			}
			e.first_position = first_position;
			e.pos_offset = pos_offset;
			e.count_offset = count_offset;
			e.impacts = format::skip_sized_impacts(&p, end);
			e.offset = offset;
			if (e.bytes == 0 || e.bytes > group_end - offset) { throw corrupt_segment("block bytes"); }
			offset += e.bytes;
			last = e.last;
		}
		if (offset != group_end) { throw corrupt_segment("group bytes"); }
		if (entries_[entries_n_ - 1].last != groups_[g].last || groups_[g].last >= seg_->doc_count()) { throw corrupt_segment("skip group last ordinal"); }
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
		}
	}

	ordinal_t load_next_block() {
		pos_ = 0;
		if (next_block_ < blocks_) {
			const entry& cur = level0_entry(next_block_);
			const char* p = blocks_begin_ + cur.offset;  // decode_group checked offset + bytes against the region
			const ordinal_t base = next_block_ ? cur.base : std::numeric_limits<ordinal_t>::max();
			std::size_t used;
			if (cur.bitset) {
				used = decode_bitset(p, cur);
			} else {
				used = bitpack::decode_delta(reinterpret_cast<const std::uint8_t*>(p), cur.bytes, base, buf_ords_);
				if (!used) { throw corrupt_segment("ordinal block"); }
			}
			if (info_.flags & format::no_wdf) {
				std::fill(buf_wdfs_, buf_wdfs_ + format::block, 0u);
				wdf_pending_ = nullptr;
			} else {
				if (used >= cur.bytes) { throw corrupt_segment("wdf block"); }
				wdf_pending_ = p + used;
				wdf_size_ = cur.bytes - used;
			}
			if (buf_ords_[format::block - 1] != cur.last || buf_ords_[0] == no_more_docs) { throw corrupt_segment("block last ordinal"); }
			if (!cur.bitset) { check_increasing(base); }  // bitsets increase by construction
			buf_len_ = format::block;
			++next_block_;
			return doc_ = buf_ords_[0];
		}
		if (next_block_ == blocks_ && !tail_done_ && df_ % format::block) {
			if (info_.flags & format::single_posting) {
				tail_done_ = true;
				buf_len_ = 1;
				return doc_ = buf_ords_[0];
			}
			const char* p = tail_;
			const std::size_t n = df_ % format::block;
			ordinal_t prev = blocks_ ? groups_.back().last : std::numeric_limits<ordinal_t>::max();
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

	// Expands a bitset block into buf_ords_; returns its bytes. Exactly 128 bits must be set,
	// none past the block's last ordinal.
	std::size_t decode_bitset(const char* p, const entry& e) {
		const ordinal_t first = next_block_ ? e.base + 1 : 0;
		if (e.last < first) { throw corrupt_segment("bitset block span"); }
		const std::uint64_t span = std::uint64_t(e.last) - first + 1, bytes = (span + 7) / 8;
		if (bytes > e.bytes) { throw corrupt_segment("bitset block bounds"); }
		std::size_t n = 0;
		for (std::uint64_t w = 0; w * 8 < bytes; ++w) {
			std::uint64_t word = 0;
			std::memcpy(&word, p + w * 8, std::min<std::uint64_t>(8, bytes - w * 8));
			if ((w + 1) * 64 > span) { word &= span % 64 ? (std::uint64_t(1) << (span % 64)) - 1 : ~std::uint64_t(0); }
			if (n + std::size_t(std::popcount(word)) > format::block) { throw corrupt_segment("bitset block count"); }
			for (; word; word &= word - 1) { buf_ords_[n++] = first + ordinal_t(w * 64 + std::size_t(std::countr_zero(word))); }
		}
		if (n != format::block) { throw corrupt_segment("bitset block count"); }
		return std::size_t(bytes);
	}

	// How many of n sorted values are below target: a few linear probes (dense lists'
	// targets are usually close), then a binary search.
	static std::size_t count_below(const ordinal_t* v, std::size_t n, ordinal_t target) noexcept {
		std::size_t i = 0;
		for (const std::size_t probe = std::min<std::size_t>(n, 4); i < probe; ++i) {
			if (v[i] >= target) { return i; }
		}
		return std::size_t(std::lower_bound(v + i, v + n, target) - v);
	}

	// Ordinals strictly increase from base (none before the first block) and stay below
	// the document count; branch-free so it vectorizes.
	void check_increasing(ordinal_t base) const {
		const std::size_t n = buf_len_ ? buf_len_ : std::min<std::size_t>(format::block, df_);
		if (n == 0) { return; }
		std::uint32_t bad = base != std::numeric_limits<ordinal_t>::max() && buf_ords_[0] <= base;
		for (std::size_t i = 1; i < n; ++i) { bad |= buf_ords_[i] <= buf_ords_[i - 1]; }
		if (bad) { throw corrupt_segment("ordinals not increasing"); }
		if (buf_ords_[n - 1] >= seg_->doc_count()) { throw corrupt_segment("ordinal out of range"); }
	}

	const segment* seg_;
	format::term_info info_;
	std::uint32_t df_;
	const char* p_ = nullptr;
	const char* end_ = nullptr;
	const char* level0_ = nullptr;
	const char* blocks_begin_ = nullptr;
	const char* tail_ = nullptr;
	std::size_t blocks_ = 0, next_block_ = 0;
	std::vector<group> groups_;
	entry entries_[32];
	std::size_t entries_n_ = 0;
	std::vector<format::impact> impacts_;
	std::size_t decoded_group_ = std::numeric_limits<std::size_t>::max();
	std::uint64_t tail_first_position_ = 0, tail_pos_offset_ = 0;
	std::size_t sum_block_ = std::numeric_limits<std::size_t>::max(), sum_pos_ = 0;
	std::uint64_t sum_ = 0;
	bool with_positions_ = false, with_counts_ = false;
	ordinal_t buf_ords_[format::block] = {};
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
class positions_reader {
public:
	positions_reader(const segment& seg, const format::term_info& info) {
		if (info.flags & format::no_positions) { return; }
		auto bytes = seg.positions_bytes();
		if (info.positions > bytes.size() || info.positions_length > bytes.size() - info.positions) { throw corrupt_segment("positions offset"); }
		seg.verify_positions(info.positions, info.positions_length);
		const char* p = bytes.data() + info.positions;
		end_ = p + info.positions_length;  // everything read stays inside the verified range
		max_count_ = seg.max_positions();
		if (!(info.flags & format::counts_are_wdf)) {
			const std::uint64_t n = format::get_leb(&p, end_);
			if (n > std::uint64_t(end_ - p)) { throw corrupt_segment("count blocks bounds"); }
			count_blocks_ = p;
			count_blocks_bytes_ = n;
			p += n;
			for (std::size_t i = 0; i < info.df % format::block; ++i) { count_tail_.push_back(std::uint32_t(format::get_leb(&p, end_))); }
		}
		full_blocks_ = format::get_leb(&p, end_);
		blocks_bytes_ = format::get_leb(&p, end_);
		if (blocks_bytes_ > std::uint64_t(end_ - p) || full_blocks_ > blocks_bytes_) { throw corrupt_segment("position blocks bounds"); }
		blocks_ = p;
		tail_ = p + blocks_bytes_;
	}

	// The number of positions of the iterator's current document, without decoding them.
	std::uint32_t count(postings_iterator& it) {
		if (it.info().flags & format::counts_are_wdf) { return it.wdf(); }
		return counts(it.in_tail(), it.positions_start().count_offset)[it.index_in_block()];
	}

	// The positions of the iterator's current document, ascending.
	void read(postings_iterator& it, std::vector<std::uint32_t>& out) {
		const auto start = it.positions_start();
		const std::uint32_t* counts = it.info().flags & format::counts_are_wdf ? it.wdfs() : this->counts(it.in_tail(), start.count_offset);
		const std::uint64_t first = it.position_index(counts);
		const std::uint32_t count = counts[it.index_in_block()];
		// No document has more positions than the segment's maximum; full blocks hold 128
		// positions each and a tail position takes at least a byte.
		if (count > max_count_ || first + count > full_blocks_ * format::block + std::uint64_t(end_ - tail_)) { throw corrupt_segment("position count"); }
		out.resize(count);
		std::uint32_t at = 0;
		std::uint64_t i = first;
		std::uint32_t* o = out.data();
		// Spans of full blocks, then the tail.
		while (i < first + count && i / format::block < full_blocks_) {
			const std::uint64_t j = i / format::block;
			if (decoded_block_ != j) { decode_block(j, start); }
			const std::uint64_t stop = std::min<std::uint64_t>(first + count, (j + 1) * format::block);
			for (const std::uint32_t* v = block_buf_ + i % format::block; i < stop; ++i) { *o++ = at += *v++; }
		}
		for (; i < first + count; ++i) { *o++ = at += value(i, start); }
	}

private:
	const std::uint32_t* counts(bool tail, std::uint64_t offset) {
		if (tail) { return count_tail_.data(); }
		if (decoded_counts_ != offset) {
			if (offset >= count_blocks_bytes_ ||
				!bitpack::decode(reinterpret_cast<const std::uint8_t*>(count_blocks_ + offset), count_blocks_bytes_ - offset, counts_buf_)) {
				throw corrupt_segment("count block");
			}
			decoded_counts_ = offset;
		}
		return counts_buf_;
	}

	// Position i of the stream; start locates the postings block's first position block.
	std::uint32_t value(std::uint64_t i, const postings_iterator::position_start& start) {
		const std::uint64_t j = i / format::block;
		if (j < full_blocks_) {
			if (decoded_block_ != j) { decode_block(j, start); }
			return block_buf_[i % format::block];
		}
		const std::uint64_t t = i - full_blocks_ * format::block;
		while (tail_values_.size() <= t) {
			if (tail_p_ == nullptr) { tail_p_ = tail_; }
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
		if (at > j) { throw corrupt_segment("position block order"); }
		for (; at < j; ++at) {
			if (offset >= blocks_bytes_) { throw corrupt_segment("position block bounds"); }
			const std::size_t n = bitpack::encoded_size(reinterpret_cast<const std::uint8_t*>(blocks_ + offset), blocks_bytes_ - offset);
			if (!n) { throw corrupt_segment("position block"); }
			offset += n;
		}
		if (offset >= blocks_bytes_ ||
			!bitpack::decode(reinterpret_cast<const std::uint8_t*>(blocks_ + offset), blocks_bytes_ - offset, block_buf_)) {
			throw corrupt_segment("position block");
		}
		decoded_block_ = j;
		decoded_offset_ = offset;
	}

	const char* end_ = nullptr;
	const char* count_blocks_ = nullptr;
	const char* blocks_ = nullptr;
	const char* tail_ = nullptr;
	const char* tail_p_ = nullptr;
	std::uint64_t count_blocks_bytes_ = 0, full_blocks_ = 0, blocks_bytes_ = 0;
	std::uint32_t max_count_ = 0;
	std::vector<std::uint32_t> count_tail_, tail_values_;
	std::uint32_t counts_buf_[format::block] = {}, block_buf_[format::block] = {};
	std::uint64_t decoded_counts_ = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t decoded_block_ = std::numeric_limits<std::uint64_t>::max(), decoded_offset_ = 0;
};

}  // namespace strata
