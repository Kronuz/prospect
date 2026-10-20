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

// An index's buffer at one flush thread (docs/research/indexing.md, inverter C, batched
// into sorted runs): each token becomes a 20-byte tuple (hash, ordinal, offset in its
// document's text, position and length) in one of 256 buckets by the hash's top byte, the
// text copied once, and nothing is looked up per token. Every batch of about a million
// tuples is grouped bucket by bucket with a small table and a stable counting sort, each
// token checked against its term's bytes (equal hashes never merge two terms), and the
// batch's terms, sorted, become a run: one compressed entry of postings per term, written
// in one sequential piece. No term table outlives a batch. The flush merges the runs by
// term, reading each one in order, and hands each term's postings to
// segment_writer::flush_from in ordinal order: the segment a segment_writer given the
// same documents writes. Internal to the index: its inputs are documents that passed stage 1.
#pragma once

#include "strata/search/writer.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace strata {

namespace tuple_detail {

// Appended to in place and read in order. The first chunk starts at 8 entries and doubles
// to a whole chunk, copied as it grows; later chunks never move. clear() keeps the chunks.
template <class T, unsigned Shift = 12>
class chunked {
public:
	static constexpr std::size_t chunk = std::size_t(1) << Shift;
	void push_back(const T& v, flush_charge* memory) {
		if (left_ == 0) { more(memory); }
		*at_++ = v;
		--left_;
		++size_;
	}
	T& operator[](std::size_t i) noexcept { return chunks_[i >> Shift][i & (chunk - 1)]; }
	const T& operator[](std::size_t i) const noexcept { return chunks_[i >> Shift][i & (chunk - 1)]; }
	std::size_t size() const noexcept { return size_; }
	void clear() noexcept { size_ = 0, left_ = 0, at_ = nullptr; }

private:
	void more(flush_charge* memory) {
		if (size_ < chunk) {
			if (first_ == size_) {  // the first chunk grows; uninitialized: every slot is written before it is read
				const std::size_t grown = std::min(chunk, std::max<std::size_t>(8, 2 * first_));
				std::unique_ptr<T[]> bigger(new T[grown]);
				if (size_) { std::copy(chunks_[0].get(), chunks_[0].get() + size_, bigger.get()); }
				if (chunks_.empty()) { chunks_.emplace_back(); }
				chunks_[0] = std::move(bigger);
				if (memory) { memory->grow((grown - first_) * sizeof(T)); }
				first_ = grown;
			}
			at_ = chunks_[0].get() + size_;
			left_ = first_ - size_;
			return;
		}
		if (size_ >> Shift == chunks_.size()) {
			chunks_.push_back(std::unique_ptr<T[]>(new T[chunk]));
			if (memory) { memory->grow(chunk * sizeof(T)); }
		}
		at_ = chunks_[size_ >> Shift].get();
		left_ = chunk;
	}
	std::vector<std::unique_ptr<T[]>> chunks_;
	T* at_ = nullptr;  // the next slot in the current chunk
	std::size_t size_ = 0, left_ = 0;
	std::size_t first_ = 0;  // the first chunk's entries
};

// The k-th of a series of allocations that starts at `first` bytes and doubles to `most`;
// the shift is capped, so any k is defined.
inline std::size_t doubled(std::size_t first, std::size_t k, std::size_t most) noexcept {
	return std::min(most, first << std::min<std::size_t>(k, std::size_t(std::countl_zero(first))));
}

// Bytes appended in pieces that never move; a piece is addressed by its 64-bit offset.
// Pages start at 4 KiB and double to 1 MiB, so a small buffer allocates little.
class byte_log {
public:
	static constexpr unsigned shift = 20;  // offsets: page << 20 | offset; a piece never spans two pages
	std::uint64_t append(std::size_t n, flush_charge* memory) {
		if (pages_.empty() || fill_ + n > size_) {
			size_ = std::max(doubled(4096, pages_.size(), std::size_t(1) << shift), n);  // a larger piece takes a page of its own
			pages_.push_back(std::unique_ptr<char[]>(new char[size_]));
			if (memory) { memory->grow(size_); }
			fill_ = 0;
		}
		const std::uint64_t at = (std::uint64_t(pages_.size() - 1) << shift) | fill_;
		fill_ += n;
		return at;
	}
	char* at(std::uint64_t offset) noexcept { return pages_[offset >> shift].get() + (offset & ((std::uint64_t(1) << shift) - 1)); }
	const char* at(std::uint64_t offset) const noexcept { return pages_[offset >> shift].get() + (offset & ((std::uint64_t(1) << shift) - 1)); }

private:
	std::vector<std::unique_ptr<char[]>> pages_;
	std::size_t fill_ = 0, size_ = 0;  // the last page's fill and size
};

// The bytes, zero-padded to a word, through a multiply and an xorshift per word; a word
// load may read past the term, so the bytes come from the padded text arena.
inline std::uint64_t hash(const char* p, std::size_t len) noexcept {
	constexpr std::uint64_t P = 0x9E3779B97F4A7C15ull;
	std::uint64_t w;
	if (len <= 8) {
		std::memcpy(&w, p, 8);
		w &= ~std::uint64_t(0) >> (64 - 8 * len);
		w *= P;
		w ^= w >> 29;
		return w + std::uint64_t(len) * 0xD6E8FEB86659FD93ull;
	}
	std::uint64_t h = std::uint64_t(len) * P;
	std::size_t i = 0;
	for (; i + 8 < len; i += 8) {
		std::memcpy(&w, p + i, 8);
		h = (h ^ w) * P;
		h ^= h >> 29;
	}
	std::memcpy(&w, p + len - 8, 8);  // the last word, overlapping the one before
	h = (h ^ w) * P;
	return h ^ (h >> 32);
}

inline void put_leb(char*& p, std::uint64_t v) noexcept {
	while (v >= 0x80) {
		*p++ = char(v | 0x80);
		v >>= 7;
	}
	*p++ = char(v);
}
inline std::uint64_t get_leb(const char*& p) noexcept {
	std::uint64_t v = 0;
	for (unsigned s = 0;; s += 7) {
		const std::uint8_t b = std::uint8_t(*p++);
		v |= std::uint64_t(b & 0x7F) << s;
		if (b < 0x80) { return v; }
	}
}

}  // namespace tuple_detail

