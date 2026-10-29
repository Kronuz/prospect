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

// The values plugin (xapiand-on-strata P1; docs/mechanisms/values.md): opaque byte values keyed
// by a 32-bit slot, one single-valued `bytes` column per slot and segment (docvalues.hh), a slot
// directory in the section's tail. Values are never interpreted here. This header holds the
// document part, the reader, the check, and the digest records; values_writer.hh the builder
// and the merge.
//
// Section, in file order: the columns back to back in slot order, then the directory, then
// the slot count:
//   column: docvalues.hh's bytes column, single-valued
//   directory entry (32 bytes): u32 slot, u8 kind (3, bytes), u8 flags (0), u16 0,
//     u32 documents with a value (deleted ones included), u32 0, u64 offset, u64 size
//   u32 slot count
#pragma once

#include "prospect/docvalues.hh"
#include "prospect/strata.hh"
#include "strata/core/reader.hh"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prospect {

class values_builder;

// One document's value for a slot.
struct slot_value {
	std::uint32_t slot = 0;
	std::string bytes;
};

namespace values_format {
// Slot 0xFFFFFFFF names the key in index_sort (format::key_slot), so no value uses it.
inline constexpr std::uint32_t reserved_slot = format::key_slot;
// A value's bytes, and a document's values together: a stored record's limit, which the flush's
// working memory per document is sized for.
inline constexpr std::uint64_t max_value_bytes = format::max_stored_record;
inline constexpr std::size_t entry_bytes = 32;
inline constexpr std::uint8_t kind_bytes = std::uint8_t(docvalues::kind::bytes);
}  // namespace values_format

namespace values_detail {
// Runs an initializer at most once to success, from any thread; one that throws leaves it to
// the next caller. (Not std::call_once: TSan's pthread_once wraps the callable without
// unwinding, and a throw there leaves every later caller waiting.)
class lazy {
public:
	template <class F>
	void run(F&& f) {
		if (done_.load(std::memory_order_acquire)) { return; }
		std::lock_guard lock(m_);
		if (done_.load(std::memory_order_relaxed)) { return; }
		f();
		done_.store(true, std::memory_order_release);
	}

private:
	std::mutex m_;
	std::atomic<bool> done_{false};
};
}  // namespace values_detail

struct values_plugin {
	static constexpr std::string_view id = "values";
	static constexpr format::owner owner = format::owner::values;
	static constexpr format::structure_bits structure = format::bit::values;
	static constexpr format::structure_bits structures = format::bit::values;
	using builder = values_builder;

	// What a document carries for it (L10): its values by strictly increasing slot, each slot
	// once. An empty value is a present value.
	struct part {
		std::vector<slot_value> values;
		// Sets the slot's value, replacing an earlier one (Xapian's add_value), keeping slot order.
		void set(std::uint32_t slot, std::string bytes) {
			auto it = std::lower_bound(values.begin(), values.end(), slot, [](const slot_value& v, std::uint32_t s) { return v.slot < s; });
			if (it != values.end() && it->slot == slot) {
				it->bytes = std::move(bytes);
			} else {
				values.insert(it, slot_value{slot, std::move(bytes)});
			}
		}
	};
	// Stage 1: slots strictly increase and avoid the reserved one; each value and all of a
	// document's together are within the limit.
	static bool validate(const validate_context&, const part& p) {
		std::uint64_t total = 0;
		for (std::size_t i = 0; i < p.values.size(); ++i) {
			const slot_value& v = p.values[i];
			if (v.slot == values_format::reserved_slot || (i && v.slot <= p.values[i - 1].slot)) { return false; }
			total += v.bytes.size();
			if (v.bytes.size() > values_format::max_value_bytes || total > values_format::max_value_bytes) { return false; }
		}
		return true;
	}
	// The writer's check: a segment without `values` holds none (F4).
	static void refuse(const part& p, bool configured) {
		if (!p.values.empty() && !configured) { throw std::invalid_argument("strata: values in a segment without values"); }
	}
	static std::uint64_t bytes(const part& p) {
		std::uint64_t n = p.values.size() * sizeof(slot_value);
		for (const auto& v : p.values) { n += v.bytes.size(); }
		return n;
	}
	static std::uint64_t disk_estimate(const part& p) {
		std::uint64_t n = 0;
		for (const auto& v : p.values) { n += v.bytes.size() + 4; }
		return n;
	}

