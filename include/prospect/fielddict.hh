// A segment's term dictionaries by field (fields-and-keys D3, D10): the `terms` section holds one
// termdict dictionary of its own for each field with at least own_threshold terms in the segment,
// in field-id order, then one pooled dictionary for the rest, keyed by the field id's varint and
// the term. `term_index` (after the inverted statistics) holds the directory and the indexes:
//
//   [own blocks bytes LEB][entries LEB][entry bytes LEB][own index bytes LEB]
//   [samples: one per 16 entries, 20 bytes: id u32, entry offset u32, blocks offset u64, index offset u32]
//   [entries: id delta LEB (the first of a group is its sample's id), flags u8, terms LEB, blocks LEB, index LEB]
//   [the own dictionaries' indexes, in entry order][the pool's index, to the end; empty: no pool]
//
// An entry exists for each field with a dictionary of its own; a field without one is looked up in
// the pool. A lookup binary-searches the samples and decodes at most 16 entries; dictionaries open
// on first use, so opening a segment reads the directory's header only. A field's terms are never
// mixed with another's: the varint is prefix-free and order-preserving, so the pool's keys sort by
// field, then term, and a field's terms are a contiguous range of the pool.
#pragma once

#include "prospect/format.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prospect::fielddict {

// Terms a field needs in a segment to have a dictionary of its own (section 2 of the design: the smallest
// total on every measured corpus at one and at five segments).
inline constexpr std::uint64_t own_threshold = 256;
inline constexpr std::size_t sample_every = 16, sample_bytes = 20;
// Directory entry flags: none are defined yet; a reader refuses any (norms arrive with D4).
inline constexpr std::uint8_t known_flags = 0;

// --- D10: a field id as an order-preserving, prefix-free varint --------------------------------

// One byte below 240, else a lead byte 0xF0 + n (n of 1 to 4) and the id's n big-endian bytes.
inline std::size_t put_field(std::string& out, std::uint32_t id) {
	if (id < 0xF0) {
		out.push_back(char(id));
		return 1;
	}
	const unsigned n = id < (1u << 8) ? 1 : id < (1u << 16) ? 2 : id < (1u << 24) ? 3 : 4;
	out.push_back(char(0xF0 + n));
	for (unsigned i = n; i-- > 0;) { out.push_back(char(id >> (8 * i))); }
	return 1 + n;
}

// The same into a buffer of at least 5 bytes; returns the bytes written.
inline std::size_t put_field(char* out, std::uint32_t id) noexcept {
	if (id < 0xF0) {
		out[0] = char(id);
		return 1;
	}
	const unsigned n = id < (1u << 8) ? 1 : id < (1u << 16) ? 2 : id < (1u << 24) ? 3 : 4;
	out[0] = char(0xF0 + n);
	for (unsigned i = 0; i < n; ++i) { out[1 + i] = char(id >> (8 * (n - 1 - i))); }
	return 1 + n;
}

// Reads one id at p, advancing it; false for a lead byte 0xF5 to 0xFF, a truncated form, a long
// form below 240 or with a leading zero byte (so equal ids have equal bytes).
inline bool get_field(const char*& p, const char* end, std::uint32_t& id) noexcept {
	if (p == end) { return false; }
	const std::uint8_t lead = std::uint8_t(*p);
	if (lead < 0xF0) {
		id = lead;
		++p;
		return true;
	}
	const unsigned n = lead - 0xF0;
	if (n < 1 || n > 4 || std::size_t(end - p) < 1 + n) { return false; }
	std::uint32_t v = 0;
	for (unsigned i = 0; i < n; ++i) { v = v << 8 | std::uint8_t(p[1 + i]); }
	if (std::uint8_t(p[1]) == 0 || v < 0xF0) { return false; }
	id = v;
	p += 1 + n;
	return true;
}

