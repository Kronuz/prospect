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
#include "fst.hh"
#include "hashes.hh"
#include <crc32c/crc32c.hh>
#include "prospect/format.hh"
#include "prospect/reader.hh"
#include "prospect/strata.hh"
#include "strata/core/writer.hh"
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

namespace prospect {

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


// Terms, postings, positions, lengths, and norms: the inverted plugin's builder
// (layers.md L5). Its sections go to the sink in rank order (F5).
class inverted_builder {
public:
	bool wide_skip_offsets = false;  // 64-bit skip offsets even where 32 bits fit
	flush_charge* memory = nullptr;
	// The index's key prefix: a term with it is refused, never dropped (F8's backstop to
	// stage 1); empty for none.
	std::string_view key_prefix;
	bool norms = true;  // whether the norms sub-structure is built (F4)

	void configure(const builder_settings& s) noexcept {
		memory = s.memory;
		key_prefix = s.key_prefix;
		norms = (s.structures & format::bit::norms) != 0;
	}

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

	// A document's part (L10): its length, then its terms and its text's tokens, or its text split
	// at spaces. The writer calls it in output order.
	void add(ordinal_t ord, const doc_fields&, const inverted_plugin::part& p) {
		(void)begin(p.length);
		terms_in_.clear();
		for (const auto& t : p.terms) { terms_in_.push_back({t.term, t.wdf, t.positions}); }
		if (p.split_text) {
			add_split(ord, terms_in_, p.text);
		} else {
			add_text(ord, terms_in_, p.text, p.tokens);
		}
	}

	// The sections from the builder's own term buffers, positions through the sink's spill.
	void finish(section_sink& sink) { finish_buffered(sink, sink.held()); }

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
		seed_shortest_ = min_length();
		term_postings tp;
		std::string encoded_postings, term_positions;
		std::vector<format::impact> all;  // a long list's (wdf, length) pairs
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