	// One slot's column in one segment: the value of each ordinal that has one, and the
	// ordinals that do, ascending.
	class column {
	public:
		explicit column(docvalues::column_view view) : view_(std::move(view)) {}
		// Ordinals with a value, deleted documents included.
		std::uint64_t documents() const noexcept { return view_.stats().docs; }
		// Distinct values, and the smallest and largest (bounds when documents were deleted).
		std::uint64_t distinct() const noexcept { return view_.terms(); }
		std::string_view first() const { return view_.term(0); }
		std::string_view last() const { return view_.term(view_.terms() - 1); }
		bool has(ordinal_t o) const noexcept { return view_.has(o); }
		std::optional<std::string_view> value(ordinal_t o) const {
			const auto i = view_.value(o);
			if (!i) { return std::nullopt; }
			return view_.term(*i);
		}
		std::uint64_t ordinals() const noexcept { return view_.doc_count(); }
		docvalues::kind type() const noexcept { return view_.type(); }
		bool multi_valued() const noexcept { return view_.multi_valued(); }

		// Documents with a value in ordinal order. Before next() or advance() it is at no
		// document; after the last it is at no_more_docs.
		class cursor {
		public:
			explicit cursor(const column& c) : col_(&c) {}
			ordinal_t doc() const noexcept { return ord_; }
			// The value at the current document.
			std::string_view value() const { return col_->view_.term(col_->view_.value_at(index_)); }
			ordinal_t next() { return advance(ord_ == before ? 0 : ord_ + 1); }
			// The first document at or after target (the current one if it is), or no_more_docs.
			ordinal_t advance(ordinal_t target) {
				if (ord_ == no_more_docs) { return ord_; }
				if (ord_ != before && target <= ord_) { return ord_; }
				const docvalues::column_view& v = col_->view_;
				std::size_t g = std::max(group_, v.find_group(target));
				while (g < v.group_count()) {
					const auto e = v.group(g);
					ordinal_t o = std::max(target, e.first);
					while (o <= e.last && !v.has(o)) { ++o; }
					if (o <= e.last) {
						group_ = g;
						ord_ = o;
						index_ = v.doc_index(o);
						return ord_;
					}
					++g;
				}
				ord_ = no_more_docs;
				return ord_;
			}

		private:
			static constexpr ordinal_t before = no_more_docs - 1;
			const column* col_;
			ordinal_t ord_ = before;
			std::size_t group_ = 0;
			std::uint64_t index_ = 0;
		};
		cursor begin() const { return cursor(*this); }

	private:
		docvalues::column_view view_;
	};

	class reader {
	public:
		// Opening reads nothing of the section: the directory is read, checked, and
		// verified on first use, from any thread (an open touches only the pinned sections).
		explicit reader(const open_context& c) : section_(c.section(format::section::values)), doc_count_(c.doc_count), present_(c.present), state_(std::make_unique<state>()) {}

		// Slots with a value in this segment, ascending.
		std::size_t slot_count() const { return entries().size(); }
		std::uint32_t slot_at(std::size_t i) const { return entries().at(i).slot; }
		// Documents with a value in slot_at(i), from the directory: no column is opened.
		std::uint64_t documents_at(std::size_t i) const { return entries().at(i).documents; }
		bool has_slot(std::uint32_t slot) const { return find(slot) != entries().size(); }

		// The slot's column, opened and checked on first use; null when no document here has
		// a value for it. Safe from several threads.
		const column* get(std::uint32_t slot) const {
			const std::size_t i = find(slot);
			return i == entries().size() ? nullptr : &open_column(i);
		}
		const column& at(std::size_t i) const {
			if (i >= entries().size()) { throw std::out_of_range("strata: values: slot index"); }
			return open_column(i);
		}
		// The document's value for the slot, or none.
		std::optional<std::string_view> value(ordinal_t ord, std::uint32_t slot) const {
			const column* c = get(slot);
			return c ? c->value(ord) : std::nullopt;
		}
		// Calls f(slot, bytes) for each of the document's values, by slot (opens every column).
		template <class F>
		void each_value(ordinal_t ord, F&& f) const {
			const auto& es = entries();
			for (std::size_t i = 0; i < es.size(); ++i) {
				if (const auto v = open_column(i).value(ord)) { f(es[i].slot, *v); }
			}
		}

		void verify_all() const {
			if (!section_.bytes.empty() && !section_.verify(0, section_.bytes.size())) { corrupt("section checksum"); }
		}

	private:
		struct entry {
			std::uint32_t slot;
			std::uint32_t documents;
			std::uint64_t offset, size;
		};
		struct cell {
			values_detail::lazy once;
			std::optional<column> col;
		};
		struct state {
			values_detail::lazy once;
			std::vector<entry> entries;
			std::unique_ptr<cell[]> cells;
		};

