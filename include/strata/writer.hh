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

// The in-memory segment: inverts documents as they arrive, then flushes one segment
// file in a single sequential pass.
//
// Postings of a term with df >= 128 (all integers LEB128 unless noted):
//   [group count][tail first position index][level 1: per 32 blocks: last ordinal delta,
//    first position index delta, level-0 byte length,
//    blocks byte length, impacts][level 0: per block: last ordinal delta, block byte
//    length, first position index delta, impacts][blocks][tail]
// A block is a bitpack FOR block of ordinal deltas, then (unless no-wdf) a PFOR block
// of wdf. The tail holds ordinal deltas, then wdf. Terms with 1 < df < 128 store only
// the tail; df == 1 lives in the term info.
//
// Positions of a term: [per-document counts when they differ from wdf: count block byte
// lengths, PFOR blocks aligned with the postings blocks, tail][position block count]
// [position block byte lengths][PFOR blocks of 128][tail].

#pragma once

#include "bitpack.hh"
#include "compressor_zstd.h"
#include "fst.hh"
#include "fusefilter.hh"
#include "hashes.hh"
#include "strata/budget.hh"
#include "strata/docids.hh"
#include "strata/format.hh"
#include "strata/keys.hh"
#include "strata/section_io.hh"
#include "task.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace strata {

// One term of a document being added.
struct posting_input {
	std::string_view term;
	std::uint32_t wdf = 1;
	std::span<const std::uint32_t> positions;  // sorted, unique; empty for positionless terms
};

// One term's postings as a flush or a merge produces them: ordinals ascending, a wdf and a
// position count per ordinal, and every position as deltas within its document.
struct term_postings {
	std::vector<std::uint32_t> ords, wdfs, counts, positions;
	std::uint64_t cf = 0;
	bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	void clear() {
		ords.clear(), wdfs.clear(), counts.clear(), positions.clear();
		cf = 0;
		all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	}
};

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
inline void finish_segment(section_sink& sink, std::span<const std::uint64_t> docids, std::uint32_t structures) {
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
	f.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
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
}  // namespace detail

// Terms, postings, positions, lengths, and norms: the inverted plugin's builder
// (layers.md L5). Its sections go to the sink in format 12's order.
class inverted_builder {
public:
	bool wide_skip_offsets = false;  // 64-bit skip offsets even where 32 bits fit
	flush_charge* memory = nullptr;
	// The index's key prefix: a term with it is refused, never dropped (F8's backstop to
	// stage 1); empty for none.
	std::string_view key_prefix;
	bool norms = true;  // whether the norms sub-structure is built (F4)

	struct token {
		std::string_view term;
		std::uint32_t position;
	};

	// A document's length; returns its ordinal. The core checked the limits.
	ordinal_t begin(std::uint32_t length) {
		const ordinal_t ord = ordinal_t(lengths_.size());
		lengths_.push_back(length);
		total_length_ += length;
		detail::charge_to(memory, lengths_.capacity() * sizeof(std::uint32_t), charged_);
		return ord;
	}
	// Each term once, with its wdf and its positions in increasing order.
	void add_terms(ordinal_t ord, std::span<const posting_input> terms) {
		for (const auto& in : terms) {
			term_buffer& t = buffer_for(in.term);
			if (t.df && t.last == ord) { throw std::invalid_argument("strata: term repeated within a document"); }
			open_posting(t, ord);
			for (auto p : in.positions) { add_position(t, p); }
			close_posting(t, in.wdf, std::uint32_t(in.positions.size()));
		}
	}
	// A document's tokens in order: each occurrence adds one to its term's wdf.
	void add_tokens(ordinal_t ord, std::span<const token> tokens) {
		touched_.clear();
		for (const auto& tk : tokens) {
			term_buffer& t = buffer_for(tk.term);
			if (!t.df || t.last != ord) {
				open_posting(t, ord);
				touched_.push_back(&t);
			}
			add_position(t, tk.position);
		}
		for (term_buffer* t : touched_) { close_posting(*t, t->open_count, t->open_count); }
	}

