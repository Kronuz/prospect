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

// Opening a segment: the layout of plugins, the core's open-time verification (footer, table of
// contents, header, pinned sections, docids), the `keys` and `stored` plugins, and the document
// as the composition of the plugins' parts. Every offset read from the file is bounds-checked;
// a violation throws corrupt_segment, which fences the segment.

#pragma once

#include "compressor_zstd.h"
#include "fusefilter.hh"
#include "hashes.hh"
#include <crc32c/crc32c.hh>
#include "residency.hh"
#include "strata/core/docids.hh"
#include "strata/core/format.hh"
#include "strata/core/keys.hh"
#include "strata/core/section_io.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace strata {

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

template <class... Entries>
struct layout {};
template <class... Ts>
struct type_list {};

// Calls f(std::type_identity<E>{}) for each entry E of a layout, in order.
namespace detail {
template <class... Entries, class F>
constexpr void for_each_entry_of(layout<Entries...>, F&& f) {
	(f(std::type_identity<Entries>{}), ...);
}
}  // namespace detail
template <class Layout, class F>
constexpr void for_each_entry(F&& f) {
	detail::for_each_entry_of(Layout{}, std::forward<F>(f));
}

// A segment's live documents: a bit per ordinal set when deleted (null: none deleted).
struct live_bits {
	const std::uint64_t* deleted = nullptr;
	bool live(ordinal_t o) const noexcept { return !deleted || !((deleted[o >> 6] >> (o & 63)) & 1); }
};
class keys_builder;
struct keys_plugin;
struct merge_context;
class stored_builder;
struct stored_plugin;

template <class Layout>
class basic_segment;


