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
#include "strata/format.hh"
#include "termdict.hh"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace strata {

// One term of a document being added.
struct posting_input {
	std::string_view term;
	std::uint32_t wdf = 1;
	std::span<const std::uint32_t> positions;  // sorted, unique; empty for positionless terms
};

class segment_writer {
public:
	// Adds a document and returns its ordinal. `length` is the document length used by
	// weighting (Xapian: the sum of wdf over non-boolean terms).
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::uint32_t length) {
		const ordinal_t ord = ordinal_t(lengths_.size());
		lengths_.push_back(length);
		docids_.push_back(docid);
		total_length_ += length;
		for (const auto& in : terms) {
			term_buffer& t = buffer_for(in.term);
			if (t.df && t.last == ord) { throw std::invalid_argument("strata: term repeated within a document"); }
			format::put_leb(t.ords, t.df ? ord - t.last : ord + 1);
			format::put_leb(t.wdfs, in.wdf);
			t.last = ord;
			++t.df;
			t.cf += in.wdf;
			t.max_wdf = std::max(t.max_wdf, in.wdf);
			if (in.wdf) { t.all_zero_wdf = false; }
			if (!in.positions.empty()) { t.any_positions = true; }
			if (in.positions.size() != in.wdf) { t.counts_are_wdf = false; }
			format::put_leb(t.counts, in.positions.size());
			std::uint32_t prev = 0;
			for (auto p : in.positions) {
				format::put_leb(t.positions, p - prev);
				prev = p;
			}
		}
		return ord;
	}

	std::size_t doc_count() const noexcept { return lengths_.size(); }

	// Writes the segment file and returns its bytes.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id) {
		std::vector<std::pair<std::string_view, std::size_t>> keys;
		keys.reserve(buffers_.size());
		for (const auto& slot : table_) {
			if (slot.key) { keys.push_back({std::string_view(slot.key, slot.length), slot.buffer}); }
		}
		std::sort(keys.begin(), keys.end());

		std::string postings, positions;
		termdict::builder dict;
		std::vector<std::uint32_t> ords, wdfs, counts, pos;
		for (auto [key, buffer] : keys) {
			term_buffer& t = buffers_[buffer];
			decode_all(t.ords, ords, t.df);
			for (std::size_t i = 1; i < ords.size(); ++i) { ords[i] += ords[i - 1]; }
			for (auto& o : ords) { --o; }  // stored as ordinal + 1 deltas
			decode_all(t.wdfs, wdfs, t.df);
			decode_all(t.counts, counts, t.df);

			format::term_info info;
			info.df = t.df;
			info.cf = t.cf;
			info.max_wdf = t.max_wdf;
			if (t.all_zero_wdf) { info.flags |= format::no_wdf; }
			if (!t.any_positions) { info.flags |= format::no_positions; }
			if (t.counts_are_wdf) { info.flags |= format::counts_are_wdf; }

			std::vector<format::impact> all;
			all.reserve(ords.size());
			for (std::size_t i = 0; i < ords.size(); ++i) { all.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			info.impacts = format::competitive(all);

			if (t.df == 1) {
				info.flags |= format::single_posting;
				info.postings = ords[0];
				info.single_wdf = wdfs[0];
			} else {
				info.postings = postings.size();
				write_postings(postings, ords, wdfs, !t.all_zero_wdf, counts);
			}
			if (t.any_positions) {
				info.positions = positions.size();
				std::size_t total = 0;
				for (auto c : counts) { total += c; }
				decode_all(t.positions, pos, total);
				write_positions(positions, counts, pos, t.counts_are_wdf);
			}
			dict.add(key, format::encode_term_info(info));
		}
		auto tables = dict.finish();

		std::string lengths = pack_u32(lengths_);
		std::string docids = encode_docids();
		std::string stats;
		format::put<std::uint64_t>(stats, lengths_.size());
		format::put<std::uint64_t>(stats, total_length_);
		format::put<std::uint32_t>(stats, lengths_.empty() ? 0 : *std::min_element(lengths_.begin(), lengths_.end()));
		format::put<std::uint32_t>(stats, lengths_.empty() ? 0 : *std::max_element(lengths_.begin(), lengths_.end()));

		std::string file = format::encode_header(segment_id);
		std::vector<format::section_entry> toc;
		toc.push_back(format::describe(format::section::header, 0, file));
		auto append = [&](format::section kind, const std::string& bytes) {
			while (file.size() % format::section_alignment) { file.push_back('\0'); }
			toc.push_back(format::describe(kind, file.size(), bytes));
			file += bytes;
		};
		append(format::section::terms, tables.blocks);
		append(format::section::term_index, tables.index);
		append(format::section::postings, postings);
		append(format::section::positions, positions);
		append(format::section::lengths, lengths);
		append(format::section::docids, docids);
		append(format::section::stats, stats);
		while (file.size() % format::section_alignment) { file.push_back('\0'); }

		format::footer f;
		f.toc_offset = file.size();
		std::string toc_bytes = format::encode_toc(toc);
		f.toc_length = toc_bytes.size();
		f.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
		f.doc_count = std::uint32_t(lengths_.size());
		f.min_docid = docids_.empty() ? 0 : *std::min_element(docids_.begin(), docids_.end());
		f.max_docid = docids_.empty() ? 0 : *std::max_element(docids_.begin(), docids_.end());
		file += toc_bytes;
		file += format::encode_footer(f);
		return file;
	}