// A set of bytes, for dictionary alphabets.
struct byte_set {
	std::array<std::uint64_t, 4> bits{};
	void add(std::uint8_t c) noexcept { bits[c >> 6] |= std::uint64_t(1) << (c & 63); }
	void add(std::string_view s) noexcept {
		for (char c : s) { add(std::uint8_t(c)); }
	}
	void add(const byte_set& o) noexcept {
		for (unsigned i = 0; i < 4; ++i) { bits[i] |= o.bits[i]; }
	}
	bool has(std::uint8_t c) const noexcept { return bits[c >> 6] >> (c & 63) & 1; }
	std::string bytes() const {
		std::string out;
		for (unsigned c = 0; c < 256; ++c) {
			if (has(std::uint8_t(c))) { out.push_back(char(c)); }
		}
		return out;
	}
};

// What the builder must know of a field before its first term: at most how many terms it will
// get (its own dictionary or the pool: any choice is a valid segment) and the bytes they use, or
// more (a dictionary's alphabet is fixed at construction). A merge plans from its inputs, whose
// terms overlap.
struct field_plan {
	std::uint32_t field = 0;
	std::uint64_t terms = 0;
	byte_set alphabet;
};

// Accumulates the plan of a sorted stream of (field, term): fields increase, terms increase
// within a field.
class planner {
public:
	void add(std::uint32_t field, std::string_view term) {
		if (plan_.empty() || plan_.back().field != field) {
			if (!plan_.empty() && plan_.back().field > field) { throw std::invalid_argument("fielddict: fields must increase"); }
			plan_.push_back({field, 0, {}});
		}
		++plan_.back().terms;
		plan_.back().alphabet.add(term);
	}
	std::vector<field_plan> finish() { return std::move(plan_); }

private:
	std::vector<field_plan> plan_;
};

// --- the builder -----------------------------------------------------------------------------------

class builder {
public:
	struct built {
		std::string blocks;  // the `terms` section: own dictionaries' blocks, then the pool's
		std::string index;   // the directory and the indexes, to follow the inverted statistics
	};

	explicit builder(std::vector<field_plan> plan) : plan_(std::move(plan)) {
		byte_set pool;
		for (std::size_t i = 0; i < plan_.size(); ++i) {
			if (i && plan_[i].field <= plan_[i - 1].field) { throw std::invalid_argument("fielddict: plan fields must increase"); }
			if (plan_[i].terms >= own_threshold) { continue; }
			std::string id;
			put_field(id, plan_[i].field);
			pool.add(id);
			pool.add(plan_[i].alphabet);
			pooled_ = true;
		}
		if (pooled_) {
			pool_dict_.emplace(pool.bytes());
		}
	}

	// The next term, in (field, term) order. `info`'s offsets are the segment's own.
	void add(std::uint32_t field, std::string_view term, const format::term_info& info) {
		if (at_ == npos || plan_[at_].field != field) { advance(field); }
		if (!own_) {
			key_.clear();
			put_field(key_, field);
			key_ += term;
			pool_dict_->add(key_, format::encode_term_info(info, pool_run_, pool_dict_->next_is_restart()));
		} else {
			own_->add(term, format::encode_term_info(info, own_run_, own_->next_is_restart()));
		}
		++added_;
	}

	built finish() {
		close_field();
		built out;
		out.blocks = std::move(blocks_);
		const std::uint64_t own_blocks = out.blocks.size();
		std::string pool_index;
		if (pooled_) {
			termdict::tables t = pool_dict_->finish();
			out.blocks += t.blocks;
			pool_index = std::move(t.index);
		}
		std::string entries, samples;
		std::uint32_t prev = 0;
		std::uint64_t blocks_at = 0, index_at = 0;
		for (std::size_t i = 0; i < entries_.size(); ++i) {
			const entry& e = entries_[i];
			if (i % sample_every == 0) {
				if (entries.size() > 0xFFFFFFFFull || index_at > 0xFFFFFFFFull) { throw std::length_error("fielddict: directory too large"); }
				format::put<std::uint32_t>(samples, e.field);
				format::put<std::uint32_t>(samples, std::uint32_t(entries.size()));
				format::put<std::uint64_t>(samples, blocks_at);
				format::put<std::uint32_t>(samples, std::uint32_t(index_at));
			}
			format::put_leb(entries, e.field - prev);
			entries.push_back(char(0));
			format::put_leb(entries, e.terms);
			format::put_leb(entries, e.blocks);
			format::put_leb(entries, e.index);
			prev = e.field;
			blocks_at += e.blocks;
			index_at += e.index;
		}
		format::put_leb(out.index, own_blocks);
		format::put_leb(out.index, entries_.size());
		format::put_leb(out.index, entries.size());
		format::put_leb(out.index, indexes_.size());
		out.index += samples;
		out.index += entries;
		out.index += indexes_;
		out.index += pool_index;
		return out;
	}

private:
	static constexpr std::size_t npos = ~std::size_t(0);
	struct entry {
		std::uint32_t field;
		std::uint64_t terms, blocks, index;
	};