// The core of an open segment (L7): the prefix first (F2), then the footer, table of
// contents, and header; the structure bits against those this binary reads (F4); every
// pinned section's checksum; stats' document count; docids; and the one verified-region
// state that every regions-checked section shares.
class core_segment {
public:
	core_segment(std::string_view file, format::structure_bits readable) : file_(file) {
		version_ = format::read_version(file);
		footer_ = format::decode_footer(file);
		format::classify_structures(footer_.structures, readable);  // before the table (F13-R35)
		sections_ = format::decode_toc(file, footer_);
		if (format::get<std::uint32_t>(file.data() + 16) != 0) { corrupt("header flags"); }
		format::check_presence(sections_, footer_.structures);
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
				if (::kronuz::crc32c::crc32c(file.data() + e.offset, e.length) != e.crc) { corrupt("section checksum"); }
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
	static constexpr format::structure_bits reads = 0;

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
	format::structure_bits structures() const noexcept { return footer_.structures; }
	std::string_view stats() const noexcept { return stats_; }
	const std::vector<format::section_entry>& sections() const noexcept { return sections_; }
	std::uint64_t min_docid() const noexcept { return footer_.min_docid; }
	std::uint64_t max_docid() const noexcept { return footer_.max_docid; }
	std::size_t verified_regions() const noexcept { return regions_->regions.verified(); }

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


// The document's key in its own dictionary (F8), and versions (F9).
struct keys_plugin {
	static constexpr std::string_view id = "keys";

	// A document's key is the core's field (L10), held exactly as given, a view into the
	// part. A keyless index (no key prefix) refuses a document that carries one (F13-R39);
	// with a prefix, a key that lacks it or passes the terms' limit is invalid (F13-R25
	// item 2, CR12).
	static std::optional<std::string_view> key_of(const validate_context& vc, const core_part& d, bool& valid) {
		valid = true;
		if (d.key.empty()) { return std::nullopt; }
		if (vc.key_prefix.empty() || d.key.size() > format::max_term_bytes) {
			valid = false;
			return std::nullopt;
		}
		if (!d.key.starts_with(vc.key_prefix)) { valid = false; }
		return d.key;
	}
	static constexpr format::owner owner = format::owner::keys;
	static constexpr format::structure_bits structure = format::bit::keys;
	static constexpr format::structure_bits structures = format::bit::keys;

	// Its builder (core/writer.hh).
	using builder = keys_builder;
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
		std::optional<ordinal_t> find(std::string_view key, live_bits live) const { return find(key, live, xxh64::hash(key)); }
		// The same with the key's hash, so a lookup across segments hashes once (CR7).
		std::optional<ordinal_t> find(std::string_view key, live_bits live, std::uint64_t hash) const {
			const auto ord = keys_.find(key, hash);
			if (ord && !live.live(*ord)) { return std::nullopt; }
			return ord;
		}
		// Calls f(key, ord) for every key >= from, in order, deleted copies included, until f
		// returns false (F8; PLAN.md:47-48).
		template <class F>
		void scan(std::string_view from, F&& f) const {
			keys_.scan(from, std::forward<F>(f));
		}
		// The keys' count and range, and every key in order with its ordinal, checked as walked.
		std::uint64_t key_count() const noexcept { return keys_.size(); }
		std::string_view min_key() const noexcept { return keys_.min(); }
		std::string_view max_key() const noexcept { return keys_.max(); }
		keys::reader::cursor cursor() const { return keys::reader::cursor(keys_); }
		// Calls f(key, ord) for every key in order through the cursor's checks (order, count),
		// refusing an ordinal named twice: the walk of check(), merges, and the digest (MR2, CR3).
		template <class F>
		void each_key(F&& f) const {
			std::vector<bool> named(keys_.size() ? doc_count_ : 0);
			for (auto c = cursor(); c.next();) {
				if (named[c.ordinal()]) { corrupt("an ordinal named by two keys"); }
				named[c.ordinal()] = true;
				f(c.key(), c.ordinal());
			}
		}

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

	// What a check counts (check.hh).
	struct report {
		std::uint64_t keys = 0;
	};
	// Every key in order, its ordinal in range and its count the index's (keys.hh); each
	// ordinal named once, the range the first and last key, and every key passing the
	// range and filter that lookups consult (F13-R37); a nonzero version belongs to a key
	// (F9, F13-R9).
	static void check(const reader& keys, const core_segment& core, report& r, std::string_view) {
		for (ordinal_t o = 0; o < core.doc_count(); ++o) { (void)keys.version(o); }
		std::vector<bool> keyed(core.doc_count());
		std::string first, last;
		std::uint64_t walked = 0;  // this segment's keys
		keys.each_key([&](std::string_view key, ordinal_t o) {
			keyed[o] = true;
			++r.keys;
			if (!walked++) { first = key; }
			last = key;
			if (!keys.may_hold(key)) { corrupt("a key its range or filter misses"); }
		});
		if (keys.key_count() && (keys.min_key() != first || keys.max_key() != last)) { corrupt("key range"); }
		for (ordinal_t o = 0; o < core.doc_count(); ++o) {
			if (!keyed[o] && keys.version(o) != 0) { corrupt("a version without a key"); }
		}
	}
};

// Stored records in zstd frames.
struct stored_plugin {
	static constexpr std::string_view id = "stored";

	struct part {
		std::string stored;
	};
	static bool validate(const validate_context&, const part& p) { return p.stored.size() <= format::max_stored_record; }
	// The writer's checks before it takes a record: within the limit, and a segment without
	// `stored` holds none (F4).
	static void refuse(std::string_view record, bool configured) {
		if (record.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		if (!record.empty() && !configured) { throw std::invalid_argument("strata: a stored record in a segment without stored"); }
	}
	static void refuse(const part& p, bool configured) { refuse(p.stored, configured); }
	static std::uint64_t bytes(const part& p) { return p.stored.size(); }
	static std::uint64_t disk_estimate(const part& p) { return p.stored.size() + p.stored.size() / 16; }
	static constexpr format::owner owner = format::owner::stored;
	static constexpr format::structure_bits structure = format::bit::stored;
	static constexpr format::structure_bits structures = format::bit::stored;
	using builder = stored_builder;  // core/writer.hh

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
	// What a check counts (check.hh): every document's record is read.
	struct report {
		std::uint64_t stored_bytes = 0;
	};
	static void check(const reader& stored, const core_segment& core, report& r, std::string_view) {
		for (ordinal_t o = 0; o < core.doc_count(); ++o) { r.stored_bytes += stored.stored(o).size(); }
	}
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
	// Buffer memory beyond the document object, and the parts' disk estimate; the core's key
	// counts as a term does (its bytes, and an ordinal and version on disk).
	std::uint64_t part_bytes() const { return key.size() + (part_bytes_of<Entries>() + ... + 0); }
	std::uint64_t disk_estimate() const { return (key.empty() ? 0 : key.size() + 12) + (disk_estimate_of<Entries>() + ... + 0); }

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


namespace detail {
template <class E>
struct reader_of {
	using type = typename E::reader;
};
// The structure bits a layout entry reads, and its own bit.
template <class E>
constexpr format::structure_bits structures_of() {
	return E::structures;
}
template <class E>
constexpr format::structure_bits structure_of() {
	return E::structure;
}
// No two plugins of a layout share an owner, so none claims another's kinds (F3), and no
// two read one bit (F4).
template <class... Entries>
consteval bool owners_distinct() {
	format::structure_bits bits = core_segment::reads;
	bool ok = true;
	(..., [&] {
		ok = ok && !(bits & structures_of<Entries>());
		bits |= structures_of<Entries>();
	}());
	std::array<int, 8> owners{};
	(..., [&] {
		ok = ok && owners[std::size_t(Entries::owner)]++ == 0;
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
	static_assert((section_plugin<Entries> && ...), "every layout entry is a section plugin");
	static_assert(detail::owners_distinct<Entries...>(), "two plugins share an owner or a structure bit");

public:
	// The bits this layout reads (F4; layers R16): the core's and each entry's.
	static constexpr format::structure_bits readable = (core_segment::reads | ... | detail::structures_of<Entries>());

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
	// The plugins' length statistics (L9, the manifest's): the segment's total and a document's;
	// 0 for a layout whose plugins keep none.
	std::uint64_t total_length() const {
		std::uint64_t n = 0;
		(..., [&] {
			if constexpr (requires { Entries::total_length(get<Entries>()); }) { n += Entries::total_length(get<Entries>()); }
		}());
		return n;
	}
	std::uint64_t length(ordinal_t o) const {
		std::uint64_t n = 0;
		(..., [&] {
			if constexpr (requires { Entries::length(get<Entries>(), o); }) { n += Entries::length(get<Entries>(), o); }
		}());
		return n;
	}

private:
	using readers = std::tuple<typename detail::reader_of<Entries>::type...>;
	static_assert(sizeof...(Entries) <= 32, "a layout's presence mask holds 32 entries");
	// Bit i: entry i's own structure bit is set in the footer.
	static std::uint32_t presence(const core_segment& core) noexcept {
		std::uint32_t out = 0, i = 0;
		(..., (out |= std::uint32_t((core.structures() & detail::structure_of<Entries>()) != 0) << i++));
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
		get<E>().verify_all();
	}

	core_segment core_;
	std::uint32_t present_;  // bit i: layout entry i is present; set before the readers open (F4)
	readers readers_;
};


// Readers hold views of the file and pointers into heap-held state, so a segment moves
// without leaving one dangling (layers.md L3).
static_assert(std::is_nothrow_move_constructible_v<basic_segment<layout<keys_plugin, stored_plugin>>>);

}  // namespace strata