			if (df >= format::block) {  // the term info stores whole-list pairs only with skip blocks
				// Each posting's length, gathered once for these pairs, the blocks', and the seeds.
				term_lengths_.resize(df);
				all.clear();
				for (std::size_t i = 0; i < ords.size(); ++i) {
					term_lengths_[i] = lengths_[ords[i]];
					all.push_back({wdfs[i], format::encode_length(term_lengths_[i])});
				}
				info.impacts = format::competitive(all);
			}

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
				write_postings(encoded_postings, ords, wdfs, term_lengths_.data(), !t.all_zero_wdf, counts, t.any_positions ? &layout : nullptr, !t.counts_are_wdf);
				info.postings_length = encoded_postings.size();
				postings.write(encoded_postings);
			}
			dict.add(key, format::encode_term_info(info, run, dict.next_is_restart()));
			if (memory) {
				const std::uint64_t now = (tp.ords.capacity() + tp.wdfs.capacity() + tp.counts.capacity() + tp.positions.capacity() + term_lengths_.capacity()) * sizeof(std::uint32_t) +
					all.capacity() * sizeof(format::impact) + encoded_postings.capacity() + term_positions.capacity() +
					(seed_ones_.capacity() + seed_sorted_.capacity() + seed_counts_.capacity() + seed_long_.capacity() + seed_scratch_.capacity() + seed_exact_.capacity() +
						seed_block_.capacity()) * sizeof(std::uint32_t) +
					seed_rest_.capacity() * sizeof(std::uint64_t) + seed_levels_.capacity() * sizeof(std::pair<std::uint32_t, std::uint32_t>);
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
	friend class segment_writer;  // the inverter's text add (detail::writer_access)
	// Terms, each once with its wdf and positions, and the text's tokens in order, each
	// adding one to its term's wdf; a term is in one of the two. Positions increase.
	void add_text(ordinal_t ord, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens) {
		add_text_with(ord, terms, [&](auto&& token) {
			for (const auto& tk : tokens) { token(text.substr(tk.offset, tk.size), tk.position); }
		});
	}
	// The same with the text's words as its tokens, at positions 1, 2, ...
	void add_split(ordinal_t ord, std::span<const posting_input> terms, std::string_view text) {
		add_text_with(ord, terms, [&](auto&& token) {
			std::uint32_t position = 0;
			text_detail::split_words(text, [&](std::size_t offset, std::size_t size) { token(text.substr(offset, size), ++position); });
		});
	}
	// tokens(token) calls token(term, position) for each token, in order.
	template <class Tokens>
	void add_text_with(ordinal_t ord, std::span<const posting_input> terms, Tokens&& tokens) {
		for (const auto& in : terms) {
			term_buffer& t = buffer_for(in.term);
			if (t.df && t.last == ord) { throw std::invalid_argument("strata: term repeated within a document"); }
			open_posting(t, ord);
			for (auto p : in.positions) { add_position(t, p); }
			close_posting(t, in.wdf, std::uint32_t(in.positions.size()));
			t.open_prev = closed;
		}
		touched_.clear();
		tokens([&](std::string_view term, std::uint32_t position) {
			term_buffer& t = buffer_for(term);
			if (!t.df || t.last != ord) {
				open_posting(t, ord);
				touched_.push_back(&t);
			} else if (t.open_prev == closed || position <= t.open_prev) {
				throw std::invalid_argument("strata: term repeated within a document");
			}
			add_position(t, position);
		});
		for (term_buffer* t : touched_) { close_posting(*t, t->open_count, t->open_count); }
	}
	struct term_buffer {
		std::string ords, wdfs, counts, positions;  // LEB128 streams while buffering
		ordinal_t last = 0;
		std::uint32_t df = 0, max_wdf = 0;
		std::uint32_t open_count = 0, open_prev = 0;  // the open posting: positions so far, the last one
		std::uint64_t cf = 0;
		bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	};
	static constexpr std::uint32_t closed = ~std::uint32_t(0);  // open_prev once a terms entry closed it

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
		const char* at = arena_.copy(term, memory).data();  // slots point into it
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
		format::block_codec codec = format::block_codec::deltas;
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

	// lengths: each posting's document length, read only when the term has skip blocks.
	void write_postings(std::string& out, const std::vector<std::uint32_t>& ords, const std::vector<std::uint32_t>& wdfs, const std::uint32_t* lengths, bool with_wdf,
		const std::vector<std::uint32_t>& counts, const positions_layout* layout, bool with_counts) {
		const std::size_t full = ords.size() / format::block * format::block;
		if (full == 0) {  // a tail alone: deltas from ordinal + 1, then wdfs
			std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();
			for (auto o : ords) { format::put_leb(out, o - prev), prev = o; }
			if (with_wdf) {
				for (auto w : wdfs) { format::put_leb(out, w); }
			}
			return;
		}
		std::string blocks;
		std::vector<block_meta> meta;
		std::uint8_t buf[bitpack::max_encoded_bytes];
		std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();  // the tail's first delta is ordinal + 1
		std::uint64_t position = 0;
		std::string bits;
		for (std::size_t b = 0; b < full; b += format::block) {
			const std::size_t start = blocks.size();
			// A dense block is a bitset over (previous last, last] when that is smaller than FOR
			// over its plain gaps, the boundary format 12 drew: gaps - 1 (M23) would take FOR's
			// 17 bytes over an 18-byte bitset, which counting ORs in without decoding.
			const ordinal_t base = b ? ords[b - 1] + 1 : 0;
			const std::uint64_t span = std::uint64_t(ords[b + format::block - 1]) - base + 1;
			const std::size_t for_bytes = format::encode_gaps(ords.data() + b, prev, b == 0, buf);
			const bool bitset = (span + 7) / 8 < gaps_bytes(ords.data() + b, b ? prev : std::numeric_limits<std::uint32_t>::max());
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
			pairs_.clear();
			for (std::size_t i = b; i < b + format::block; ++i) { pairs_.push_back({wdfs[i], format::encode_length(lengths[i])}); }
			meta.push_back({ords[b + format::block - 1], blocks.size() - start, position, format::competitive(pairs_)});
			meta.back().codec = bitset ? format::block_codec::bitset : format::block_codec::deltas;
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
		// Level 1 is a fixed-width array, one entry per group, so a reader locates groups
		// without parsing; each group's record holds its advance entries, pairs, and
		// positional deltas (format.hh, "skip data"). Offsets take u32, or u64 when the term
		// flags itself wide.
		const bool with_positions = layout != nullptr;
		with_counts = with_counts && with_positions;
		std::string level1, pairs, records;
		const std::size_t groups = (meta.size() + format::skip::group_blocks - 1) / format::skip::group_blocks;
		struct group_fields {
			ordinal_t last;
			std::uint32_t pairs;
			std::uint64_t record, blocks, first_position, pos_offset, count_offset;
		};
		std::vector<group_fields> fields;
		std::uint8_t codecs = 0;
		for (const auto& m : meta) { codecs |= std::uint8_t(m.codec); }
		const unsigned wf = unsigned(std::bit_width(codecs));
		std::uint64_t blocks_before = 0;
		for (std::size_t g = 0; g < groups; ++g) {
			const std::size_t b0 = g * format::skip::group_blocks, b1 = std::min(meta.size(), b0 + format::skip::group_blocks), n = b1 - b0;
			const ordinal_t base = g ? meta[b0 - 1].last : 0, last = meta[b1 - 1].last;
			std::uint64_t group_bytes = 0;
			for (std::size_t i = b0; i < b1; ++i) { group_bytes += meta[i].bytes; }
			const unsigned wl = unsigned(std::bit_width(last - base)), wo = unsigned(std::bit_width(group_bytes - 1));
			std::string record(format::advance_bytes(n, wl, wo, wf), '\0');
			std::uint64_t start = 0;
			for (std::size_t i = 0; i < n; ++i) {
				if (i + 1 < n) { format::put_bits(record, i * wl, wl, meta[b0 + i].last - base); }
				if (i) { format::put_bits(record, (n - 1) * wl + (i - 1) * wo, wo, start); }
				format::put_bits(record, (n - 1) * (wl + wo) + i * wf, wf, std::uint8_t(meta[b0 + i].codec));
				start += meta[b0 + i].bytes;
			}
			std::vector<format::impact> group_pairs;
			for (std::size_t i = b0; i < b1; ++i) {
				format::encode_sized_impacts(record, meta[i].impacts);
				group_pairs.insert(group_pairs.end(), meta[i].impacts.begin(), meta[i].impacts.end());
			}
			if (with_positions) {
				for (std::size_t i = b0 + 1; i < b1; ++i) {
					format::put_leb(record, meta[i].first_position - meta[i - 1].first_position);
					format::put_leb(record, meta[i].pos_offset - meta[i - 1].pos_offset);
					if (with_counts) { format::put_leb(record, meta[i].count_offset - meta[i - 1].count_offset); }
				}
			}
			if (pairs.size() > std::numeric_limits<std::uint32_t>::max()) { throw std::length_error("strata: skip data too large"); }
			const block_meta& first = meta[b0];
			fields.push_back({last, std::uint32_t(pairs.size()), records.size(), blocks_before, first.first_position, first.pos_offset, first.count_offset});
			format::encode_impacts(pairs, format::competitive(std::move(group_pairs)));
			records += record;
			blocks_before += group_bytes;
		}
		// Offsets take 32 bits unless one of this term's needs 64.
		bool wide = wide_skip_offsets;
		for (const auto& f : fields) {
			wide = wide || std::max({f.record, f.blocks, f.first_position, f.pos_offset, f.count_offset}) > std::numeric_limits<std::uint32_t>::max();
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
			put_offset(f.record);
			put_offset(f.blocks);
			if (with_positions) {
				put_offset(f.first_position);
				put_offset(f.pos_offset);
				put_offset(f.count_offset);
			}
		}
		format::put_leb(out, groups);
		out.push_back(char((wide ? format::skip::wide : 0) | wf << format::skip::codec_shift));
		if (with_positions) {
			format::put_leb(out, position);  // the tail's first position index and its block offset
			format::put_leb(out, layout->offset_of(position));
		}
		if (with_wdf) {
			const auto seeds = seeds_of(lengths, wdfs);
			for (std::size_t j = 0; j < 3 && format::seed_ks[j] <= ords.size(); ++j) {
				format::put_leb(out, seeds[j].code_wdf);
				out.push_back(char(seeds[j].code));
				format::put_leb(out, seeds[j].exact_wdf);
				format::put_leb(out, seeds[j].exact);
			}
		}
		format::put_leb(out, pairs.size());
		format::put_leb(out, records.size());
		format::put_leb(out, blocks.size());
		out += level1;
		out += pairs;
		out += records;
		out += blocks;
		out += tail;
	}

	// The bytes bitpack's FOR or constant block would take over the block's plain gaps.
	static std::size_t gaps_bytes(const std::uint32_t* ords, std::uint32_t prev) noexcept {
		std::uint32_t all = 0, first = ords[0] - prev;
		bool constant = true;
		for (std::size_t i = 0; i < format::block; ++i) {
			const std::uint32_t g = ords[i] - (i ? ords[i - 1] : prev);
			all |= g;
			constant = constant && g == first;
		}
		return constant ? 5 : 1 + bitpack::packed_bytes(32 - unsigned(std::countl_zero(all)));
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

	// The presets' seed scores compared exactly, in integers, so every build picks the same pairs
	// (no libm, no contraction). Each preset scores w / (w + K(len)) times a positive constant,
	// and K is a positive multiple of norm() below: bm25 (k1 1.2, b 0.75, avg max(1, T / N)),
	// T + 3·len·N; bm25_xapian (k1 1, b 0.5, min_normlen 0.5, avg T / N or 1), 2T + max(2·len·N, T).
	struct seed_rule {
		using u128 = unsigned __int128;
		u128 lt = 1, ln = 1, xt = 1, xn = 1;
		seed_rule(std::uint64_t docs, std::uint64_t total) {
			if (docs && total >= docs) { lt = total, ln = docs; }
			if (docs && total) { xt = total, xn = docs; }
		}
		u128 lucene(std::uint8_t code) const noexcept { return lt + 3 * u128(format::decode_length(code)) * ln; }
		u128 xapian(std::uint32_t len) const noexcept { return 2 * xt + std::max(2 * u128(len) * xn, xt); }
		// w1 / (w1 + n1) > w2 / (w2 + n2), for w > 0 and n > 0.
		static bool above(std::uint32_t w1, u128 n1, std::uint32_t w2, u128 n2) noexcept { return u128(w1) * n2 > u128(w2) * n1; }
	};

	// F13 (M20): for each stored k, the pair (w, the k-th shortest length with wdf >= w) each preset
	// scores highest at this segment's statistics, levels of wdf highest first until no lower wdf
	// can beat every best at the shortest length. A level's k-th shortest depends on no order, so it
	// is read from exact and per-16 length counts that the levels fill.
	std::array<format::seed_pairs, 3> seeds_of(const std::uint32_t* lengths, const std::vector<std::uint32_t>& wdfs) {
		std::array<format::seed_pairs, 3> out{};
		const std::size_t df = wdfs.size();
		const seed_rule rule(lengths_.size(), total_length_);
		if (seed_exact_.empty()) {
			seed_exact_.assign(seed_cap + 1, 0);
			seed_block_.assign((seed_cap >> 4) + 1, 0);
		}
		// Lengths with wdf 1, and (wdf, length) of the rest above 1, by branchless compaction.
		seed_ones_.resize(df);
		seed_rest_.resize(df);
		std::uint32_t top = 0;
		std::size_t ones = 0, rest = 0;
		for (std::size_t i = 0; i < df; ++i) {
			const std::uint32_t w = wdfs[i], l = lengths[i];
			top = std::max(top, w);
			seed_ones_[ones] = l;
			ones += w == 1;
			seed_rest_[rest] = std::uint64_t(w) << 32 | l;
			rest += w >= 2;
		}
		// The rest's lengths by wdf, highest first, and each level's (wdf, end): a counting sort
		// while wdfs are small, else a sort. wdf 1 is the last level.
		seed_levels_.clear();
		seed_sorted_.resize(rest);
		if (top <= (1u << 16)) {
			seed_counts_.assign(std::size_t(top) + 1, 0);
			for (std::size_t i = 0; i < rest; ++i) { ++seed_counts_[seed_rest_[i] >> 32]; }
			std::size_t at = 0;
			for (std::uint32_t w = top; w >= 2; --w) {
				const std::uint32_t n = seed_counts_[w];
				seed_counts_[w] = std::uint32_t(at);
				at += n;
				if (n) { seed_levels_.push_back({w, std::uint32_t(at)}); }
			}
			for (std::size_t i = 0; i < rest; ++i) { seed_sorted_[seed_counts_[seed_rest_[i] >> 32]++] = std::uint32_t(seed_rest_[i]); }
		} else {
			std::sort(seed_rest_.begin(), seed_rest_.begin() + std::ptrdiff_t(rest), std::greater<>());
			for (std::size_t i = 0; i < rest; ++i) {
				seed_sorted_[i] = std::uint32_t(seed_rest_[i]);
				if (i + 1 == rest || seed_rest_[i + 1] >> 32 != seed_rest_[i] >> 32) { seed_levels_.push_back({std::uint32_t(seed_rest_[i] >> 32), std::uint32_t(i + 1)}); }
			}
		}
		if (ones) { seed_levels_.push_back({1, std::uint32_t(rest + ones)}); }
		const std::size_t ks = std::size_t(std::count_if(std::begin(format::seed_ks), std::end(format::seed_ks), [&](std::uint32_t k) { return k <= df; }));
		const auto shortest_lucene = rule.lucene(format::encode_length(seed_shortest_)), shortest_xapian = rule.xapian(seed_shortest_);
		std::uint32_t best_lucene_w[3] = {}, best_xapian_w[3] = {};  // 0: none yet
		seed_rule::u128 best_lucene_n[3] = {}, best_xapian_n[3] = {};
		// Each k's k-th shortest length so far, and the postings at or below it; none until the
		// first full level. Lengths from seed_cap on are counted together, and kept in seed_long_.
		constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t kth[3] = {none, none, none};
		std::size_t at_most[3] = {};
		auto add = [&](const std::uint32_t* p, std::size_t n) {
			std::size_t a0 = at_most[0], a1 = at_most[1], a2 = at_most[2];
			const std::uint32_t k0 = kth[0], k1 = kth[1], k2 = kth[2];
			for (std::size_t i = 0; i < n; ++i) {
				const std::uint32_t l = p[i];
				if (l < seed_cap) {
					++seed_exact_[l];
					++seed_block_[l >> 4];
				} else {
					++seed_block_[seed_cap >> 4];
					seed_long_.push_back(l);
				}
				a0 += l <= k0, a1 += l <= k1, a2 += l <= k2;
			}
			at_most[0] = a0, at_most[1] = a1, at_most[2] = a2;
		};
		std::size_t begin = 0;
		for (std::size_t li = 0; li < seed_levels_.size(); ++li) {
			const auto [w, end] = seed_levels_[li];
			add(w == 1 ? seed_ones_.data() : seed_sorted_.data() + begin, w == 1 ? ones : end - begin);
			begin = end;
			bool full = true;
			for (std::size_t j = 0; j < ks; ++j) {
				const std::size_t k = format::seed_ks[j];
				if (end < k) {
					full = false;
					continue;
				}
				std::uint32_t l = kth[j];
				std::size_t p = at_most[j];
				if (l == none || l >= seed_cap) {  // upward from 0: blocks, then lengths
					std::size_t b = 0;
					p = 0;
					while (p + seed_block_[b] < k) { p += seed_block_[b++]; }
					if (b == seed_cap >> 4) {
						seed_scratch_.assign(seed_long_.begin(), seed_long_.end());
						std::nth_element(seed_scratch_.begin(), seed_scratch_.begin() + std::ptrdiff_t(k - p - 1), seed_scratch_.end());
						l = seed_scratch_[k - p - 1];  // found again at every level: at_most is not kept
					} else {
						l = std::uint32_t(b << 4);
						while (p + seed_exact_[l] < k) { p += seed_exact_[l++]; }
						p += seed_exact_[l];
					}
				} else {  // downward while the length below still holds k postings
					while (p - seed_exact_[l] >= k) {
						p -= seed_exact_[l--];
						if ((l & 15) == 15) {
							while (p - seed_block_[l >> 4] >= k) { p -= seed_block_[l >> 4], l -= 16; }
						}
					}
				}
				kth[j] = l, at_most[j] = p;
				const std::uint8_t code = format::encode_length(l);
				if (const auto n = rule.lucene(code); !best_lucene_w[j] || seed_rule::above(w, n, best_lucene_w[j], best_lucene_n[j])) {
					best_lucene_w[j] = w, best_lucene_n[j] = n;
					out[j].code_wdf = w;
					out[j].code = code;
				}
				if (const auto n = rule.xapian(l); !best_xapian_w[j] || seed_rule::above(w, n, best_xapian_w[j], best_xapian_n[j])) {
					best_xapian_w[j] = w, best_xapian_n[j] = n;
					out[j].exact_wdf = w;
					out[j].exact = l;
				}
			}
			if (full && li + 1 < seed_levels_.size()) {
				const std::uint32_t next = seed_levels_[li + 1].first;
				bool done = true;
				for (std::size_t j = 0; j < ks; ++j) {
					done = done && !seed_rule::above(next, shortest_lucene, best_lucene_w[j], best_lucene_n[j]) &&
						!seed_rule::above(next, shortest_xapian, best_xapian_w[j], best_xapian_n[j]);
				}
				if (done) { break; }
			}
		}
		auto clear = [&](const std::uint32_t* p, std::size_t n) {
			for (std::size_t i = 0; i < n; ++i) {
				const std::uint32_t l = std::min(p[i], seed_cap);
				seed_exact_[l] = 0;
				seed_block_[l >> 4] = 0;
			}
		};
		clear(seed_sorted_.data(), std::min(begin, rest));
		if (begin > rest) { clear(seed_ones_.data(), ones); }
		seed_long_.clear();
		return out;
	}

	// seeds_of's scratch. Its counts cover lengths below seed_cap exactly, and seed_exact_'s
	// one more slot is written only by clear; both are zero between calls.
	static constexpr std::uint32_t seed_cap = 1u << 16;
	std::vector<std::uint32_t> term_lengths_;  // finish: the term's lengths, by posting
	std::vector<std::uint32_t> seed_ones_, seed_sorted_, seed_counts_, seed_long_, seed_scratch_, seed_exact_, seed_block_;
	std::vector<std::uint64_t> seed_rest_;
	std::vector<std::pair<std::uint32_t, std::uint32_t>> seed_levels_;
	std::uint32_t seed_shortest_ = 0;  // the segment's shortest length, set once by finish
	std::vector<format::impact> pairs_;  // write_postings: one block's pairs
	detail::stable_bytes arena_{std::size_t(1) << 20};
	std::vector<slot> table_;
	std::deque<term_buffer> buffers_;
	std::vector<term_buffer*> touched_;  // add_tokens: the document's open postings
	std::vector<posting_input> terms_in_;  // add(part)'s scratch
	std::vector<std::uint32_t> lengths_;
	std::uint32_t max_positions_ = 0;  // the most positions of one term in one document
	std::uint64_t total_length_ = 0;
	std::uint64_t charged_ = 0;  // the lengths column's charged capacity
};

namespace detail {
struct writer_access;
}

// The search layout's writer: the generic writer with the terms-taking adds the tests, the
// tools, and the inverter use.
class segment_writer : public basic_segment_writer<search_layout> {
public:
	// Adds a document and returns its ordinal. Docids strictly increase, so ordinal order
	// is docid order. `length` is the document length used by weighting (Xapian: the sum
	// of wdf over non-boolean terms). `stored` is the document's stored record, returned
	// by segment::stored. `key` is the document's key (F8), never one of its terms, or empty.
	// Each term appears once, with its wdf and its positions in increasing order.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::uint32_t length, std::string_view stored = {},
		std::uint64_t version = 0, std::string_view key = {}) {
		if (!terms.empty() && !(structures & format::bit::inverted)) { throw std::invalid_argument("strata: terms in a segment without inverted"); }
		const ordinal_t ord = begin_document(docid, length, stored, version, key, false);
		if (structures & format::bit::inverted) { builder<inverted_plugin>().add_terms(ord, terms); }
		return ord;
	}

	// The same from a document's tokens in order, as a tokenizer produces them: each
	// occurrence of a term adds one to its wdf and its position to its positions, so
	// nothing has to group them first. Positions increase.
	using token = inverted_builder::token;
	ordinal_t add_tokens(std::uint64_t docid, std::span<const token> tokens, std::uint32_t length, std::string_view stored = {},
		std::uint64_t version = 0, std::string_view key = {}) {
		if (!tokens.empty() && !(structures & format::bit::inverted)) { throw std::invalid_argument("strata: terms in a segment without inverted"); }
		const ordinal_t ord = begin_document(docid, length, stored, version, key, false);
		if (structures & format::bit::inverted) { builder<inverted_plugin>().add_tokens(ord, tokens); }
		return ord;
	}

private:
	friend struct detail::writer_access;
	// The inverter's entry points (detail::writer_access).
	ordinal_t add_text(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens, std::uint32_t length,
		std::string_view stored, std::uint64_t version, std::string_view key) {
		const ordinal_t ord = begin_document(docid, length, stored, version, key, true);
		if (structures & format::bit::inverted) { builder<inverted_plugin>().add_text(ord, terms, text, tokens); }
		return ord;
	}
	ordinal_t add_split(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::uint32_t length, std::string_view stored,
		std::uint64_t version, std::string_view key) {
		const ordinal_t ord = begin_document(docid, length, stored, version, key, true);
		if (structures & format::bit::inverted) { builder<inverted_plugin>().add_split(ord, terms, text); }
		return ord;
	}
	template <class Next>
	void flush_from(const std::array<std::uint8_t, 16>& segment_id, output& out, spill& positions_spill, std::string_view alphabet, Next&& next) {
		write(segment_id, out, positions_spill, [&](auto& b, section_sink& s) {
			if constexpr (std::is_same_v<std::decay_t<decltype(b)>, inverted_builder>) {
				b.finish(s, positions_spill, alphabet, next);
			} else {
				b.finish(s);
			}
		});
	}

	// The per-document data; returns the document's ordinal. A borrowed key's bytes outlive
	// the flush (keys_builder::add).
	ordinal_t begin_document(std::uint64_t docid, std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key, bool borrowed) {
		stored_plugin::refuse(stored, (structures & format::bit::stored) != 0);
		const ordinal_t ord = begin({docid, version, key}, borrowed);
		if (structures & format::bit::stored) { builder<stored_plugin>().add(stored); }
		if (structures & format::bit::inverted) { (void)builder<inverted_plugin>().begin(length); }
		return ord;
	}
};


namespace detail {
// The inverter's ways into a segment_writer: a document as terms and text tokens or split
// text (the multi-thread flush), per-document data alone, and terms from the tuple buffer.
// Each key is borrowed: its bytes must outlive the writer's flush.
struct writer_access {
	static ordinal_t add_text(segment_writer& w, std::uint64_t docid, std::span<const posting_input> terms, std::string_view text,
		std::span<const text_token> tokens, std::uint32_t length, std::string_view stored = {}, std::uint64_t version = 0, std::string_view key = {}) {
		return w.add_text(docid, terms, text, tokens, length, stored, version, key);
	}
	static ordinal_t add_split(segment_writer& w, std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::uint32_t length,
		std::string_view stored, std::uint64_t version, std::string_view key) {
		return w.add_split(docid, terms, text, length, stored, version, key);
	}
	static ordinal_t add_document(segment_writer& w, std::uint64_t docid, std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		return w.begin_document(docid, length, stored, version, key, true);
	}
	template <class Next>
	static void flush_from(segment_writer& w, const std::array<std::uint8_t, 16>& segment_id, output& out, spill& positions, std::string_view alphabet, Next&& next) {
		w.flush_from(segment_id, out, positions, alphabet, std::forward<Next>(next));
	}
};
}  // namespace detail

}  // namespace prospect