namespace detail {
// Preconditions, which stage 1 checks and add() asserts in debug builds: terms are non-empty
// and unique, tokens are non-empty and within the text, positions strictly increase, and no
// token equals a terms entry. A zero-length token would shift the hash by 64 and a repeated
// term would write corrupt position deltas.
class tuple_buffer {
public:
	// Where the buffer reports its allocations (D17); none: nothing is counted.
	flush_charge* memory = nullptr;
	// Every hash is masked with it: tests narrow it to make terms collide.
	std::uint64_t hash_mask = ~std::uint64_t(0);
	// A token at or past this offset of its text takes a side entry (tuple offsets are 32
	// bits); tests lower it.
	std::uint64_t tuple_offsets = std::uint64_t(1) << 32;
	// Tuples grouped into a run at a time: about 20 MiB of them by default, so a bucket's
	// share (1/256) stays in L2.
	std::size_t batch_tuples = std::size_t(1) << 20;
	// A run's bytes at most, run_cap or less: a batch's run is cut between terms at it, so a run
	// of more than one entry keeps its 32-bit marks; an entry past it takes a run alone (64-bit
	// lengths). Tests lower it.
	static constexpr std::uint64_t run_cap = std::numeric_limits<std::uint32_t>::max();
	std::uint64_t max_run_bytes = run_cap;
	// Any one entry's bytes at most, for `tuples` terms and tokens with `given` positions and
	// the longest term `longest` (encode_entry: its term, 46, 20 a tuple, 5 a given position).
	static constexpr std::uint64_t entry_bound(std::uint64_t tuples, std::uint64_t given, std::uint64_t longest) noexcept {
		return 46 + longest + 20 * tuples + 5 * given;
	}