	std::size_t doc_count() const noexcept { return lengths_.size(); }
	std::uint64_t total_length() const noexcept { return total_length_; }
	std::uint32_t min_length() const noexcept { return lengths_.empty() ? 0 : *std::min_element(lengths_.begin(), lengths_.end()); }
	std::uint32_t max_length() const noexcept { return lengths_.empty() ? 0 : *std::max_element(lengths_.begin(), lengths_.end()); }
	std::uint32_t max_positions() const noexcept { return max_positions_; }
	// The statistics that lead term_index (F7, L9): total, minimum, and maximum length, and
	// the most positions of a term in a document; 20 bytes.
	std::string statistics() const {
		std::string out;
		format::put<std::uint64_t>(out, total_length());
		format::put<std::uint32_t>(out, min_length());
		format::put<std::uint32_t>(out, max_length());
		format::put<std::uint32_t>(out, max_positions());
		return out;
	}

	// The sections from the builder's own term buffers.
	void finish_buffered(section_sink& sink, spill& positions_spill) {
		std::vector<std::pair<std::string_view, std::size_t>> keys;
		keys.reserve(buffers_.size());
		if (memory) { memory->grow(keys.capacity() * sizeof(keys[0])); }
		for (const auto& slot : table_) {
			if (slot.key) { keys.push_back({std::string_view(slot.key, slot.length), slot.buffer}); }
		}
		std::sort(keys.begin(), keys.end());
		std::array<bool, 256> used{};
		for (const auto& [key, _] : keys) {
			for (char c : key) { used[std::uint8_t(c)] = true; }
		}
		std::string alphabet;
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { alphabet.push_back(char(c)); }
		}
		std::size_t next = 0;
		finish(sink, positions_spill, alphabet, [&](term_postings& tp) -> std::optional<std::string_view> {
			if (next == keys.size()) { return std::nullopt; }
			const auto [key, buffer] = keys[next++];
			const term_buffer& t = buffers_[buffer];
			tp.clear();
			decode_all(t.ords, tp.ords, t.df);
			for (std::size_t i = 1; i < tp.ords.size(); ++i) { tp.ords[i] += tp.ords[i - 1]; }
			for (auto& o : tp.ords) { --o; }  // stored as ordinal + 1 deltas
			decode_all(t.wdfs, tp.wdfs, t.df);
			if (t.counts_are_wdf) {
				tp.counts = tp.wdfs;
			} else {
				decode_all(t.counts, tp.counts, t.df);
			}
			if (t.any_positions) {
				std::size_t total = 0;
				for (auto c : tp.counts) { total += c; }
				decode_all(t.positions, tp.positions, total);
			}
			tp.cf = t.cf;
			tp.all_zero_wdf = t.all_zero_wdf;
			tp.any_positions = t.any_positions;
			tp.counts_are_wdf = t.counts_are_wdf;
			return key;
		});
	}

	// The sections with terms from next(tp), in strictly increasing order (a merge).
	template <class Next>
	void finish(section_sink& sink, spill& positions_spill, std::string_view alphabet, Next&& next) {
		section_stream& postings = sink.begin();
		const auto deferred = sink.defer(positions_spill);
		section_stream& positions = deferred->stream();
		termdict::builder dict(alphabet);
		format::term_run run;
		term_postings tp;
		std::string encoded_postings, term_positions;
		std::string last_key;
		bool first_term = true;
		std::uint64_t scratch = 0;  // the per-term buffers' charged capacity
		while (const auto got = next(tp)) {
			const std::string_view key = *got;
			if (!first_term && key <= std::string_view(last_key)) { throw std::invalid_argument("strata: terms must increase"); }
			first_term = false;
			refuse_key(key);
			last_key.assign(key);
			const auto& t = tp;
			const auto& ords = tp.ords;
			const auto& wdfs = tp.wdfs;
			const auto& counts = tp.counts;
			const auto& pos = tp.positions;
			const std::uint32_t df = std::uint32_t(ords.size());
			if (df == 0 || wdfs.size() != df || counts.size() != df) { throw std::invalid_argument("strata: malformed term postings"); }
			for (auto c : counts) { max_positions_ = std::max(max_positions_, c); }

			format::term_info info;
			info.df = df;
			info.cf = t.cf;
			if (t.all_zero_wdf) { info.flags |= format::no_wdf; }
			if (!t.any_positions) { info.flags |= format::no_positions; }
			if (t.counts_are_wdf) { info.flags |= format::counts_are_wdf; }

			std::vector<format::impact> all;
			all.reserve(ords.size());
			for (std::size_t i = 0; i < ords.size(); ++i) { all.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			info.impacts = format::competitive(all);

			positions_layout layout;
			if (t.any_positions) {
				info.positions = positions.size();
				term_positions.clear();
				layout = write_positions(term_positions, counts, pos, t.counts_are_wdf);
				info.positions_length = term_positions.size();
				positions.write(term_positions);
			}
			if (df == 1) {
				info.flags |= format::single_posting;
				info.postings = ords[0];
				info.single_wdf = wdfs[0];
			} else {
				info.postings = postings.size();
				encoded_postings.clear();
				write_postings(encoded_postings, ords, wdfs, !t.all_zero_wdf, counts, t.any_positions ? &layout : nullptr, !t.counts_are_wdf);
				info.postings_length = encoded_postings.size();
				postings.write(encoded_postings);
			}
			dict.add(key, format::encode_term_info(info, run, dict.next_is_restart()));
			if (memory) {
				const std::uint64_t now = (tp.ords.capacity() + tp.wdfs.capacity() + tp.counts.capacity() + tp.positions.capacity()) * sizeof(std::uint32_t) +
					all.capacity() * sizeof(format::impact) + encoded_postings.capacity() + term_positions.capacity();
				if (now > scratch) {
					memory->grow(now - scratch);
					scratch = now;
				}
			}
		}
		sink.end(format::section::postings);
		sink.place(*deferred, format::section::positions);
		auto tables = dict.finish();
		if (memory) { memory->grow(tables.blocks.capacity() + tables.index.capacity()); }
		sink.write(format::section::terms, tables.blocks);
		sink.write(format::section::term_index, statistics() + tables.index);
		detail::append_built(sink, memory, format::section::lengths, pack_u32(lengths_));
		if (!norms) { return; }
		std::string bytes(lengths_.size(), '\0');
		for (std::size_t i = 0; i < lengths_.size(); ++i) { bytes[i] = char(format::encode_length(lengths_[i])); }
		detail::append_built(sink, memory, format::section::norms, bytes);
	}

private:
	struct term_buffer {
		std::string ords, wdfs, counts, positions;  // LEB128 streams while buffering
		ordinal_t last = 0;
		std::uint32_t df = 0, max_wdf = 0;
		std::uint32_t open_count = 0, open_prev = 0;  // the open posting: positions so far, the last one
		std::uint64_t cf = 0;
		bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	};

	// A term's posting in document ord: opened, given its positions in order, then closed
	// with its wdf and position count.
	void open_posting(term_buffer& t, ordinal_t ord) {
		put_stream(t.ords, t.df ? ord - t.last : ord + 1);
		t.last = ord;
		++t.df;
		t.open_count = 0;
		t.open_prev = 0;
	}
	void add_position(term_buffer& t, std::uint32_t p) {
		put_stream(t.positions, p - t.open_prev);
		t.open_prev = p;
		++t.open_count;
	}
	void close_posting(term_buffer& t, std::uint32_t wdf, std::uint32_t count) {
		put_stream(t.wdfs, wdf);
		t.cf += wdf;
		t.max_wdf = std::max(t.max_wdf, wdf);
		if (wdf) { t.all_zero_wdf = false; }
		if (count) { t.any_positions = true; }
		// Counts are kept only once one differs from its wdf; until then they are the wdfs,
		// whose LEB128 stream is the same bytes.
		if (t.counts_are_wdf && count != wdf) {
			t.counts_are_wdf = false;
			const std::size_t before = t.counts.capacity();
			t.counts.assign(t.wdfs, 0, t.wdfs.size() - leb_size(wdf));
			if (memory && t.counts.capacity() > before) { memory->grow(t.counts.capacity() - before); }
		}
		if (!t.counts_are_wdf) { put_stream(t.counts, count); }
		max_positions_ = std::max(max_positions_, count);
	}
	// Appends to a posting stream, charging its reallocations.
	void put_stream(std::string& stream, std::uint64_t v) {
		if (!memory) {
			format::put_leb(stream, v);
			return;
		}
		const std::size_t before = stream.capacity();
		format::put_leb(stream, v);
		if (stream.capacity() > before) { memory->grow(stream.capacity() - before); }
	}

	// Open addressing with linear probing over stored hashes; grows at half load.
	struct slot {
		std::uint64_t hash = 0;
		const char* key = nullptr;  // in arena_
		std::uint32_t length = 0;
		std::uint32_t buffer = 0;
	};

	void refuse_key(std::string_view term) const {
		if (!key_prefix.empty() && term.starts_with(key_prefix)) { throw std::invalid_argument("strata: a term with the key prefix (F8)"); }
	}
	term_buffer& buffer_for(std::string_view term) {
		if (table_.empty()) {
			table_.resize(1 << 16);
			if (memory) { memory->grow(table_.size() * sizeof(slot)); }
		}
		const std::uint64_t h = ::wordwise::hash(term.data(), term.size());
		std::size_t mask = table_.size() - 1;
		for (std::size_t i = h & mask;; i = (i + 1) & mask) {
			slot& sl = table_[i];
			if (!sl.key) { break; }
			if (sl.hash == h && sl.length == term.size() && std::memcmp(sl.key, term.data(), term.size()) == 0) { return buffers_[sl.buffer]; }
		}
		refuse_key(term);
		// Term bytes live in stable arena chunks so slots can point at them.
		if (arena_.empty() || arena_.back().size() + term.size() > arena_.back().capacity()) {
			arena_.emplace_back();
			arena_.back().reserve(std::max<std::size_t>(1 << 20, term.size()));
			if (memory) { memory->grow(arena_.back().capacity()); }
		}
		std::string& chunk = arena_.back();
		const char* at = chunk.data() + chunk.size();
		chunk.append(term);
		if ((buffers_.size() + 1) * 2 > table_.size()) { grow(); }
		insert({h, at, std::uint32_t(term.size()), std::uint32_t(buffers_.size())});
		if (memory) { memory->grow(sizeof(term_buffer)); }
		return buffers_.emplace_back();
	}

	void insert(const slot& s) {
		const std::size_t mask = table_.size() - 1;
		std::size_t i = s.hash & mask;
		while (table_[i].key) { i = (i + 1) & mask; }
		table_[i] = s;
	}

	void grow() {
		if (memory) { memory->grow(table_.size() * sizeof(slot)); }  // the table doubles
		std::vector<slot> old(table_.size() * 2);
		old.swap(table_);
		for (const auto& s : old) {
			if (s.key) { insert(s); }
		}
	}

	static std::size_t leb_size(std::uint64_t v) noexcept {
		std::size_t n = 1;
		while (v >= 0x80) { v >>= 7, ++n; }
		return n;
	}

	static void decode_all(const std::string& stream, std::vector<std::uint32_t>& out, std::size_t n) {
		out.resize(n);
		const char* p = stream.data();
		const char* end = p + stream.size();
		for (auto& v : out) { v = std::uint32_t(format::get_leb(&p, end)); }
	}

	struct block_meta {
		ordinal_t last;
		std::uint64_t bytes;
		std::uint64_t first_position;  // index into the term's position stream
		std::vector<format::impact> impacts;
		std::uint64_t pos_offset = 0;    // byte offset of the position block holding first_position
		std::uint64_t count_offset = 0;  // byte offset of the block's count block
		bool bitset = false;             // ordinals as a bitset instead of FOR deltas
	};

	// Where write_positions put each block, for the skip data.
	struct positions_layout {
		std::vector<std::uint64_t> block_offsets;  // each full position block, from the blocks' start
		std::uint64_t blocks_bytes = 0;            // where the tail starts
		std::vector<std::uint64_t> count_offsets;  // each postings block's count block
		std::uint64_t offset_of(std::uint64_t first_position) const {
			const std::uint64_t j = first_position / format::block;
			return j < block_offsets.size() ? block_offsets[j] : blocks_bytes;
		}
	};

	void write_postings(std::string& out, const std::vector<std::uint32_t>& ords, const std::vector<std::uint32_t>& wdfs, bool with_wdf,
		const std::vector<std::uint32_t>& counts, const positions_layout* layout, bool with_counts) {
		const std::size_t full = ords.size() / format::block * format::block;
		std::string blocks;
		std::vector<block_meta> meta;
		std::uint32_t deltas[format::block];
		std::uint8_t buf[bitpack::max_encoded_bytes];
		std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();  // the first delta is ordinal + 1
		std::uint64_t position = 0;
		std::string bits;
		for (std::size_t b = 0; b < full; b += format::block) {
			const std::size_t start = blocks.size();
			// A dense block is a bitset over (previous last, last] when that is smaller.
			const ordinal_t base = b ? ords[b - 1] + 1 : 0;
			const std::uint64_t span = std::uint64_t(ords[b + format::block - 1]) - base + 1;
			bitpack::delta_encode(prev, ords.data() + b, deltas);
			const std::size_t for_bytes = bitpack::encode_for(deltas, buf);
			const bool bitset = (span + 7) / 8 < for_bytes;
			if (bitset) {
				bits.assign((span + 7) / 8, '\0');
				for (std::size_t i = b; i < b + format::block; ++i) {
					const std::uint64_t at = ords[i] - base;
					bits[at >> 3] = char(std::uint8_t(bits[at >> 3]) | (1u << (at & 7)));
				}
				blocks += bits;
			} else {
				blocks.append(reinterpret_cast<char*>(buf), for_bytes);
			}
			if (with_wdf) { blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(wdfs.data() + b, buf)); }
			std::vector<format::impact> pairs;
			for (std::size_t i = b; i < b + format::block; ++i) { pairs.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			meta.push_back({ords[b + format::block - 1], blocks.size() - start, position, format::competitive(std::move(pairs))});
			meta.back().bitset = bitset;
			if (layout) {
				meta.back().pos_offset = layout->offset_of(position);
				if (with_counts) { meta.back().count_offset = layout->count_offsets[b / format::block]; }
			}
			for (std::size_t i = b; i < b + format::block; ++i) { position += counts[i]; }
			prev = ords[b + format::block - 1];
		}
		std::string tail;
		for (std::size_t i = full; i < ords.size(); ++i) { format::put_leb(tail, ords[i] - prev), prev = ords[i]; }
		if (with_wdf) {
			for (std::size_t i = full; i < ords.size(); ++i) { format::put_leb(tail, wdfs[i]); }
		}
		if (meta.empty()) {
			out += tail;
			return;
		}
		// Level 0, grouped by 32 blocks. Level 1 is a fixed-width array, one entry per group,
		// so a reader locates groups without parsing: last ordinal (u32), the offset of its
		// competitive pairs in the pairs area (u32), its level-0 and blocks offsets, and for
		// terms with positions its first position index and position and count block
		// offsets, each u32, or u64 when the term flags itself wide. Level-0 positional fields (present only with positions, counts only
		// when they differ from wdf) are deltas restarting per group.
		const bool with_positions = layout != nullptr;
		with_counts = with_counts && with_positions;
		std::string level1, pairs, level0;
		const std::size_t groups = (meta.size() + 31) / 32;
		struct group_fields {
			ordinal_t last;
			std::uint32_t pairs;
			std::uint64_t level0, blocks, first_position, pos_offset, count_offset;
		};
		std::vector<group_fields> fields;
		ordinal_t prev_last = 0;
		std::uint64_t blocks_before = 0;
		for (std::size_t g = 0; g < groups; ++g) {
			std::string group;
			const block_meta& first = meta[g * 32];
			block_meta prev = first;
			std::uint64_t group_bytes = 0;
			std::vector<format::impact> group_pairs;
			const std::uint64_t level0_offset = level0.size(), blocks_offset = blocks_before;
			for (std::size_t i = g * 32; i < std::min(meta.size(), g * 32 + 32); ++i) {
				format::put_leb(group, meta[i].last - (i ? prev_last : 0));
				format::put_leb(group, meta[i].bytes << 1 | std::uint64_t(meta[i].bitset));
				if (with_positions) {
					format::put_leb(group, meta[i].first_position - prev.first_position);
					format::put_leb(group, meta[i].pos_offset - prev.pos_offset);
					if (with_counts) { format::put_leb(group, meta[i].count_offset - prev.count_offset); }
				}
				format::encode_sized_impacts(group, meta[i].impacts);
				prev_last = meta[i].last;
				prev = meta[i];
				group_bytes += meta[i].bytes;
				group_pairs.insert(group_pairs.end(), meta[i].impacts.begin(), meta[i].impacts.end());
			}
			blocks_before += group_bytes;
			if (pairs.size() > std::numeric_limits<std::uint32_t>::max()) { throw std::length_error("strata: skip data too large"); }
			fields.push_back({meta[std::min(meta.size(), g * 32 + 32) - 1].last, std::uint32_t(pairs.size()), level0_offset, blocks_offset,
				first.first_position, first.pos_offset, first.count_offset});
			format::encode_impacts(pairs, format::competitive(std::move(group_pairs)));
			level0 += group;
		}
		// Offsets take 32 bits unless one of this term's needs 64.
		bool wide = wide_skip_offsets;
		for (const auto& f : fields) {
			wide = wide || std::max({f.level0, f.blocks, f.first_position, f.pos_offset, f.count_offset}) > std::numeric_limits<std::uint32_t>::max();
		}
		auto put_offset = [&](std::uint64_t v) {
			if (wide) {
				format::put<std::uint64_t>(level1, v);
			} else {
				format::put<std::uint32_t>(level1, std::uint32_t(v));
			}
		};
		for (const auto& f : fields) {
			format::put<std::uint32_t>(level1, f.last);
			format::put<std::uint32_t>(level1, f.pairs);
			put_offset(f.level0);
			put_offset(f.blocks);
			if (with_positions) {
				put_offset(f.first_position);
				put_offset(f.pos_offset);
				put_offset(f.count_offset);
			}
		}
		format::put_leb(out, groups);
		out.push_back(char(wide));
		if (with_positions) {
			format::put_leb(out, position);  // the tail's first position index and its block offset
			format::put_leb(out, layout->offset_of(position));
		}
		format::put_leb(out, pairs.size());
		format::put_leb(out, level0.size());
		format::put_leb(out, blocks.size());
		out += level1;
		out += pairs;
		out += level0;
		out += blocks;
		out += tail;
	}

	// [count blocks bytes][count blocks][tail counts] when counts differ from wdf, then
	// [full position blocks][position blocks bytes][position blocks][tail positions]. Blocks
	// sit back to back; the skip data locates them.
	static positions_layout write_positions(std::string& out, const std::vector<std::uint32_t>& counts, const std::vector<std::uint32_t>& pos,
		bool counts_are_wdf) {
		positions_layout layout;
		std::uint8_t buf[bitpack::max_encoded_bytes];
		if (!counts_are_wdf) {
			const std::size_t full = counts.size() / format::block * format::block;
			std::string count_blocks;
			for (std::size_t b = 0; b < full; b += format::block) {
				layout.count_offsets.push_back(count_blocks.size());
				count_blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(counts.data() + b, buf));
			}
			format::put_leb(out, count_blocks.size());
			out += count_blocks;
			for (std::size_t i = full; i < counts.size(); ++i) { format::put_leb(out, counts[i]); }
		}
		const std::size_t full = pos.size() / format::block * format::block;
		std::string blocks;
		for (std::size_t b = 0; b < full; b += format::block) {
			layout.block_offsets.push_back(blocks.size());
			blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(pos.data() + b, buf));
		}
		layout.blocks_bytes = blocks.size();
		format::put_leb(out, full / format::block);
		format::put_leb(out, blocks.size());
		out += blocks;
		for (std::size_t i = full; i < pos.size(); ++i) { format::put_leb(out, pos[i]); }
		return layout;
	}

	// Values bit-packed in 128-value blocks at one width: [width u8][blocks].
	static std::string pack_u32(const std::vector<std::uint32_t>& values) {
		std::uint32_t all = 0;
		for (auto v : values) { all |= v; }
		const unsigned width = 32 - unsigned(std::countl_zero(all));
		std::string out(1, char(width));
		std::uint32_t chunk[format::block];
		std::uint8_t buf[bitpack::packed_bytes(32)];
		for (std::size_t b = 0; b < values.size(); b += format::block) {
			std::size_t n = std::min<std::size_t>(format::block, values.size() - b);
			std::fill(chunk, chunk + format::block, 0u);
			std::copy_n(values.begin() + std::ptrdiff_t(b), n, chunk);
			bitpack::pack(chunk, width, buf);
			out.append(reinterpret_cast<char*>(buf), bitpack::packed_bytes(width));
		}
		return out;
	}

	std::deque<std::string> arena_;
	std::vector<slot> table_;
	std::deque<term_buffer> buffers_;
	std::vector<term_buffer*> touched_;  // add_tokens: the document's open postings
	std::vector<std::uint32_t> lengths_;
	std::uint32_t max_positions_ = 0;  // the most positions of one term in one document
	std::uint64_t total_length_ = 0;
	std::uint64_t charged_ = 0;  // the lengths column's charged capacity
};