	// The fields come in plan order, a planned field possibly without terms.
	void advance(std::uint32_t field) {
		close_field();
		std::size_t next = at_ == npos ? 0 : at_ + 1;
		while (next < plan_.size() && plan_[next].field < field) { ++next; }
		if (next >= plan_.size() || plan_[next].field != field) { throw std::invalid_argument("fielddict: a field out of the plan's order"); }
		at_ = next;
		added_ = 0;
		last_blocks_ = blocks_.size();
		if (plan_[at_].terms >= own_threshold) {
			own_run_ = {};
			own_.emplace(plan_[at_].alphabet.bytes(), [this](std::string_view b) { blocks_ += b; });
		}
	}

	void close_field() {
		if (at_ == npos) { return; }
		if (added_ > plan_[at_].terms) { throw std::invalid_argument("fielddict: a field has more terms than planned"); }
		if (own_) {
			termdict::tables t = own_->finish();
			entries_.push_back({plan_[at_].field, added_, blocks_.size() - last_blocks_, t.index.size()});
			indexes_ += t.index;
			own_.reset();
		}
	}

	std::vector<field_plan> plan_;
	std::size_t at_ = npos;
	std::uint64_t added_ = 0, last_blocks_ = 0;
	bool pooled_ = false;
	std::optional<termdict::builder> pool_dict_, own_;
	format::term_run pool_run_, own_run_;
	std::string key_, blocks_, indexes_;
	std::vector<entry> entries_;
};

// --- the reader ------------------------------------------------------------------------------------

// Called with a byte range of the terms section before any of it is read; false rejects it.
using verifier = bool (*)(void* context, std::uint64_t offset, std::uint64_t length);

class reader {
public:
	// A directory entry resolved: where a field's dictionary lies.
	struct entry {
		std::uint32_t field = 0;
		std::uint64_t terms = 0, blocks = 0, blocks_size = 0, index = 0, index_size = 0;
	};

	// `terms` is the section's bytes, `index` the term_index after the statistics. Both must
	// outlive the reader. Throws corrupt_segment when the directory's header is inconsistent;
	// nothing else is read.
	reader(std::string_view terms, std::string_view index, verifier verify = nullptr, void* context = nullptr)
		: state_(std::make_unique<state>()), terms_(terms), verify_(verify), context_(context) {
		const char* p = index.data();
		const char* end = p + index.size();
		own_blocks_ = format::get_leb(&p, end);
		entry_count_ = format::get_leb(&p, end);
		entry_bytes_ = format::get_leb(&p, end);
		index_bytes_ = format::get_leb(&p, end);
		const std::uint64_t samples = (entry_count_ + sample_every - 1) / sample_every;
		if (own_blocks_ > terms.size() || entry_count_ > std::uint64_t(end - p) || samples > std::uint64_t(end - p) / sample_bytes) { corrupt("field directory"); }
		const std::uint64_t left = std::uint64_t(end - p) - samples * sample_bytes;
		if (entry_bytes_ > left || index_bytes_ > left - entry_bytes_ || (entry_count_ == 0) != (entry_bytes_ == 0)) { corrupt("field directory"); }
		samples_ = p;
		samples_count_ = samples;
		entries_ = p + samples * sample_bytes;
		indexes_ = std::string_view(entries_ + entry_bytes_, index_bytes_);
		pool_index_ = std::string_view(entries_ + entry_bytes_ + index_bytes_, std::size_t(end - (entries_ + entry_bytes_ + index_bytes_)));
		if (entry_count_ == 0 && index_bytes_ != 0) { corrupt("field directory"); }
		if (pool_index_.empty() != (terms.size() == own_blocks_)) { corrupt("field directory"); }  // pool blocks and pool index come together
	}
	reader(reader&&) = default;
	reader& operator=(reader&&) = default;

