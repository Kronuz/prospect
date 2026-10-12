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
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
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

// Receives a segment file's bytes in order.
struct output {
	virtual ~output() = default;
	virtual void write(const char* data, std::size_t size) = 0;
	void write(std::string_view bytes) { write(bytes.data(), bytes.size()); }
};

// Holds a section produced alongside another until it is appended.
struct spill : output {
	virtual void copy_to(output& out) = 0;
};

class string_output final : public output {
public:
	explicit string_output(std::string& s) : s_(s) {}
	using output::write;
	void write(const char* data, std::size_t size) override { s_.append(data, size); }

private:
	std::string& s_;
};

class string_spill final : public spill {
public:
	using output::write;
	void write(const char* data, std::size_t size) override { s_.append(data, size); }
	void copy_to(output& out) override {
		out.write(s_);
		std::string().swap(s_);
	}

private:
	std::string s_;
};

// Writes to a file; throws std::runtime_error on failure.
class file_output final : public output {
public:
	explicit file_output(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
		if (!f_) { throw std::runtime_error("strata: cannot create " + path); }
	}
	~file_output() override {
		if (f_) { std::fclose(f_); }
	}
	file_output(const file_output&) = delete;
	file_output& operator=(const file_output&) = delete;
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (std::fwrite(data, 1, size, f_) != size) { throw std::runtime_error("strata: write failed"); }
	}
	// Flushes and closes; throws when the data did not reach the file.
	void close() {
		const bool ok = std::fflush(f_) == 0;
		std::fclose(std::exchange(f_, nullptr));
		if (!ok) { throw std::runtime_error("strata: write failed"); }
	}

private:
	std::FILE* f_;
};

// A temporary file holding a spilled section; removed when copied or destroyed.
class file_spill final : public spill {
public:
	explicit file_spill(std::string path) : path_(std::move(path)), f_(std::fopen(path_.c_str(), "w+b")) {
		if (!f_) { throw std::runtime_error("strata: cannot create " + path_); }
	}
	~file_spill() override { release(); }
	file_spill(const file_spill&) = delete;
	file_spill& operator=(const file_spill&) = delete;
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (std::fwrite(data, 1, size, f_) != size) { throw std::runtime_error("strata: spill write failed"); }
	}
	void copy_to(output& out) override {
		if (std::fflush(f_) != 0 || std::fseek(f_, 0, SEEK_SET) != 0) { throw std::runtime_error("strata: spill read failed"); }
		std::vector<char> buf(1 << 20);
		for (std::size_t n; (n = std::fread(buf.data(), 1, buf.size(), f_)) > 0;) { out.write(buf.data(), n); }
		if (std::ferror(f_)) { throw std::runtime_error("strata: spill read failed"); }
		release();
	}

private:
	void release() {
		if (f_) {
			std::fclose(f_);
			std::remove(path_.c_str());
			f_ = nullptr;
		}
	}
	std::string path_;
	std::FILE* f_;
};

// Tracks bytes written and pads to an alignment.
class counting_output final : public output {
public:
	explicit counting_output(output& out) : out_(out) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		out_.write(data, size);
		size_ += size;
	}
	std::uint64_t size() const noexcept { return size_; }
	void align(std::size_t alignment) {
		static constexpr char zeros[64] = {};
		while (size_ % alignment) { write(zeros, std::min<std::size_t>(sizeof zeros, alignment - size_ % alignment)); }
	}

private:
	output& out_;
	std::uint64_t size_ = 0;
};