// Keys and versions: the keys plugin's builder (F8, F9). keys streams with the plugins'
// sections; key_index and versions go to the tail.
class keys_builder {
public:
	flush_charge* memory = nullptr;

	void add(const doc_fields& f) {
		if (!f.key.empty()) {
			key_bytes_ += f.key.size();
			keys_.push_back({std::string(f.key), ordinal_t(versions_.size())});
		}
		versions_.push_back(f.version);
		detail::charge_to(memory, versions_.capacity() * sizeof(std::uint64_t) + keys_.capacity() * sizeof(keys_[0]) + key_bytes_, charged_);
	}
	void finish(section_sink& sink) {
		std::vector<std::pair<std::string_view, ordinal_t>> entries;
		entries.reserve(keys_.size());
		for (const auto& [k, o] : keys_) { entries.push_back({k, o}); }
		auto sections = keys::encode(std::move(entries));
		detail::append_built(sink, memory, format::section::keys, sections.keys);
		detail::append_built(sink, memory, format::section::versions, encode_versions());
		detail::append_built(sink, memory, format::section::key_index, sections.index);
	}

private:
	// versions: [min u64][width u8][(version - min) in width bits each, little-endian bit order].
	std::string encode_versions() const {
		std::string out;
		const std::uint64_t lo = versions_.empty() ? 0 : *std::min_element(versions_.begin(), versions_.end());
		const std::uint64_t hi = versions_.empty() ? 0 : *std::max_element(versions_.begin(), versions_.end());
		const unsigned width = unsigned(std::bit_width(hi - lo));
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
	std::vector<std::pair<std::string, ordinal_t>> keys_;
	std::uint64_t key_bytes_ = 0, charged_ = 0;
};

// Stored records: the stored plugin's builder.
class stored_builder {
public:
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;  // checked per frame

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

// The in-memory segment: per-document fields and docids in the core, terms, keys, and
// stored records in the plugins' builders; one sequential pass writes the file.
class segment_writer {
public:
	// Writes 64-bit skip offsets even where 32 bits fit (tests the wide layout).
	bool wide_skip_offsets = false;
	// Documents arrive in output order with distinct docids in any order (a reordering
	// merge, reordering.md D2); flushes leave it off, so docids increase.
	bool permuted = false;
	// The verification region size of every section, as a power of two.
	std::uint8_t region_shift = format::default_region_shift;
	// The run's draw on the indexing budget, if any: the term table, posting buffers,
	// per-document columns, and the flush's scratch and output report their allocations.
	flush_charge* memory = nullptr;
	// When given, the stored section checks it per frame and throws task::cancelled.
	const task::cancel_token* cancel = nullptr;
	// The index's key prefix, for the inverted builder's backstop (F8); empty for none.
	std::string key_prefix;
	// The structures the segment holds, set from the index's configuration, never from its
	// documents (F4). keys stays until keyless segments arrive with F9's version rule.
	std::uint32_t structures = format::bit::inverted | format::bit::norms | format::bit::keys | format::bit::stored;