	std::uint64_t entry_count() const noexcept { return entry_count_; }
	bool has_pool() const noexcept { return !pool_index_.empty(); }

	// The directory's entry for `field`, or none: the field has no dictionary of its own.
	std::optional<entry> entry_of(std::uint32_t field) const {
		if (entry_count_ == 0) { return std::nullopt; }
		std::size_t lo = 0, hi = samples_count_;
		if (field < sample_id(0)) { return std::nullopt; }
		while (hi - lo > 1) {
			const std::size_t mid = (lo + hi) / 2;
			if (sample_id(mid) <= field) { lo = mid; } else { hi = mid; }
		}
		std::optional<entry> found;
		walk_group(lo, [&](const entry& e) {
			if (e.field == field) { found = e; }
			return e.field < field;
		});
		return found;
	}

	// The value stored for `term` of `field`; visit(value) also sees the values of the restart
	// run up to and including it, in that dictionary's order (find_run's contract).
	template <class Visit>
	std::optional<std::string_view> find_run(std::uint32_t field, std::string_view term, Visit&& visit) const {
		try {
			if (const auto e = entry_of(field)) { return own(*e).find_run(term, visit); }
			if (!has_pool()) { return std::nullopt; }
			std::string key;
			put_field(key, field);
			key += term;
			return pool().find_run(key, visit);
		} catch (const termdict::corrupt&) {
			corrupt("terms checksum");
		}
	}

	// Calls f(term, value) for each term of `field` from `from` on, in order, until f returns false.
	template <class F>
	void scan(std::uint32_t field, std::string_view from, F&& f) const {
		try {
			if (const auto e = entry_of(field)) {
				own(*e).scan(from, f);
				return;
			}
			if (!has_pool()) { return; }
			std::string key;
			put_field(key, field);
			const std::size_t lead = key.size();
			key += from;
			pool().scan(key, [&](std::string_view k, std::string_view v) {
				const char* q = k.data();
				std::uint32_t id;
				if (!get_field(q, k.data() + k.size(), id)) { corrupt("pooled field id"); }
				if (id != field) { return false; }
				return f(k.substr(lead), v);
			});
		} catch (const termdict::corrupt&) {
			corrupt("terms checksum");
		}
	}

	// Whether the segment has any term of `field`.
	bool has_field(std::uint32_t field) const {
		bool any = false;
		scan(field, {}, [&](std::string_view, std::string_view) {
			any = true;
			return false;
		});
		return any;
	}

	// A dictionary's view of the terms section: the verifier, shifted to the dictionary's blocks.
	struct dict_context {
		verifier verify;
		void* context;
		std::uint64_t base;
	};

	// Every term in (field, term) order with its decoded info, one at a time: the own dictionaries
	// and the pool interleaved by field id. Neither copied nor moved.
	class cursor {
	public:
		explicit cursor(const reader& r) : r_(&r) {
			if (r.has_pool()) {
				pool_.emplace(r.pool());
				try {
					pool_live_ = pool_->seek({});
				} catch (const termdict::corrupt&) {
					corrupt("terms checksum");
				}
				if (pool_live_) { pool_field_ = field_of(pool_->key()); }
			}
		}
		cursor(const cursor&) = delete;
		cursor& operator=(const cursor&) = delete;

		bool next() {
			try {
				return step();
			} catch (const termdict::corrupt&) {
				corrupt("terms checksum");
			}
		}
		std::uint32_t field() const noexcept { return field_; }
		std::string_view term() const noexcept { return term_; }
		const format::term_info& info() const noexcept { return info_; }
		// The entry's raw value bytes, as the builder stored them.
		std::string_view value() const noexcept { return value_; }