		const std::vector<entry>& entries() const {
			state_->once.run([&] { read_directory(*state_); });
			return state_->entries;
		}
		void read_directory(state& st) const {
			if (!present_) { return; }
			const std::string_view s = section_.bytes;
			if (s.size() < 4) { corrupt("values section"); }
			const std::uint32_t n = format::get<std::uint32_t>(s.data() + s.size() - 4);
			if (n > (s.size() - 4) / values_format::entry_bytes) { corrupt("values directory"); }
			const std::uint64_t directory = s.size() - 4 - std::uint64_t(n) * values_format::entry_bytes;
			if (!section_.verify(directory, s.size() - directory)) { corrupt("values checksum"); }
			std::vector<entry> es;
			es.reserve(n);
			std::uint64_t next = 0;
			for (std::uint32_t i = 0; i < n; ++i) {
				const char* p = s.data() + directory + std::uint64_t(i) * values_format::entry_bytes;
				entry e{format::get<std::uint32_t>(p), format::get<std::uint32_t>(p + 8), format::get<std::uint64_t>(p + 16), format::get<std::uint64_t>(p + 24)};
				if (std::uint8_t(p[4]) != values_format::kind_bytes || p[5] != 0 || format::get<std::uint16_t>(p + 6) != 0 || format::get<std::uint32_t>(p + 12) != 0) {
					corrupt("values directory entry");
				}
				if (e.slot == values_format::reserved_slot || (i && e.slot <= es.back().slot)) { corrupt("values slot order"); }
				if (e.documents == 0 || e.documents > doc_count_) { corrupt("values documents"); }
				if (e.offset != next || e.size == 0 || e.size > directory - next) { corrupt("values column bounds"); }
				next += e.size;
				es.push_back(e);
			}
			if (next != directory) { corrupt("values columns"); }
			if (n) { st.cells = std::make_unique<cell[]>(n); }
			st.entries = std::move(es);
		}
		std::size_t find(std::uint32_t slot) const {
			const auto& es = entries();
			const auto it = std::lower_bound(es.begin(), es.end(), slot, [](const entry& e, std::uint32_t s) { return e.slot < s; });
			return it != es.end() && it->slot == slot ? std::size_t(it - es.begin()) : es.size();
		}
		const column& open_column(std::size_t i) const {
			cell& c = state_->cells[i];
			c.once.run([&] {
				const entry& e = state_->entries[i];
				if (!section_.verify(e.offset, e.size)) { corrupt("values checksum"); }
				column col(docvalues::column_view::open(section_.bytes.data() + e.offset, std::size_t(e.size)));
				if (col.type() != docvalues::kind::bytes || col.multi_valued() || col.ordinals() != doc_count_ || col.documents() != e.documents) { corrupt("values column"); }
				c.col.emplace(std::move(col));
			});
			return *c.col;
		}

		section_handle section_;
		std::uint64_t doc_count_;
		bool present_;
		std::unique_ptr<state> state_;  // heap-held: the reader moves with its segment
	};

	// L6: rebuilds the plugin's section for a merge's output (values_writer.hh).
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink);

	// What a check counts (check.hh): every value is read.
	struct report {
		std::uint64_t value_slots = 0, values = 0, value_bytes = 0;
	};
	static void check(const reader& r, const core_segment& core, report& rep) {
		for (std::size_t i = 0; i < r.slot_count(); ++i) {
			const column& c = r.at(i);
			std::uint64_t n = 0;
			auto it = c.begin();
			for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
				if (o >= core.doc_count()) { corrupt("values ordinal"); }
				rep.value_bytes += it.value().size();
				++n;
			}
			if (n != r.documents_at(i)) { corrupt("values documents"); }
			rep.values += n;
			++rep.value_slots;
		}
	}
	// Its digest records (L8): one per value of a live document, none for a document without.
	template <class Live, class Emit>
	static void records(const reader& r, const core_segment& core, Live&& live, Emit&& emit) {
		for (std::size_t i = 0; i < r.slot_count(); ++i) {
			auto it = r.at(i).begin();
			for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
				if (live(o)) { record(core.docid(o), r.slot_at(i), it.value(), emit); }
			}
		}
	}
	template <class Emit>
	static void records(std::uint64_t docid, const part& p, Emit&& emit) {
		for (const auto& v : p.values) { record(docid, v.slot, v.bytes, emit); }
	}
	template <class Emit>
	static void record(std::uint64_t docid, std::uint32_t slot, std::string_view bytes, Emit& emit) {
		digest_record rec('V');
		rec.u64(docid);
		rec.u32(slot);
		rec.bytes(bytes);
		emit(rec.data());
	}
};

}  // namespace prospect