private:
	struct term_buffer {
		std::string ords, wdfs, counts, positions;  // LEB128 streams while buffering
		ordinal_t last = 0;
		std::uint32_t df = 0, max_wdf = 0;
		std::uint64_t cf = 0;
		bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	};

	// Open addressing with linear probing over stored hashes; grows at half load.
	struct slot {
		std::uint64_t hash = 0;
		const char* key = nullptr;  // in arena_
		std::uint32_t length = 0;
		std::uint32_t buffer = 0;
	};

	term_buffer& buffer_for(std::string_view term) {
		if (table_.empty()) { table_.resize(1 << 16); }
		const std::uint64_t h = ::wordwise::hash(term.data(), term.size());
		std::size_t mask = table_.size() - 1;
		for (std::size_t i = h & mask;; i = (i + 1) & mask) {
			slot& sl = table_[i];
			if (!sl.key) { break; }
			if (sl.hash == h && sl.length == term.size() && std::memcmp(sl.key, term.data(), term.size()) == 0) { return buffers_[sl.buffer]; }
		}
		// Term bytes live in stable arena chunks so slots can point at them.
		if (arena_.empty() || arena_.back().size() + term.size() > arena_.back().capacity()) {
			arena_.emplace_back();
			arena_.back().reserve(std::max<std::size_t>(1 << 20, term.size()));
		}
		std::string& chunk = arena_.back();
		const char* at = chunk.data() + chunk.size();
		chunk.append(term);
		if ((buffers_.size() + 1) * 2 > table_.size()) { grow(); }
		insert({h, at, std::uint32_t(term.size()), std::uint32_t(buffers_.size())});
		return buffers_.emplace_back();
	}

	void insert(const slot& s) {
		const std::size_t mask = table_.size() - 1;
		std::size_t i = s.hash & mask;
		while (table_[i].key) { i = (i + 1) & mask; }
		table_[i] = s;
	}

	void grow() {
		std::vector<slot> old(table_.size() * 2);
		old.swap(table_);
		for (const auto& s : old) {
			if (s.key) { insert(s); }
		}
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
	};

	void write_postings(std::string& out, const std::vector<std::uint32_t>& ords, const std::vector<std::uint32_t>& wdfs, bool with_wdf,
		const std::vector<std::uint32_t>& counts) {
		const std::size_t full = ords.size() / format::block * format::block;
		std::string blocks;
		std::vector<block_meta> meta;
		std::uint32_t deltas[format::block];
		std::uint8_t buf[bitpack::max_encoded_bytes];
		std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();  // the first delta is ordinal + 1
		std::uint64_t position = 0;
		for (std::size_t b = 0; b < full; b += format::block) {
			const std::size_t start = blocks.size();
			bitpack::delta_encode(prev, ords.data() + b, deltas);
			blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_for(deltas, buf));
			if (with_wdf) { blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(wdfs.data() + b, buf)); }
			std::vector<format::impact> pairs;
			for (std::size_t i = b; i < b + format::block; ++i) { pairs.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			meta.push_back({ords[b + format::block - 1], blocks.size() - start, position, format::competitive(std::move(pairs))});
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
		// Level 0, grouped by 32 blocks; level 1 locates each group.
		std::string level1, level0;
		const std::size_t groups = (meta.size() + 31) / 32;
		ordinal_t prev_last = 0, prev_group_last = 0;
		std::uint64_t prev_group_first_position = 0;
		for (std::size_t g = 0; g < groups; ++g) {
			std::string group;
			const std::uint64_t group_first_position = meta[g * 32].first_position;
			std::uint64_t prev_first_position = group_first_position;  // level-0 deltas restart per group
			std::uint64_t group_bytes = 0;
			std::vector<format::impact> group_pairs;
			for (std::size_t i = g * 32; i < std::min(meta.size(), g * 32 + 32); ++i) {
				format::put_leb(group, meta[i].last - (i ? prev_last : 0));
				format::put_leb(group, meta[i].bytes);
				format::put_leb(group, meta[i].first_position - prev_first_position);
				format::encode_impacts(group, meta[i].impacts);
				prev_last = meta[i].last;
				prev_first_position = meta[i].first_position;
				group_bytes += meta[i].bytes;
				group_pairs.insert(group_pairs.end(), meta[i].impacts.begin(), meta[i].impacts.end());
			}
			const ordinal_t group_last = meta[std::min(meta.size(), g * 32 + 32) - 1].last;
			format::put_leb(level1, group_last - (g ? prev_group_last : 0));
			format::put_leb(level1, group_first_position - prev_group_first_position);
			prev_group_first_position = group_first_position;
			format::put_leb(level1, group.size());
			format::put_leb(level1, group_bytes);
			format::encode_impacts(level1, format::competitive(std::move(group_pairs)));
			prev_group_last = group_last;
			level0 += group;
		}
		format::put_leb(out, groups);
		format::put_leb(out, position);  // first position index of the tail
		out += level1;
		out += level0;
		out += blocks;
		out += tail;
	}

	static void write_positions(std::string& out, const std::vector<std::uint32_t>& counts, const std::vector<std::uint32_t>& pos, bool counts_are_wdf) {
		std::uint8_t buf[bitpack::max_encoded_bytes];
		if (!counts_are_wdf) {
			const std::size_t full = counts.size() / format::block * format::block;
			std::string count_blocks;
			for (std::size_t b = 0; b < full; b += format::block) {
				std::size_t n = bitpack::encode_pfor(counts.data() + b, buf);
				format::put_leb(out, n);
				count_blocks.append(reinterpret_cast<char*>(buf), n);
			}
			out += count_blocks;
			for (std::size_t i = full; i < counts.size(); ++i) { format::put_leb(out, counts[i]); }
		}
		const std::size_t full = pos.size() / format::block * format::block;
		std::string blocks, sizes;
		for (std::size_t b = 0; b < full; b += format::block) {
			std::size_t n = bitpack::encode_pfor(pos.data() + b, buf);
			format::put_leb(sizes, n);
			blocks.append(reinterpret_cast<char*>(buf), n);
		}
		format::put_leb(out, full / format::block);
		out += sizes;
		out += blocks;
		for (std::size_t i = full; i < pos.size(); ++i) { format::put_leb(out, pos[i]); }
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

	// docids: [kind u8] then, for contiguous ids (docid = base + ordinal), [base u64];
	// otherwise [count u64][docids u64 each].
	std::string encode_docids() const {
		std::string out;
		bool contiguous = true;
		for (std::size_t i = 1; i < docids_.size() && contiguous; ++i) { contiguous = docids_[i] == docids_[0] + i; }
		if (contiguous) {
			out.push_back(char(0));
			format::put<std::uint64_t>(out, docids_.empty() ? 1 : docids_[0]);
		} else {
			out.push_back(char(1));
			format::put<std::uint64_t>(out, docids_.size());
			for (auto d : docids_) { format::put<std::uint64_t>(out, d); }
		}
		return out;
	}

	std::deque<std::string> arena_;
	std::vector<slot> table_;
	std::deque<term_buffer> buffers_;
	std::vector<std::uint32_t> lengths_;
	std::vector<std::uint64_t> docids_;
	std::uint64_t total_length_ = 0;
};

}  // namespace strata