	private:
		bool step() {
			if (started_) {
				if (from_pool_) {
					pool_live_ = pool_->next();
					if (pool_live_) { pool_field_ = field_of(pool_->key()); }
				} else if (!own_cur_->next()) {
					own_cur_.reset();
					own_dict_.reset();
					++entry_at_;
					open_own();
				}
			} else {
				started_ = true;
				open_own();
			}
			const bool own_live = own_cur_.has_value();
			if (!own_live && !pool_live_) { return false; }
			if (own_live && pool_live_ && own_field_ == pool_field_) { corrupt("a field in both an own dictionary and the pool"); }
			from_pool_ = !own_live || (pool_live_ && pool_field_ < own_field_);
			{
				if (from_pool_) {
					const char* q = pool_->key().data();
					std::uint32_t id;
					get_field(q, pool_->key().data() + pool_->key().size(), id);
					field_ = id;
					term_ = pool_->key().substr(std::size_t(q - pool_->key().data()));
					value_ = pool_->value();
					info_ = format::decode_term_info(value_, pool_run_);
				} else {
					field_ = own_field_;
					term_ = own_cur_->key();
					value_ = own_cur_->value();
					info_ = format::decode_term_info(value_, own_run_);
				}
			}
			return true;
		}
		static std::uint32_t field_of(std::string_view key) {
			const char* q = key.data();
			std::uint32_t id = 0;
			if (!get_field(q, key.data() + key.size(), id)) { corrupt("pooled field id"); }
			return id;
		}
		void open_own() {
			own_run_ = {};
			while (entry_at_ < r_->entry_count_) {
				const auto e = r_->entry_at(entry_at_);
				own_dict_ = std::make_unique<termdict::reader>(r_->dictionary(e, &own_ctx_));
				own_cur_.emplace(*own_dict_);
				own_field_ = e.field;
				if (own_cur_->next()) { return; }
				own_cur_.reset();
				own_dict_.reset();
				++entry_at_;
			}
		}
		const reader* r_;
		std::optional<termdict::reader::cursor> pool_, own_cur_;
		std::unique_ptr<termdict::reader> own_dict_;
		dict_context own_ctx_{};
		std::uint64_t entry_at_ = 0;
		std::uint32_t own_field_ = 0, pool_field_ = 0, field_ = 0;
		bool started_ = false, from_pool_ = false, pool_live_ = false;
		format::term_run own_run_, pool_run_;
		std::string_view term_, value_;
		format::term_info info_;
	};

	// Adds this segment's fields to a merge's plan: an own dictionary's term count and bytes, and
	// the pool's per-field counts and bytes (a pass over the pool's keys).
	void add_plan(std::map<std::uint32_t, field_plan>& plan) const {
		auto at = [&](std::uint32_t field) -> field_plan& {
			auto& p = plan[field];
			p.field = field;
			return p;
		};
		for (std::uint64_t i = 0; i < entry_count_; ++i) {
			const entry e = entry_at(i);
			field_plan& p = at(e.field);
			p.terms += e.terms;
			try {
				for (char c : own(e).alphabet_bytes()) { p.alphabet.add(std::uint8_t(c)); }
			} catch (const termdict::corrupt&) {
				corrupt("term index");
			}
		}
		if (!has_pool()) { return; }
		try {
			pool().scan({}, [&](std::string_view key, std::string_view) {
				const char* q = key.data();
				std::uint32_t id;
				if (!get_field(q, key.data() + key.size(), id)) { corrupt("pooled field id"); }
				field_plan& p = at(id);
				++p.terms;
				p.alphabet.add(key.substr(std::size_t(q - key.data())));
				return true;
			});
		} catch (const termdict::corrupt&) {
			corrupt("terms checksum");
		}
	}