	// A document: its terms, each once with its wdf and positions, and its text's tokens in
	// order, each adding one to its term's wdf; a term is in one of the two and positions
	// increase. `key` is the document's key (F8), never one of its terms, or empty. Returns
	// its ordinal.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens,
		std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		return add_with(docid, terms, text, stored, version, key, tokens.size(), [&](auto&& token, std::string_view) {
			for (const auto& tk : tokens) { token(tk.offset, tk.size, tk.position); }
			return length;
		});
	}
	// A document whose tokens are its text's words (split_words) at positions 1, 2, ..., its
	// length their count; the text is split where it was copied. Stage 1 checked the words
	// (text_detail::splittable).
	ordinal_t add_split(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::string_view stored, std::uint64_t version,
		std::string_view key) {
		return add_with(docid, terms, text, stored, version, key, text.size(), [&](auto&& token, std::string_view copy) {
			std::uint32_t position = 0;
			text_detail::split_words(copy, [&](std::size_t offset, std::size_t size) { token(offset, std::uint32_t(size), ++position); });
			return position;
		});
	}

private:
	// add()'s body: `tokens(token, copy)` calls token(offset, size, position) for each token of
	// the copied text and returns the document's length; at most `most` tokens.
	template <class Tokens>
	ordinal_t add_with(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::string_view stored, std::uint64_t version,
		std::string_view key, std::uint64_t most, Tokens&& tokens) {
		if (docids_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (stored.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		if (batched_ >= batch_tuples) { drain(); }
		const ordinal_t ord = ordinal_t(docids_.size());
		std::size_t entry_bytes = 0;
		std::uint64_t given_positions = 0, longest_term = 0;
		for (const auto& in : terms) {
			entry_bytes += in.term.size();
			given_positions += in.positions.size();
			longest_term = std::max<std::uint64_t>(longest_term, in.term.size());
		}
		reserve_entry(terms.size() + most, given_positions, std::max<std::uint64_t>(longest_term, text.size()));
		char* base = room(text.size() + entry_bytes);
		if (!text.empty()) { std::memcpy(base, text.data(), text.size()); }
#ifndef NDEBUG
		std::optional<std::uint32_t> prev_position;
		auto is_term = [&](std::string_view t) { return std::any_of(terms.begin(), terms.end(), [&](const posting_input& in) { return in.term == t; }); };
		{
			std::vector<std::string_view> sorted;  // the terms, to find repeats
			for (const auto& in : terms) {
				assert(std::adjacent_find(in.positions.begin(), in.positions.end(), std::greater_equal<>()) == in.positions.end());
				sorted.push_back(in.term);
			}
			std::sort(sorted.begin(), sorted.end());
			assert(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
		}
#endif
		auto token = [&](std::uint64_t offset, std::uint32_t size, std::uint32_t position) {
			const char* p = base + offset;
			assert(size != 0 && offset + size <= text.size() && (!prev_position || position > *prev_position) && !is_term({p, size}));
#ifndef NDEBUG
			prev_position = position;
#endif
			const std::uint64_t h = tuple_detail::hash(p, size) & hash_mask;
			if (size < long_term && position < (std::uint32_t(1) << 24) && offset < tuple_offsets) {
				push(tuple(h, ord, std::uint32_t(offset), position << 8 | size));
			} else {
				side_.push_back({p, size, 1, 1, positions_.size()}, memory);
				positions_.push_back(position, memory);
				push(tuple(h, ord, std::uint32_t(side_.size() - 1), long_term));
			}
		};
		const std::uint32_t length = tokens(token, std::string_view(base, text.size()));
		char* at = base + text.size();
		for (const auto& in : terms) {
			assert(!in.term.empty());
			std::memcpy(at, in.term.data(), in.term.size());
			side_.push_back({at, std::uint32_t(in.term.size()), in.wdf, std::uint32_t(in.positions.size()), positions_.size()}, memory);
			for (auto p : in.positions) { positions_.push_back(p, memory); }
			push(tuple(tuple_detail::hash(at, in.term.size()) & hash_mask, ord, std::uint32_t(side_.size() - 1), long_term));
			at += in.term.size();
		}
		docids_.push_back(docid);
		versions_.push_back(version);
		lengths_.push_back(length);
		keys_ += key;
		key_ends_.push_back(keys_.size());
		texts_.push_back(base);
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		charge_columns();
		return ord;
	}

	// Drains first when one entry of the batch could pass max_run_bytes with this document in
	// it (entry_bound), so an entry past the cap holds one document's postings; the cut alone
	// keeps every run correct.
	void reserve_entry(std::uint64_t tuples, std::uint64_t given_positions, std::uint64_t longest) {
		if (entry_bound(batched_ + tuples, entry_positions_ + given_positions, std::max(entry_term_, longest)) > max_run_bytes) {
			drain();  // also clears entry_term_ left by documents without tuples
		}
		entry_term_ = std::max(entry_term_, longest);
		entry_positions_ += given_positions;
	}

public:
	std::size_t doc_count() const noexcept { return docids_.size(); }
	std::size_t run_count() const noexcept { return runs_.size(); }  // runs drained so far
	// Whether `docs` more documents with `entries` terms and tokens in all still fit the
	// 32-bit ordinals and side-entry indexes; the index flushes first when they do not.
	bool fits(std::size_t docs, std::size_t entries) const noexcept {
		return docids_.size() + docs < std::size_t(no_more_docs) - 1 && side_.size() + entries < max_side;
	}
	std::uint64_t docid(ordinal_t ord) const { return docids_.at(ord); }
	std::uint64_t version(ordinal_t ord) const { return versions_.at(ord); }
	std::uint32_t length(ordinal_t ord) const { return lengths_.at(ord); }
	std::string_view stored(ordinal_t ord) const {
		const std::uint64_t begin = ord ? stored_ends_.at(ord - 1) : 0;
		return std::string_view(stored_).substr(begin, stored_ends_.at(ord) - begin);
	}
	std::string_view key(ordinal_t ord) const {
		const std::uint64_t begin = ord ? key_ends_.at(ord - 1) : 0;
		return std::string_view(keys_).substr(begin, key_ends_.at(ord) - begin);
	}

	// Writes to `out` the segment of the documents at `order` (ordinals, docids increasing),
	// or of every one in ordinal order when `order` is empty (their docids must increase),
	// through `w`, a writer holding no documents yet; positions gather in `positions` until
	// the postings end. The batch drains first; a second flush writes the same bytes.
	void flush(const std::array<std::uint8_t, 16>& segment_id, std::span<const ordinal_t> order, segment_writer& w, output& out, spill& positions) {
		drain_for_flush(w.memory);
		const flush_plan p = plan(order, w.memory);
		add_documents(w, p);
		merger m(*this, {}, {});
		term_postings old;
		std::vector<live_posting> live;
		writer_access::flush_from(w, segment_id, out, positions, alphabet(p), [&](term_postings& tp) -> std::optional<std::string_view> {
			std::string_view term;
			while (m.next(term)) {
				if (postings_of(m.entries(), p, tp, old, live)) { return term; }
			}
			return std::nullopt;
		});
	}

	// --- The flush's steps, which a flush on several threads (perf/indexing-gap) takes
	// itself: drain_for_flush(), plan(), add_documents() into its writer, alphabet(),
	// splitters(), and each_term() over disjoint key ranges, from any threads at once once the
	// buffer drained.

	// Which documents the flush writes, and their ordinals in it.
	struct flush_plan {
		bool all = true;                  // every document, in ordinal order
		bool monotone = true;             // the new ordinals keep the old order
		std::vector<ordinal_t> order;     // the documents written, by new ordinal
		std::vector<ordinal_t> renumber;  // old ordinal -> new, or no_more_docs when left out
	};
	// Drains the last batch, its allocations charged to `run` (the flush's) when given.
	void drain_for_flush(flush_charge* run) {
		struct restore {
			flush_charge*& at;
			flush_charge* was;
			~restore() { at = was; }
		} r{memory, std::exchange(memory, run ? run : memory)};
		drain();
	}
	flush_plan plan(std::span<const ordinal_t> order, flush_charge* run = nullptr) const {
		flush_plan p;
		if (order.empty()) { return p; }
		p.all = false;
		p.order.assign(order.begin(), order.end());
		p.renumber.assign(doc_count(), no_more_docs);
		for (std::size_t i = 0; i < order.size(); ++i) {
			p.renumber[order[i]] = ordinal_t(i);
			p.monotone = p.monotone && (i == 0 || order[i] > order[i - 1]);
		}
		if (run) { run->grow((p.renumber.capacity() + p.order.capacity()) * sizeof(ordinal_t)); }
		return p;
	}
	// The documents' columns into `w`, in the flush's order.
	void add_documents(segment_writer& w, const flush_plan& p) const {
		const std::size_t docs = p.all ? doc_count() : p.order.size();
		for (std::size_t i = 0; i < docs; ++i) {
			const ordinal_t o = p.all ? ordinal_t(i) : p.order[i];
			writer_access::add_document(w, docids_[o], lengths_[o], stored(o), versions_[o], key(o));
		}
	}
	// Every byte the written terms use, in order: the runs' terms, or, when documents are
	// left out, those with a posting left (a pass over the runs).
	std::string alphabet(const flush_plan& p) const {
		std::array<bool, 256> used = used_;
		if (!p.all) {
			used = {};
			each_term({}, {}, p, [&](std::string_view term, term_postings&) {
				for (char c : term) { used[std::uint8_t(c)] = true; }
			});
		}
		std::string a;
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { a.push_back(char(c)); }
		}
		return a;
	}
	// Up to ranges - 1 increasing terms that split the runs' terms into ranges of about equal
	// counts, sampled from the runs' marks (every 64th entry).
	std::vector<std::string_view> splitters(std::size_t ranges) const {
		std::vector<std::string_view> sample;
		for (const run_ref& r : runs_) {
			const char* base = log_.at(r.offset);
			for (std::size_t m = r.first_mark; m < r.first_mark + r.marks; ++m) { sample.push_back(view(base + marks_[m]).term); }
		}
		std::sort(sample.begin(), sample.end());
		std::vector<std::string_view> out;
		for (std::size_t k = 1; k < ranges && !sample.empty(); ++k) { out.push_back(sample[sample.size() * k / ranges]); }
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}
	// Calls f(term, tp) for each term in [lo, hi) in byte order (empty: no bound), tp its
	// postings in the plan's numbering; terms with none left are skipped.
	template <class F>
	void each_term(std::string_view lo, std::string_view hi, const flush_plan& p, F&& f) const {
		merger m(*this, lo, hi);
		term_postings tp, old;
		std::vector<live_posting> live;
		std::string_view term;
		while (m.next(term)) {
			if (postings_of(m.entries(), p, tp, old, live)) { f(term, tp); }
		}
	}

	// Calls f(term, ord, wdf, positions) for every posting, in no particular order, with few
	// allocations (the digest calls it often).
	template <class F>
	void scan(F&& f) const {
		std::vector<std::uint32_t> absolute;
		term_postings tp;
		for (const run_ref& r : runs_) {
			const char* p = log_.at(r.offset);
			for (std::uint32_t e = 0; e < r.entries; ++e) {
				const entry_view v = view(p);
				tp.clear();
				decode_entry(v, tp, ~std::uint32_t(0));
				std::size_t at = 0;
				for (std::size_t i = 0; i < tp.ords.size(); ++i) {
					absolute.clear();
					std::uint32_t pos = 0;
					for (std::uint32_t c = 0; c < tp.counts[i]; ++c) { absolute.push_back(pos += tp.positions[at++]); }
					f(v.term, tp.ords[i], tp.wdfs[i], std::span<const std::uint32_t>(absolute));
				}
				p = v.end;
			}
		}
		// The batch not drained yet: each bucket grouped on a copy, the buffer left as it is.
		std::vector<tuple> copy;
		std::vector<group> groups;
		group_scratch sc;
		auto bucket_at = [&](std::size_t b) -> const std::vector<tuple>& { return buckets_ ? (*buckets_)[b] : flat_; };
		for (std::size_t b = 0; b < (buckets_ ? 256u : 1u); ++b) {
			const std::vector<tuple>& bucket = bucket_at(b);
			copy.assign(bucket.begin(), bucket.end());
			groups.clear();
			group_run(copy.data(), copy.size(), groups, sc);
			for (const group& g : groups) {
				for (const tuple* t = g.begin; t != g.end;) {
					const ordinal_t ord = t->ord;
					std::uint32_t wdf = 0;
					absolute.clear();
					for (; t != g.end && t->ord == ord; ++t) {
						if (t->len() == long_term) {
							const side_entry& se = side_[t->off];
							wdf += se.wdf;
							for (std::uint32_t i = 0; i < se.count; ++i) { absolute.push_back(positions_[se.positions + i]); }
						} else {
							++wdf;
							absolute.push_back(t->pos());
						}
					}
					f(g.term, ord, wdf, std::span<const std::uint32_t>(absolute));
				}
			}
		}
	}

private:
	friend struct tuple_buffer_test_access;  // tests decode synthetic entries
	static constexpr std::uint32_t long_term = 255;  // the length field's escape: `off` names a side entry
	static constexpr std::size_t max_side = std::size_t(1) << 32;  // `off` indexes side entries

	// hash split in halves keeps the tuple at 20 bytes on a 4-byte alignment.
	struct tuple {
		std::uint32_t hash_hi, hash_lo, ord, off, pos_len;
		tuple() = default;  // trivial: buckets are not zeroed
		tuple(std::uint64_t h, ordinal_t o, std::uint32_t offset, std::uint32_t pl) : hash_hi(std::uint32_t(h >> 32)), hash_lo(std::uint32_t(h)), ord(o), off(offset), pos_len(pl) {}
		std::uint64_t hash() const noexcept { return std::uint64_t(hash_hi) << 32 | hash_lo; }
		std::uint32_t pos() const noexcept { return pos_len >> 8; }
		std::uint32_t len() const noexcept { return pos_len & 255; }
	};
	static_assert(sizeof(tuple) == 20);
	// A term given whole (its wdf and positions) or a token too long or too far in for a tuple.
	struct side_entry {
		const char* bytes;
		std::uint32_t size, wdf, count;
		std::size_t positions;  // the first, in positions_
	};
	struct group {
		std::string_view term;
		tuple* begin;
		tuple* end;
	};
	struct group_scratch {
		std::vector<std::uint64_t> hashes;
		std::vector<std::uint32_t> ids, tids, counts;
		std::vector<tuple> sorted;
	};
	// A batch's run: its terms' postings, sorted by term, one entry each, in one log piece.
	// An entry: [term size][term][postings][positions][cf][flags u8: 1 all wdfs 0, 2 any
	// position, 4 counts are wdfs][section byte lengths x 4], numbers LEB128, then the
	// sections: LEB128 ordinal deltas from -1, wdfs, counts (only when one differs from its
	// wdf), position deltas.
	struct run_ref {
		std::uint64_t offset;
		std::uint32_t entries;
		std::size_t first_mark, marks;  // in marks_: every 64th entry's offset in the run
	};
	static constexpr std::uint32_t mark_every = 64;
	struct entry_view {
		std::string_view term;
		std::uint32_t postings, positions;
		std::uint64_t cf;  // sum of wdfs: up to 2^32 - 1 each
		std::uint32_t flags;
		std::uint64_t lengths[4];  // the sections' bytes
		const char* data;  // the sections
		const char* end;   // the next entry
	};
	static entry_view view(const char* p) noexcept {
		entry_view v;
		const std::uint32_t size = std::uint32_t(tuple_detail::get_leb(p));
		v.term = std::string_view(p, size);
		p += size;
		v.postings = std::uint32_t(tuple_detail::get_leb(p));
		v.positions = std::uint32_t(tuple_detail::get_leb(p));
		v.cf = tuple_detail::get_leb(p);
		v.flags = std::uint8_t(*p++);
		for (auto& l : v.lengths) { l = tuple_detail::get_leb(p); }
		v.data = p;
		v.end = v.data + std::size_t(v.lengths[0]) + v.lengths[1] + v.lengths[2] + v.lengths[3];
		return v;
	}
	struct live_posting {
		ordinal_t ord;
		std::uint32_t wdf, count, first;  // first: its first position in the decoded term
		bool operator<(const live_posting& o) const noexcept { return ord < o.ord; }
	};

	// The runs' terms in byte order, each with its entries in run order (ordinal order): a
	// heap of one cursor per run, keyed by its term's first 8 bytes (big-endian), then the
	// term, then the run, so a term's entries leave it in run order.
	class merger {
	public:
		// Over the terms in [lo, hi); an empty bound is none.
		merger(const tuple_buffer& b, std::string_view lo, std::string_view hi) : hi_(hi) {
			for (std::uint32_t r = 0; r < b.runs_.size(); ++r) {
				const run_ref& run = b.runs_[r];
				const char* base = b.log_.at(run.offset);
				// The last mark before lo, then entries up to lo.
				std::size_t m = 0;
				if (!lo.empty()) {
					std::size_t first = 0, count = run.marks;
					while (count > 0) {
						const std::size_t half = count / 2;
						if (view(base + b.marks_[run.first_mark + first + half]).term < lo) {
							first += half + 1, count -= half + 1;
						} else {
							count = half;
						}
					}
					m = first ? first - 1 : 0;
				}
				cursor c{base + (run.marks ? b.marks_[run.first_mark + m] : 0), std::uint32_t(run.entries - m * mark_every), {}};
				if (!c.left) { continue; }
				c.v = view(c.at);
				while (!lo.empty() && c.v.term < lo && --c.left) {
					c.at = c.v.end;
					c.v = view(c.at);
				}
				if (c.left && (hi_.empty() || c.v.term < hi_)) {
					heap_.push_back({prefix(c.v.term), std::uint32_t(cursors_.size())});
					cursors_.push_back(c);
				}
			}
			for (std::size_t i = heap_.size() / 2; i-- > 0;) { sift(i); }
		}
		// The next term, its entries in entries(); false at the end.
		bool next(std::string_view& term) {
			if (heap_.empty() || (!hi_.empty() && cursors_[heap_.front().c].v.term >= hi_)) { return false; }
			entries_.clear();
			const slot top = heap_.front();
			term = cursors_[top.c].v.term;
			do {
				cursor& c = cursors_[heap_.front().c];
				entries_.push_back(c.v);
				if (--c.left) {
					c.at = c.v.end;
					c.v = view(c.at);
					heap_.front().prefix = prefix(c.v.term);
				} else {
					heap_.front() = heap_.back();
					heap_.pop_back();
					if (heap_.empty()) { break; }
				}
				sift(0);
			} while (heap_.front().prefix == top.prefix && cursors_[heap_.front().c].v.term == term);
			return true;
		}
		const std::vector<entry_view>& entries() const noexcept { return entries_; }

	private:
		struct cursor {
			const char* at;
			std::uint32_t left;
			entry_view v;
		};
		struct slot {
			std::uint64_t prefix;  // the cursor's term's first 8 bytes, big-endian, zero-padded
			std::uint32_t c;       // in cursors_, which are in run order
		};
		// A term's first 8 bytes as a big-endian number; an entry has at least 8 bytes from its
		// term on (the term, then 3 counts, the flags, and 4 lengths), so one load reads them.
		static std::uint64_t prefix(std::string_view term) noexcept {
			std::uint64_t w;
			std::memcpy(&w, term.data(), 8);
			if constexpr (std::endian::native == std::endian::little) {
				if (term.size() < 8) { w &= (std::uint64_t(1) << (8 * term.size())) - 1; }
				return __builtin_bswap64(w);
			} else {
				return term.size() < 8 ? w & ~(~std::uint64_t(0) >> (8 * term.size())) : w;
			}
		}
		bool before(const slot& x, const slot& y) const noexcept {
			if (x.prefix != y.prefix) { return x.prefix < y.prefix; }
			const int c = cursors_[x.c].v.term.compare(cursors_[y.c].v.term);
			return c != 0 ? c < 0 : x.c < y.c;
		}
		void sift(std::size_t i) noexcept {
			const std::size_t n = heap_.size();
			const slot s = heap_[i];
			for (;;) {
				std::size_t child = 2 * i + 1;
				if (child >= n) { break; }
				if (child + 1 < n && before(heap_[child + 1], heap_[child])) { ++child; }
				if (!before(heap_[child], s)) { break; }
				heap_[i] = heap_[child];
				i = child;
			}
			heap_[i] = s;
		}
		std::string_view hi_;
		std::vector<cursor> cursors_;
		std::vector<slot> heap_;
		std::vector<entry_view> entries_;
	};

	std::string_view bytes_of(const tuple& t) const noexcept {
		if (t.len() == long_term) { return std::string_view(side_[t.off].bytes, side_[t.off].size); }
		return std::string_view(texts_[t.ord - batch_first_] + t.off, t.len());
	}

	// A tuple into one bucket until a batch holds a bucket's share (batch_tuples / 256), then
	// into its hash's bucket of 256, which stay; buckets grow by half.
	void push(const tuple& t) {
		if (!buckets_) {
			if (flat_.size() < batch_tuples / 256) {
				push_to(flat_, t);
				++batched_;
				return;
			}
			spread();
		}
		push_to((*buckets_)[t.hash_hi >> 24], t);
		++batched_;
	}
	void push_to(std::vector<tuple>& v, const tuple& t) {
		if (v.size() == v.capacity()) {
			const std::size_t before = v.capacity();
			v.reserve(std::max<std::size_t>(16, before + before / 2));
			if (memory) { memory->grow((v.capacity() - before) * sizeof(tuple)); }
		}
		v.push_back(t);
	}
	// The 256 buckets, the one bucket's tuples moved in, in order, and its memory freed.
	void spread() {
		buckets_ = std::make_unique<std::array<std::vector<tuple>, 256>>();
		if (memory) { memory->grow(sizeof(*buckets_)); }
		for (const tuple& t : flat_) { push_to((*buckets_)[t.hash_hi >> 24], t); }
		if (memory) { memory->shrink(flat_.capacity() * sizeof(tuple)); }
		std::vector<tuple>().swap(flat_);
	}

	// Room for n bytes in the batch's text arena, and a zeroed word after them for hash's
	// loads (the next room starts on it). Chunks are kept from batch to batch.
	char* room(std::size_t n) {
		while (arena_at_ < arena_.size() && arena_fill_ + n + 8 > arena_sizes_[arena_at_]) { ++arena_at_, arena_fill_ = 0; }
		if (arena_at_ == arena_.size()) {
			const std::size_t size = std::max<std::size_t>(tuple_detail::doubled(256, arena_.size(), arena_chunk), n + 8);  // 256 B doubling
			arena_.push_back(std::unique_ptr<char[]>(new char[size]));
			arena_sizes_.push_back(size);
			arena_fill_ = 0;
			if (memory) { memory->grow(size); }
		}
		char* p = arena_[arena_at_].get() + arena_fill_;
		std::memset(p + n, 0, 8);
		arena_fill_ += n;
		return p;
	}

	void charge_columns() {
		if (!memory) { return; }
		const std::uint64_t now = (docids_.capacity() + versions_.capacity() + stored_ends_.capacity() + key_ends_.capacity()) * sizeof(std::uint64_t) +
			lengths_.capacity() * sizeof(std::uint32_t) + keys_.capacity() + texts_.capacity() * sizeof(const char*) + stored_.capacity() +
			runs_.capacity() * sizeof(run_ref) + marks_.capacity() * sizeof(std::uint32_t);
		if (now > columns_charged_) {
			memory->grow(now - columns_charged_);
			columns_charged_ = now;
		}
	}

	// drain()'s scratch, kept from batch to batch.
	void charge_scratch() {
		if (!memory) { return; }
		const std::uint64_t now = groups_.capacity() * sizeof(group) + scratch_.hashes.capacity() * 8 +
			(scratch_.ids.capacity() + scratch_.tids.capacity() + scratch_.counts.capacity() + order_.capacity()) * 4 +
			scratch_.sorted.capacity() * sizeof(tuple) + (keys_scratch_.capacity() + radix_scratch_.capacity()) * sizeof(chunk_key) +
			sort_todo_.capacity() * sizeof(key_run) + entry_at_.capacity() * sizeof(entry_ref);
		if (now > scratch_charged_) {
			memory->grow(now - scratch_charged_);
			scratch_charged_ = now;
		}
	}

	// The batch as a run: each bucket grouped, the groups sorted by term, one entry each in
	// one log piece; then the batch's tuples, text, and side entries reset, memory kept.
	void drain() {
		assert(max_run_bytes <= run_cap);
		if (batched_) {
			groups_.clear();
			encoded_.size = 0;
			entry_at_.clear();
			auto group_bucket = [&](std::vector<tuple>& v) {
				const std::size_t first = groups_.size();
				group_run(v.data(), v.size(), groups_, scratch_);
				for (std::size_t i = first; i < groups_.size(); ++i) { encode_entry(std::uint32_t(i)); }  // its tuples still in cache
			};
			if (buckets_) {
				for (auto& v : *buckets_) { group_bucket(v); }
			} else {
				group_bucket(flat_);
			}
			sort_groups(groups_);
			// The encoded entries into the run in term order, marks placed as they land.
			// Runs of at most max_run_bytes, cut between entries.
			for (std::size_t k = 0, n = order_.size(); k < n;) {
				std::size_t end = n;
				std::uint64_t bytes = encoded_.size;
				if (bytes > max_run_bytes) {
					end = k, bytes = 0;
					while (end < n && bytes + entry_at_[order_[end]].size <= max_run_bytes) { bytes += entry_at_[order_[end++]].size; }
					if (end == k) { bytes = entry_at_[order_[end++]].size; }  // past the cap: a run alone, its one mark 0
				}
				const std::size_t first_mark = marks_.size();
				const std::uint64_t at = log_.append(bytes, memory);
				char* const run = log_.at(at);
				std::size_t offset = 0;
				for (std::size_t j = k; j < end; ++j) {
					const entry_ref& e = entry_at_[order_[j]];
					if ((j - k) % mark_every == 0) { marks_.push_back(std::uint32_t(offset)); }
					std::memcpy(run + offset, encoded_.data.get() + e.offset, e.size);
					offset += e.size;
				}
				runs_.push_back({at, std::uint32_t(end - k), first_mark, marks_.size() - first_mark});
				k = end;
			}
			if (buckets_) {
				for (auto& b : *buckets_) { b.clear(); }
			}
			flat_.clear();
		}
		batched_ = 0;
		entry_term_ = 0, entry_positions_ = 0;
		side_.clear();
		positions_.clear();
		arena_at_ = 0, arena_fill_ = 0;
		texts_.clear();
		batch_first_ = ordinal_t(docids_.size());
		charge_columns();
		charge_scratch();
	}

	// Bytes that grow by half and keep their capacity, never zero-filled.
	struct growing_bytes {
		std::unique_ptr<char[]> data;
		std::size_t size = 0, capacity = 0;
		char* room(std::size_t n, flush_charge* memory) {  // n more bytes at data + size
			if (size + n > capacity) {
				const std::size_t grown = std::max(size + n, capacity + capacity / 2 + 4096);
				std::unique_ptr<char[]> more(new char[grown]);
				if (size) { std::memcpy(more.get(), data.get(), size); }
				if (memory) { memory->grow(grown - capacity); }
				data = std::move(more);
				capacity = grown;
			}
			return data.get() + size;
		}
	};
	// A group's entry in encoded_: where and how long.
	struct entry_ref {
		std::size_t offset;
		std::uint64_t size;
	};

	// Group i's postings (one batch's, ordinals increasing) as its run entry, appended to
	// encoded_.
	void encode_entry(std::uint32_t i) {
		const group& g = groups_[i];
		std::size_t tuples = 0, side_positions = 0;
		for (const tuple* p = g.begin; p != g.end; ++p) {
			++tuples;
			if (p->len() == long_term) { side_positions += side_[p->off].count; }
		}
		char* ords = sec_[0].room(5 * tuples, memory);
		char* wdfs = sec_[1].room(5 * tuples, memory);
		char* counts = sec_[2].room(5 * tuples, memory);
		char* positions = sec_[3].room(5 * (tuples + side_positions), memory);
		char* const starts[4] = {ords, wdfs, counts, positions};
		std::uint32_t postings = 0, npositions = 0, flags = 1 | 4;
		std::uint64_t cf = 0;
		bool counted = false;
		ordinal_t last = ~ordinal_t(0);
		for (const tuple* p = g.begin; p != g.end;) {
			const ordinal_t ord = p->ord;
			std::uint32_t wdf = 0, count = 0, prev = 0;
			for (; p != g.end && p->ord == ord; ++p) {
				if (p->len() == long_term) {
					const side_entry& se = side_[p->off];
					wdf += se.wdf, count += se.count;
					for (std::uint32_t k = 0; k < se.count; ++k) {
						const std::uint32_t pos = positions_[se.positions + k];
						tuple_detail::put_leb(positions, pos - prev);
						prev = pos;
					}
				} else {
					++wdf, ++count;
					tuple_detail::put_leb(positions, p->pos() - prev);
					prev = p->pos();
				}
			}
			tuple_detail::put_leb(ords, ord - last);  // from -1: the first is the ordinal + 1
			tuple_detail::put_leb(wdfs, wdf);
			tuple_detail::put_leb(counts, count);
			last = ord;
			counted = counted || count != wdf;
			++postings;
			npositions += count;
			cf += wdf;
			if (wdf) { flags &= ~1u; }
			if (count) { flags |= 2; }
			if (count != wdf) { flags &= ~4u; }
		}
		const std::uint64_t lengths[4] = {std::uint64_t(ords - starts[0]), std::uint64_t(wdfs - starts[1]), counted ? std::uint64_t(counts - starts[2]) : 0u,
			std::uint64_t(positions - starts[3])};
		char* const begin = encoded_.room(66 + g.term.size() + lengths[0] + lengths[1] + lengths[2] + lengths[3], memory);
		char* e = begin;
		tuple_detail::put_leb(e, g.term.size());
		std::memcpy(e, g.term.data(), g.term.size());
		e += g.term.size();
		for (std::uint64_t v : {std::uint64_t(postings), std::uint64_t(npositions), cf}) { tuple_detail::put_leb(e, v); }
		*e++ = char(flags);
		for (std::uint64_t l : lengths) { tuple_detail::put_leb(e, l); }
		for (int k = 0; k < 4; ++k) {
			std::memcpy(e, starts[k], lengths[k]);
			e += lengths[k];
		}
		entry_at_.push_back({encoded_.size, std::uint64_t(e - begin)});
		encoded_.size += std::size_t(e - begin);
		for (char c : g.term) { used_[std::uint8_t(c)] = true; }
	}

	// One entry's postings appended to tp, its ordinals renumbered from -1 (a run's entries
	// are their documents' whole postings for the batch).
	static void decode_entry(const entry_view& v, term_postings& tp, std::uint32_t ord) {
		const char* p = v.data;
		const char* wdfs = p + v.lengths[0];
		const char* counts = wdfs + v.lengths[1];
		const char* positions = counts + v.lengths[2];
		const char* wdfs_again = wdfs;  // the counts when the entry has none apart
		for (std::uint32_t i = 0; i < v.postings; ++i) {
			ord += std::uint32_t(tuple_detail::get_leb(p));
			tp.ords.push_back(ord);
			tp.wdfs.push_back(std::uint32_t(tuple_detail::get_leb(wdfs)));
			const std::uint32_t count = std::uint32_t(tuple_detail::get_leb(v.lengths[2] ? counts : wdfs_again));
			tp.counts.push_back(count);
			for (std::uint32_t k = 0; k < count; ++k) { tp.positions.push_back(std::uint32_t(tuple_detail::get_leb(positions))); }
		}
	}
	// A term's postings from its entries in run order, with its flags.
	static void decode(const std::vector<entry_view>& entries, term_postings& tp) {
		tp.clear();
		std::size_t postings = 0, positions = 0;
		for (const auto& v : entries) { postings += v.postings, positions += v.positions; }
		tp.ords.reserve(postings), tp.wdfs.reserve(postings), tp.counts.reserve(postings), tp.positions.reserve(positions);
		tp.all_zero_wdf = true, tp.any_positions = false, tp.counts_are_wdf = true;
		for (const auto& v : entries) {
			decode_entry(v, tp, ~std::uint32_t(0));
			tp.cf += v.cf;
			if (!(v.flags & 1)) { tp.all_zero_wdf = false; }
			if (v.flags & 2) { tp.any_positions = true; }
			if (!(v.flags & 4)) { tp.counts_are_wdf = false; }
		}
	}

	// A term's postings in the plan's numbering, from its entries in run order; false when
	// none is left.
	static bool postings_of(const std::vector<entry_view>& entries, const flush_plan& p, term_postings& tp, term_postings& old, std::vector<live_posting>& live) {
		if (p.all) {
			decode(entries, tp);
			return true;
		}
		decode(entries, old);
		relabel(old, p.renumber, p.monotone, live, tp);
		return !tp.ords.empty();
	}

	// A decoded term's postings in the flushed numbering: documents left out dropped,
	// ordinals renumbered, sorted again when the numbering reorders, flags recomputed.
	static void relabel(const term_postings& old, const std::vector<ordinal_t>& renumber, bool monotone, std::vector<live_posting>& live, term_postings& tp) {
		live.clear();
		std::uint32_t first = 0;
		for (std::size_t i = 0; i < old.ords.size(); ++i) {
			if (renumber[old.ords[i]] != no_more_docs) { live.push_back({renumber[old.ords[i]], old.wdfs[i], old.counts[i], first}); }
			first += old.counts[i];
		}
		if (!monotone) { std::sort(live.begin(), live.end()); }
		tp.clear();
		for (const auto& l : live) {
			tp.ords.push_back(l.ord);
			tp.wdfs.push_back(l.wdf);
			tp.counts.push_back(l.count);
			tp.positions.insert(tp.positions.end(), old.positions.begin() + l.first, old.positions.begin() + l.first + l.count);
			tp.cf += l.wdf;
			if (l.wdf) { tp.all_zero_wdf = false; }
			if (l.count) { tp.any_positions = true; }
			if (l.count != l.wdf) { tp.counts_are_wdf = false; }
		}
	}

	// One bucket: an id per distinct hash from a small table, a stable counting sort by id,
	// then every tuple checked against its group's first; a group holding two terms (a hash
	// collision) is split by bytes, each term's tuples keeping their order.
	void group_run(tuple* p, std::size_t n, std::vector<group>& groups, group_scratch& sc) const {
		if (!n) { return; }
		std::size_t cap = std::bit_ceil(std::max<std::size_t>(64, n / 2));
		auto& hashes = sc.hashes;
		auto& ids = sc.ids;
		hashes.assign(cap, 0);
		ids.resize(cap);
		sc.tids.resize(n);
		std::uint32_t distinct = 0;
		auto find = [&](std::uint64_t h) -> std::uint32_t {
			for (std::size_t k = (h >> 8) & (cap - 1);; k = (k + 1) & (cap - 1)) {
				if (hashes[k] == h) { return ids[k]; }
				if (hashes[k] == 0) {
					hashes[k] = h;
					ids[k] = distinct;
					return distinct++;
				}
			}
		};
		for (std::size_t i = 0; i < n; ++i) {
			std::uint64_t h = p[i].hash();
			h += h == 0;  // 0 marks an empty slot
			sc.tids[i] = find(h);
			if (2 * std::size_t(distinct) > cap) {  // keep the load at most a half
				std::vector<std::uint64_t> old_hashes(std::move(hashes));
				std::vector<std::uint32_t> old_ids(std::move(ids));
				cap *= 2;
				hashes.assign(cap, 0);
				ids.resize(cap);
				for (std::size_t k = 0; k < old_hashes.size(); ++k) {
					if (!old_hashes[k]) { continue; }
					std::size_t j = (old_hashes[k] >> 8) & (cap - 1);
					while (hashes[j]) { j = (j + 1) & (cap - 1); }
					hashes[j] = old_hashes[k];
					ids[j] = old_ids[k];
				}
			}
		}
		auto& counts = sc.counts;  // then each id's end in sorted
		counts.assign(distinct + 1, 0);
		for (std::size_t i = 0; i < n; ++i) { ++counts[sc.tids[i] + 1]; }
		for (std::uint32_t t = 0; t < distinct; ++t) { counts[t + 1] += counts[t]; }
		sc.sorted.resize(n);
		for (std::size_t i = 0; i < n; ++i) { sc.sorted[counts[sc.tids[i]]++] = p[i]; }
		std::memcpy(p, sc.sorted.data(), n * sizeof(tuple));
		// Tokens of at most 8 bytes with equal hashes and lengths are equal (hash's word
		// form is a bijection at each length); the rest compare bytes.
		const bool exact = hash_mask == ~std::uint64_t(0);
		std::size_t begin = 0;
		for (std::uint32_t t = 0; t < distinct; ++t) {
			const std::size_t end = counts[t];
			const tuple& f = p[begin];
			const std::string_view first = bytes_of(f);
			bool mixed = false;
			if (exact && f.len() <= 8) {
				for (std::size_t i = begin + 1; i < end && !mixed; ++i) { mixed = p[i].len() != f.len() || p[i].hash() != f.hash(); }
			} else {
				for (std::size_t i = begin + 1; i < end && !mixed; ++i) { mixed = bytes_of(p[i]) != first; }
			}
			if (mixed) {
				split(p + begin, p + end, groups);
			} else {
				groups.push_back({first, p + begin, p + end});
			}
			begin = end;
		}
	}

	// A hash group holding more than one term: each term's tuples, in their order.
	void split(tuple* begin, tuple* end, std::vector<group>& groups) const {
		std::stable_sort(begin, end, [&](const tuple& x, const tuple& y) { return bytes_of(x) < bytes_of(y); });
		for (tuple* g = begin; g != end;) {
			tuple* e = g + 1;
			while (e != end && bytes_of(*e) == bytes_of(*g)) { ++e; }
			groups.push_back({bytes_of(*g), g, e});
			g = e;
		}
	}

	// The groups' order by term into order_: 8-byte big-endian chunks, zero-padded, with the
	// bytes left (up to 9: more than a chunk) as a tiebreak; a run that ties on a chunk and
	// continues sorts again on the next. A batch's terms are distinct, so every run ends.
	struct chunk_key {
		std::uint64_t chunk;
		std::uint32_t left, index;
		bool operator<(const chunk_key& o) const noexcept { return chunk != o.chunk ? chunk < o.chunk : left < o.left; }
	};
	struct key_run {
		chunk_key *b, *e;
		std::size_t depth;
	};
	static chunk_key key_at(std::string_view term, std::size_t depth, std::uint32_t index) noexcept {
		std::uint64_t c = 0;
		const std::size_t n = std::min<std::size_t>(8, term.size() - depth);
		for (std::size_t i = 0; i < n; ++i) { c |= std::uint64_t(std::uint8_t(term[depth + i])) << (56 - 8 * i); }
		return {c, std::uint32_t(std::min<std::size_t>(term.size() - depth, 9)), index};
	}
	// A batch of more than a few hundred terms is sorted by LSD radix on the first chunk and
	// its length class, 8 bits a pass (a pass whose digit every key shares is skipped), and
	// the terms that tie there are sorted on past 8 bytes; a smaller one by sort_run alone.
	void sort_groups(const std::vector<group>& groups) {
		const std::size_t n = groups.size();
		keys_scratch_.resize(n);
		for (std::size_t i = 0; i < n; ++i) { keys_scratch_[i] = key_at(groups[i].term, 0, std::uint32_t(i)); }
		if (n < radix_from) {
			sort_run(groups, keys_scratch_.data(), keys_scratch_.data() + n, 0);
		} else {
			radix_sort(keys_scratch_, radix_scratch_);
			chunk_key* const e = keys_scratch_.data() + n;
			for (chunk_key* r = keys_scratch_.data(); r != e;) {
				chunk_key* t = r + 1;
				while (t != e && t->chunk == r->chunk && t->left == r->left) { ++t; }
				if (t - r > 1 && r->left == 9) {  // tied on the first chunk, and every one continues past it
					for (chunk_key* k = r; k != t; ++k) { *k = key_at(groups[k->index].term, 8, k->index); }
					sort_run(groups, r, t, 8);
				}
				r = t;
			}
		}
		order_.resize(n);
		for (std::size_t i = 0; i < n; ++i) { order_[i] = keys_scratch_[i].index; }
	}
	// By (chunk, left): left first, then the chunk's bytes from the lowest; all nine digits'
	// counts gathered in one read.
	static void radix_sort(std::vector<chunk_key>& keys, std::vector<chunk_key>& scratch) {
		const std::size_t n = keys.size();
		scratch.resize(n);
		std::array<std::array<std::uint32_t, 256>, 9> counts{};
		for (const chunk_key& k : keys) {
			++counts[0][k.left];
			for (unsigned b = 0; b < 8; ++b) { ++counts[b + 1][(k.chunk >> (8 * b)) & 255]; }
		}
		auto digit = [](const chunk_key& k, unsigned d) noexcept { return d == 0 ? unsigned(k.left) : unsigned(k.chunk >> (8 * (d - 1))) & 255; };
		chunk_key* from = keys.data();
		chunk_key* to = scratch.data();
		for (unsigned d = 0; d < 9; ++d) {
			std::array<std::uint32_t, 256>& c = counts[d];
			if (c[digit(from[0], d)] == n) { continue; }
			std::uint32_t sum = 0;
			for (std::uint32_t& x : c) { sum += std::exchange(x, sum); }
			for (std::size_t i = 0; i < n; ++i) { to[c[digit(from[i], d)]++] = from[i]; }
			std::swap(from, to);
		}
		if (from != keys.data()) { keys.swap(scratch); }
	}
	// Sorts [b, e) from depth on; each run tied on a chunk is queued to sort on the next, so a
	// long shared prefix costs heap, not stack.
	void sort_run(const std::vector<group>& groups, chunk_key* b, chunk_key* e, std::size_t depth) {
		std::vector<key_run>& todo = sort_todo_;
		todo.push_back({b, e, depth});
		while (!todo.empty()) {
			const key_run k = todo.back();
			todo.pop_back();
			std::sort(k.b, k.e);
			for (chunk_key* r = k.b; r != k.e;) {
				chunk_key* t = r + 1;
				while (t != k.e && t->chunk == r->chunk && t->left == r->left) { ++t; }
				if (t - r > 1 && r->left == 9) {  // tied on this chunk, and every one continues past it
					for (chunk_key* x = r; x != t; ++x) { *x = key_at(groups[x->index].term, k.depth + 8, x->index); }
					todo.push_back({r, t, k.depth + 8});
				}
				r = t;
			}
		}
	}

	static constexpr std::size_t arena_chunk = std::size_t(4) << 20;

	// The batch: tuples by bucket, the documents' text, and side entries.
	std::vector<tuple> flat_;  // one bucket, in arrival order, until the 256 exist
	std::unique_ptr<std::array<std::vector<tuple>, 256>> buckets_;  // by the hash's top byte, each in arrival order
	std::size_t batched_ = 0;  // tuples in the buckets
	std::uint64_t entry_term_ = 0, entry_positions_ = 0;  // the batch's longest term and given positions
	tuple_detail::chunked<side_entry, 7> side_;
	tuple_detail::chunked<std::uint32_t, 8> positions_;  // side entries' positions
	std::vector<std::unique_ptr<char[]>> arena_;         // the batch's text and terms, each padded by a word
	std::vector<std::size_t> arena_sizes_;
	std::size_t arena_at_ = 0, arena_fill_ = 0;
	std::vector<const char*> texts_;  // each batch document's text, from batch_first_
	ordinal_t batch_first_ = 0;
	group_scratch scratch_;
	std::vector<group> groups_;  // drain()'s
	std::vector<chunk_key> keys_scratch_, radix_scratch_;
	std::vector<key_run> sort_todo_;  // sort_run's pending runs
	static constexpr std::size_t radix_from = 256;  // terms a batch: fewer sort by comparisons
	std::vector<std::uint32_t> order_;
	growing_bytes sec_[4];   // an entry's sections under construction: ordinals, wdfs, counts, positions
	growing_bytes encoded_;  // the batch's entries, in the buckets' order
	std::vector<entry_ref> entry_at_;
	// The runs, one per drained batch, and the bytes their terms use.
	tuple_detail::byte_log log_;
	std::vector<run_ref> runs_;
	std::vector<std::uint32_t> marks_;
	std::array<bool, 256> used_{};
	// Per document.
	std::vector<std::uint64_t> docids_, versions_;
	std::vector<std::uint32_t> lengths_;
	std::string keys_;  // the documents' keys (F8), each ending at its key_ends_
	std::vector<std::uint64_t> key_ends_;
	std::string stored_;
	std::vector<std::uint64_t> stored_ends_;
	std::uint64_t columns_charged_ = 0, scratch_charged_ = 0;
};
}  // namespace detail

}  // namespace strata