	// Adds a document and returns its ordinal. Docids strictly increase, so ordinal order
	// is docid order. `length` is the document length used by weighting (Xapian: the sum
	// of wdf over non-boolean terms). `stored` is the document's stored record, returned
	// by segment::stored. `key` is the document's key (F8), never one of its terms, or empty.
	// Each term appears once, with its wdf and its positions in increasing order.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::uint32_t length, std::string_view stored = {},
		std::uint64_t version = 0, std::string_view key = {}) {
		if (!terms.empty() && !(structures & format::bit::inverted)) { throw std::invalid_argument("strata: terms in a segment without inverted"); }
		const ordinal_t ord = begin_document(docid, length, stored, version, key);
		inverted_.add_terms(ord, terms);
		return ord;
	}

	// The same from a document's tokens in order, as a tokenizer produces them: each
	// occurrence of a term adds one to its wdf and its position to its positions, so
	// nothing has to group them first. Positions increase.
	using token = inverted_builder::token;
	ordinal_t add_tokens(std::uint64_t docid, std::span<const token> tokens, std::uint32_t length, std::string_view stored = {},
		std::uint64_t version = 0, std::string_view key = {}) {
		if (!tokens.empty() && !(structures & format::bit::inverted)) { throw std::invalid_argument("strata: terms in a segment without inverted"); }
		const ordinal_t ord = begin_document(docid, length, stored, version, key);
		inverted_.add_tokens(ord, tokens);
		return ord;
	}