	// Every field id of the dictionary lies in a table of `table_size` fields and none is the key
	// field (D7: it holds no terms). A pass over the directory and the pool's keys.
	void check_fields(std::uint64_t table_size, std::optional<std::uint32_t> key_field) const {
		auto ok = [&](std::uint64_t id) { return id < table_size && id != key_field; };
		for (std::uint64_t i = 0; i < entry_count_; ++i) {
			if (!ok(entry_at(i).field)) { corrupt("field id outside the field table"); }
		}
		if (!has_pool()) { return; }
		try {
			pool().scan({}, [&](std::string_view key, std::string_view) {
				const char* q = key.data();
				std::uint32_t id;
				if (!get_field(q, key.data() + key.size(), id)) { corrupt("pooled field id"); }
				if (!ok(id)) { corrupt("field id outside the field table"); }
				return true;
			});
		} catch (const termdict::corrupt&) {
			corrupt("terms checksum");
		}
	}

	// Every directory entry, in order, checked whole: the sizes add up to the sections, ids
	// increase, flags are known, each sample matches its entry (check's job; lookups check a group);
	// then every term is read, in order, against the entries' counts.
	void check() const {
		std::uint32_t prev = 0;
		std::uint64_t blocks = 0, index = 0;
		const char* p = entries_;
		const char* end = entries_ + entry_bytes_;
		for (std::uint64_t i = 0; i < entry_count_; ++i) {
			if (i % sample_every == 0) {
				const char* s = samples_ + (i / sample_every) * sample_bytes;
				if (format::get<std::uint32_t>(s + 4) != std::uint32_t(p - entries_) || format::get<std::uint64_t>(s + 8) != blocks || format::get<std::uint32_t>(s + 16) != index) {
					corrupt("field directory sample");
				}
			}
			const std::uint64_t delta = format::get_leb(&p, end);
			if (p == end) { corrupt("field directory entry"); }
			const std::uint8_t flags = std::uint8_t(*p++);
			const std::uint64_t terms = format::get_leb(&p, end), b = format::get_leb(&p, end), x = format::get_leb(&p, end);
			if (terms == 0 || flags & ~known_flags || (i && delta == 0) || (i == 0 ? delta : std::uint64_t(prev) + delta) > 0xFFFFFFFFull) { corrupt("field directory entry"); }
			prev = std::uint32_t(i ? prev + delta : delta);
			if (i % sample_every == 0 && format::get<std::uint32_t>(samples_ + (i / sample_every) * sample_bytes) != prev) { corrupt("field directory sample"); }
			if (b > own_blocks_ - blocks || x > index_bytes_ - index) { corrupt("field directory sizes"); }
			blocks += b;
			index += x;
		}
		if (p != end || blocks != own_blocks_ || index != index_bytes_) { corrupt("field directory total"); }
		// The terms themselves: strictly ascending by (field, term), and an own dictionary holds
		// as many as its entry says.
		cursor c(*this);
		bool any = false;
		std::uint32_t field = 0;
		std::uint64_t run = 0;
		std::string last;
		auto close = [&] {
			if (const auto e = entry_of(field); any && e && e->terms != run) { corrupt("field term count"); }
		};
		while (c.next()) {
			if (any && (c.field() < field || (c.field() == field && c.term() <= last))) { corrupt("terms out of order"); }
			if (any && c.field() != field) {
				close();
				run = 0;
			}
			field = c.field();
			last.assign(c.term());
			any = true;
			++run;
		}
		close();
	}

private:
	friend class cursor;
	static bool verify_in(void* c, std::uint64_t offset, std::uint64_t length) {
		const auto* d = static_cast<const dict_context*>(c);
		return d->verify(d->context, d->base + offset, length);
	}
	struct opened {
		dict_context context;
		std::unique_ptr<termdict::reader> dict;
	};
	struct state {
		std::mutex mutex;
		std::map<std::uint64_t, opened> open;  // by blocks offset; the pool at own_blocks_
	};

	std::uint32_t sample_id(std::size_t s) const noexcept { return format::get<std::uint32_t>(samples_ + s * sample_bytes); }