// One section's bytes, checksummed as they pass (whole section and 64 KiB regions).
class section_stream {
public:
	explicit section_stream(counting_output& out) : out_(out), offset_(out.size()) {}
	void write(std::string_view bytes) {
		out_.write(bytes);
		crc_.update(bytes.data(), bytes.size());
		for (std::size_t at = 0; at < bytes.size();) {
			const std::size_t n = std::min(bytes.size() - at, format::region_size - region_fill_);
			region_.update(bytes.data() + at, n);
			region_fill_ += n;
			at += n;
			if (region_fill_ == format::region_size) {
				regions_.push_back(region_.value());
				region_ = {};
				region_fill_ = 0;
			}
		}
		length_ += bytes.size();
	}
	std::uint64_t size() const noexcept { return length_; }
	format::section_entry finish(format::section kind) {
		if (region_fill_) { regions_.push_back(region_.value()); }
		return {kind, offset_, length_, crc_.value(), std::move(regions_)};
	}

private:
	counting_output& out_;
	std::uint64_t offset_, length_ = 0;
	hashes::Crc32c crc_, region_;
	std::size_t region_fill_ = 0;
	std::vector<std::uint32_t> regions_;
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
			// Counts are kept only once one differs from its wdf; until then they are the wdfs,
			// whose LEB128 stream is the same bytes.
			if (t.counts_are_wdf && in.positions.size() != in.wdf) {
				t.counts_are_wdf = false;
				t.counts.assign(t.wdfs, 0, t.wdfs.size() - leb_size(in.wdf));
			}
			if (!t.counts_are_wdf) { format::put_leb(t.counts, in.positions.size()); }
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
		std::string file;
		string_output out(file);
		string_spill positions;
		flush(segment_id, out, positions);
		return file;
	}

	// Streams the segment file to out in one pass. The positions section is produced
	// alongside the postings, so it goes to `positions` until the postings end.
	void flush(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill) {
		std::vector<std::pair<std::string_view, std::size_t>> keys;
		keys.reserve(buffers_.size());
		for (const auto& slot : table_) {
			if (slot.key) { keys.push_back({std::string_view(slot.key, slot.length), slot.buffer}); }
		}
		std::sort(keys.begin(), keys.end());

		counting_output out(sink);
		std::vector<format::section_entry> toc;
		{
			const std::string header = format::encode_header(segment_id);
			section_stream h(out);
			h.write(header);
			toc.push_back(h.finish(format::section::header));
		}
		out.align(format::section_alignment);
		section_stream postings(out);
		counting_output spill_out(positions_spill);
		section_stream positions(spill_out);
		termdict::builder dict;
		std::vector<std::uint32_t> ords, wdfs, counts, pos;
		std::string term_postings, term_positions;
		for (auto [key, buffer] : keys) {
			term_buffer& t = buffers_[buffer];
			decode_all(t.ords, ords, t.df);
			for (std::size_t i = 1; i < ords.size(); ++i) { ords[i] += ords[i - 1]; }
			for (auto& o : ords) { --o; }  // stored as ordinal + 1 deltas
			decode_all(t.wdfs, wdfs, t.df);
			if (t.counts_are_wdf) {
				counts = wdfs;
			} else {
				decode_all(t.counts, counts, t.df);
			}

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

			positions_layout layout;
			if (t.any_positions) {
				info.positions = positions.size();
				std::size_t total = 0;
				for (auto c : counts) { total += c; }
				decode_all(t.positions, pos, total);
				term_positions.clear();
				layout = write_positions(term_positions, counts, pos, t.counts_are_wdf);
				info.positions_length = term_positions.size();
				positions.write(term_positions);
			}
			if (t.df == 1) {
				info.flags |= format::single_posting;
				info.postings = ords[0];
				info.single_wdf = wdfs[0];
			} else {
				info.postings = postings.size();
				term_postings.clear();
				write_postings(term_postings, ords, wdfs, !t.all_zero_wdf, counts, t.any_positions ? &layout : nullptr, !t.counts_are_wdf);
				info.postings_length = term_postings.size();
				postings.write(term_postings);
			}
			dict.add(key, format::encode_term_info(info));
		}
		toc.push_back(postings.finish(format::section::postings));
		out.align(format::section_alignment);
		{
			auto entry = positions.finish(format::section::positions);
			entry.offset = out.size();
			positions_spill.copy_to(out);
			toc.push_back(std::move(entry));
		}
		auto tables = dict.finish();
		std::string stats;
		format::put<std::uint64_t>(stats, lengths_.size());
		format::put<std::uint64_t>(stats, total_length_);
		format::put<std::uint32_t>(stats, lengths_.empty() ? 0 : *std::min_element(lengths_.begin(), lengths_.end()));
		format::put<std::uint32_t>(stats, lengths_.empty() ? 0 : *std::max_element(lengths_.begin(), lengths_.end()));
		auto append = [&](format::section kind, const std::string& bytes) {
			out.align(format::section_alignment);
			section_stream sec(out);
			sec.write(bytes);
			toc.push_back(sec.finish(kind));
		};
		append(format::section::terms, tables.blocks);
		append(format::section::term_index, tables.index);
		append(format::section::lengths, pack_u32(lengths_));
		append(format::section::docids, encode_docids());
		append(format::section::stats, stats);
		out.align(format::section_alignment);

		format::footer f;
		f.toc_offset = out.size();
		std::string toc_bytes = format::encode_toc(toc);
		f.toc_length = toc_bytes.size();
		f.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
		f.doc_count = std::uint32_t(lengths_.size());
		f.min_docid = docids_.empty() ? 0 : *std::min_element(docids_.begin(), docids_.end());
		f.max_docid = docids_.empty() ? 0 : *std::max_element(docids_.begin(), docids_.end());
		out.write(toc_bytes);
		out.write(format::encode_footer(f));
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
		for (std::size_t b = 0; b < full; b += format::block) {
			const std::size_t start = blocks.size();
			bitpack::delta_encode(prev, ords.data() + b, deltas);
			blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_for(deltas, buf));
			if (with_wdf) { blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(wdfs.data() + b, buf)); }
			std::vector<format::impact> pairs;
			for (std::size_t i = b; i < b + format::block; ++i) { pairs.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			meta.push_back({ords[b + format::block - 1], blocks.size() - start, position, format::competitive(std::move(pairs))});
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
		// Level 0, grouped by 32 blocks; level 1 locates each group. Positional fields
		// (first position index, position block offset, count block offset) are present only
		// for terms with positions, and counts only when they differ from wdf.
		const bool with_positions = layout != nullptr;
		with_counts = with_counts && with_positions;
		std::string level1, level0;
		const std::size_t groups = (meta.size() + 31) / 32;
		ordinal_t prev_last = 0, prev_group_last = 0;
		block_meta prev_group{};
		for (std::size_t g = 0; g < groups; ++g) {
			std::string group;
			const block_meta& first = meta[g * 32];
			block_meta prev = first;  // level-0 deltas restart per group
			std::uint64_t group_bytes = 0;
			std::vector<format::impact> group_pairs;
			for (std::size_t i = g * 32; i < std::min(meta.size(), g * 32 + 32); ++i) {
				format::put_leb(group, meta[i].last - (i ? prev_last : 0));
				format::put_leb(group, meta[i].bytes);
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
			const ordinal_t group_last = meta[std::min(meta.size(), g * 32 + 32) - 1].last;
			format::put_leb(level1, group_last - (g ? prev_group_last : 0));
			if (with_positions) {
				format::put_leb(level1, first.first_position - prev_group.first_position);
				format::put_leb(level1, first.pos_offset - prev_group.pos_offset);
				if (with_counts) { format::put_leb(level1, first.count_offset - prev_group.count_offset); }
			}
			prev_group = first;
			format::put_leb(level1, group.size());
			format::put_leb(level1, group_bytes);
			format::encode_sized_impacts(level1, format::competitive(std::move(group_pairs)));
			prev_group_last = group_last;
			level0 += group;
		}
		format::put_leb(out, groups);
		if (with_positions) {
			format::put_leb(out, position);  // the tail's first position index and its block offset
			format::put_leb(out, layout->offset_of(position));
		}
		out += level1;
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