	// Adds a document's per-document data only, for a writer whose terms come from
	// flush_from (a merge); returns its ordinal.
	ordinal_t add_document(std::uint64_t docid, std::uint32_t length, std::string_view stored = {}, std::uint64_t version = 0, std::string_view key = {}) {
		return begin_document(docid, length, stored, version, key);
	}

	std::size_t doc_count() const noexcept { return inverted_.doc_count(); }

	// Writes the segment file and returns its bytes.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id) {
		std::string file;
		string_output out(file, memory);
		string_spill positions(memory);
		flush(segment_id, out, positions);
		return file;
	}

	// Streams the segment file to out in one pass. The positions section is produced
	// alongside the postings, so it goes to `positions` until the postings end.
	void flush(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill) {
		write(segment_id, sink, [&](section_sink& s) { inverted_.finish_buffered(s, positions_spill); });
	}

	// Writes the segment with terms from next(tp), which fills tp and returns the term, in
	// strictly increasing order, or nothing at the end. Documents come from add() or
	// add_document(); a term's ordinals name them. `alphabet` holds every byte the terms
	// use (the dictionary codes its keys over it; empty: all 256).
	template <class Next>
	void flush_from(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill, std::string_view alphabet, Next&& next) {
		write(segment_id, sink, [&](section_sink& s) { inverted_.finish(s, positions_spill, alphabet, std::forward<Next>(next)); });
	}

private:
	// Builders read the writer's settings as they are when each call starts.
	void sync() noexcept {
		inverted_.memory = keys_.memory = stored_.memory = memory;
		inverted_.wide_skip_offsets = wide_skip_offsets;
		inverted_.key_prefix = key_prefix;
		inverted_.norms = (structures & format::bit::norms) != 0;
		stored_.cancel = cancel;
	}