	// Decodes the group starting at sample s, calling f(entry) until it returns false.
	template <class F>
	void walk_group(std::size_t s, F&& f) const {
		const char* smp = samples_ + s * sample_bytes;
		const std::uint64_t first = s * sample_every;
		const std::uint64_t n = std::min<std::uint64_t>(sample_every, entry_count_ - first);
		const std::uint32_t sample_field = format::get<std::uint32_t>(smp);
		const std::uint32_t entry_off = format::get<std::uint32_t>(smp + 4);
		if (entry_off > entry_bytes_) { corrupt("field directory sample"); }
		const char* p = entries_ + entry_off;
		const char* end = entries_ + entry_bytes_;
		std::uint64_t blocks = format::get<std::uint64_t>(smp + 8), index = format::get<std::uint32_t>(smp + 16);
		std::uint32_t id = sample_field;
		for (std::uint64_t i = 0; i < n; ++i) {
			const std::uint64_t delta = format::get_leb(&p, end);
			if (p == end) { corrupt("field directory entry"); }
			const std::uint8_t flags = std::uint8_t(*p++);
			const std::uint64_t terms = format::get_leb(&p, end), b = format::get_leb(&p, end), x = format::get_leb(&p, end);
			if (i) {
				if (delta == 0 || delta > 0xFFFFFFFFull - id) { corrupt("field directory entry"); }
				id += std::uint32_t(delta);
			}
			if (flags & ~known_flags) { corrupt("field directory flags"); }
			if (blocks > own_blocks_ || b > own_blocks_ - blocks || index > index_bytes_ || x > index_bytes_ - index) { corrupt("field directory sizes"); }
			if (!f(entry{id, terms, blocks, b, index, x})) { return; }
			blocks += b;
			index += x;
		}
	}
	entry entry_at(std::uint64_t i) const {
		entry out;
		walk_group(i / sample_every, [&, k = i % sample_every](const entry& e) mutable {
			out = e;
			return k-- != 0;
		});
		return out;
	}

	// An own dictionary read through the same checksum verifier as open_at's; `ctx` outlives it.
	termdict::reader dictionary(const entry& e, dict_context* ctx) const {
		const std::string_view blocks = terms_.substr(e.blocks, e.blocks_size), index = indexes_.substr(e.index, e.index_size);
		try {
			if (!verify_) { return termdict::reader(blocks, index); }
			*ctx = dict_context{verify_, context_, e.blocks};
			return termdict::reader(blocks, index, &verify_in, ctx);
		} catch (const termdict::corrupt&) {
			corrupt("term index");
		} catch (const std::invalid_argument&) {
			corrupt("term index");
		}
	}
	const termdict::reader& own(const entry& e) const { return open_at(e.blocks, e.blocks_size, indexes_.substr(e.index, e.index_size)); }
	const termdict::reader& pool() const { return open_at(own_blocks_, terms_.size() - own_blocks_, pool_index_); }

	const termdict::reader& open_at(std::uint64_t base, std::uint64_t size, std::string_view index) const {
		std::lock_guard lock(state_->mutex);
		auto it = state_->open.find(base);
		if (it == state_->open.end()) {
			it = state_->open.emplace(base, opened{{verify_, context_, base}, nullptr}).first;  // map nodes never move
			try {
				const std::string_view blocks = terms_.substr(base, size);
				it->second.dict = verify_ ? std::make_unique<termdict::reader>(blocks, index, &verify_in, &it->second.context)
										  : std::make_unique<termdict::reader>(blocks, index);
			} catch (const termdict::corrupt&) {
				state_->open.erase(it);
				corrupt("term index");
			} catch (const std::invalid_argument&) {
				state_->open.erase(it);
				corrupt("term index");
			}
		}
		return *it->second.dict;
	}

	std::unique_ptr<state> state_;
	std::string_view terms_;
	verifier verify_ = nullptr;
	void* context_ = nullptr;
	std::uint64_t own_blocks_ = 0, entry_count_ = 0, entry_bytes_ = 0, index_bytes_ = 0, samples_count_ = 0;
	const char* samples_ = nullptr;
	const char* entries_ = nullptr;
	std::string_view indexes_, pool_index_;
};

}  // namespace prospect::fielddict