	// The per-document data; returns the document's ordinal.
	ordinal_t begin_document(std::uint64_t docid, std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		sync();
		// Ordinals are 32-bit and the largest value means no more documents.
		if (inverted_.doc_count() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (docid == 0 || (!permuted && !docids_.empty() && docid <= docids_.back())) { throw std::invalid_argument("strata: docids must increase within a segment"); }
		if (stored.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		if (!stored.empty() && !(structures & format::bit::stored)) { throw std::invalid_argument("strata: a stored record in a segment without stored"); }
		keys_.add({docid, version, key});
		stored_.add(stored);
		const ordinal_t ord = inverted_.begin(length);
		docids_.push_back(docid);
		detail::charge_to(memory, docids_.capacity() * sizeof(std::uint64_t), charged_);
		return ord;
	}

	// The file in F5's order: the header; the streamed sections (the inverted ones, then
	// stored); the pinned tail by rank; then the table of contents and footer. Every
	// structure of format 12's layout is built (F4).
	template <class Inverted>
	void write(const std::array<std::uint8_t, 16>& segment_id, output& out, Inverted&& inverted) {
		sync();
		constexpr std::uint32_t known = format::bit::inverted | format::bit::norms | format::bit::keys | format::bit::stored;
		if ((structures & ~known) || !(structures & format::bit::keys) || ((structures & format::bit::norms) && !(structures & format::bit::inverted))) {
			throw std::invalid_argument("strata: segment structures");
		}
		section_sink sink(out, region_shift, nullptr, memory);
		detail::begin_segment(sink, segment_id);
		if (structures & format::bit::inverted) { inverted(sink); }
		detail::write_docids(sink, memory, docids_);
		keys_.finish(sink);
		if (structures & format::bit::stored) { stored_.finish(sink); }
		detail::finish_segment(sink, docids_, structures);
	}

	inverted_builder inverted_;
	keys_builder keys_;
	stored_builder stored_;
	std::vector<std::uint64_t> docids_;
	std::uint64_t charged_ = 0;  // the docids column's charged capacity
};

}  // namespace strata
