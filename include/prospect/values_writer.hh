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

// The values plugin's side of a segment's writing (values.hh holds the format): the builder a
// flush fills, and the merge that rebuilds the section from its inputs' columns.
#pragma once

#include "prospect/strata.hh"
#include "prospect/values.hh"
#include "strata/core/merge.hh"
#include "strata/core/writer.hh"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace prospect {

namespace values_detail {

// Streams one segment's values section: a column per call in strictly increasing slot order,
// then the directory.
class section_writer {
public:
	section_writer(section_sink& sink, flush_charge* memory, const task::cancel_token* cancel, std::uint64_t doc_count)
		: stream_(sink.begin()), sink_(sink), memory_(memory), cancel_(cancel), doc_count_(doc_count) {}

	// `ords` ascending, below the segment's documents, and `values[i]` the value of ords[i].
	void column(std::uint32_t slot, std::span<const ordinal_t> ords, std::span<const std::string_view> values) {
		if (cancel_ && cancel_->requested()) { throw task::cancelled{}; }
		if (slot == values_format::reserved_slot || (!directory_.empty() && slot <= last_slot_) || ords.empty() || ords.size() != values.size() || ords.size() > doc_count_ ||
			ords.back() >= doc_count_ || !std::is_sorted(ords.begin(), ords.end(), std::less_equal<>())) {
			throw std::invalid_argument("strata: values: a column's slot or ordinals");
		}
		std::vector<std::string_view> terms(values.begin(), values.end());
		std::sort(terms.begin(), terms.end());
		terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
		std::vector<::docset::doc_t> present(ords.begin(), ords.end());
		std::vector<std::uint64_t> starts(ords.size() + 1), flat(ords.size());
		for (std::size_t i = 0; i < ords.size(); ++i) {
			starts[i] = i;
			flat[i] = std::uint64_t(std::lower_bound(terms.begin(), terms.end(), values[i]) - terms.begin());
		}
		starts.back() = ords.size();
		const std::string dictionary = docvalues::detail::put_dictionary(terms);
		const std::string bytes = docvalues::detail::encode_present(docvalues::kind::bytes, false, doc_count_, present, starts, flat, &dictionary, {});
		if (memory_) { memory_->grow(bytes.capacity()); }
		stream_.write(bytes);
		if (memory_) { memory_->shrink(bytes.capacity()); }
		format::put<std::uint32_t>(directory_, slot);
		format::put<std::uint8_t>(directory_, values_format::kind_bytes);
		format::put<std::uint8_t>(directory_, 0);
		format::put<std::uint16_t>(directory_, 0);
		format::put<std::uint32_t>(directory_, std::uint32_t(ords.size()));
		format::put<std::uint32_t>(directory_, 0);
		format::put<std::uint64_t>(directory_, offset_);
		format::put<std::uint64_t>(directory_, bytes.size());
		offset_ += bytes.size();
		last_slot_ = slot;
		++slots_;
	}
	void finish() {
		format::put<std::uint32_t>(directory_, slots_);
		stream_.write(directory_);
		sink_.end(format::section::values);
	}

private:
	section_stream& stream_;
	section_sink& sink_;
	flush_charge* memory_;
	const task::cancel_token* cancel_;
	std::uint64_t doc_count_;
	std::string directory_;
	std::uint64_t offset_ = 0;
	std::uint32_t last_slot_ = 0, slots_ = 0;
};

}  // namespace values_detail

// A segment's values, collected per slot as documents arrive (one begin() per document, in
// ordinal order) and encoded at the flush.
class values_builder {
public:
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;

	void configure(const builder_settings& s) {
		memory = s.memory;
		cancel = s.cancel;
	}

	// A document of the segment, in ordinal order; returns its ordinal.
	ordinal_t begin() {
		if (docs_ >= std::uint64_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		return ordinal_t(docs_++);
	}
	std::uint64_t doc_count() const noexcept { return docs_; }
	// The value of `ord` (a document begun) for `slot`, once per slot and document, documents
	// of one slot in increasing order. The bytes are copied.
	void add_value(ordinal_t ord, std::uint32_t slot, std::string_view bytes) {
		if (ord >= docs_ || slot == values_format::reserved_slot || bytes.size() > values_format::max_value_bytes) { throw std::invalid_argument("strata: values: a value's document, slot, or size"); }
		auto [it, fresh] = index_.try_emplace(slot, std::uint32_t(columns_.size()));
		if (fresh) { columns_.emplace_back().slot = slot; }
		slot_data& c = columns_[it->second];
		if (!c.ords.empty() && c.ords.back() >= ord) { throw std::invalid_argument("strata: values: a slot holds one value per document, in document order"); }
		c.ords.push_back(ord);
		c.values.push_back(owned_.copy(bytes, memory));
		held_ += sizeof(ordinal_t) + sizeof(std::string_view);
		detail::charge_to(memory, held_, charged_);
	}
	void add_values(ordinal_t ord, std::span<const slot_value> values) {
		for (const auto& v : values) { add_value(ord, v.slot, v.bytes); }
	}
	// The generic writer's entry (L5): the next document with its part.
	void add(ordinal_t ord, const doc_fields&, const values_plugin::part& p) {
		if (ord != begin()) { throw std::invalid_argument("strata: values: documents out of order"); }
		add_values(ord, p.values);
	}

	void finish(section_sink& sink) {
		std::sort(columns_.begin(), columns_.end(), [](const slot_data& a, const slot_data& b) { return a.slot < b.slot; });
		values_detail::section_writer w(sink, memory, cancel, docs_);
		for (const auto& c : columns_) { w.column(c.slot, c.ords, c.values); }
		w.finish();
	}

private:
	struct slot_data {
		std::uint32_t slot = 0;
		std::vector<ordinal_t> ords;
		std::vector<std::string_view> values;
	};
	std::uint64_t docs_ = 0;
	std::unordered_map<std::uint32_t, std::uint32_t> index_;  // slot -> columns_
	std::vector<slot_data> columns_;
	detail::stable_bytes owned_{std::size_t(64) << 10};
	std::uint64_t held_ = 0, charged_ = 0;
};

// Rebuilt a slot at a time: each input's column is read in ordinal order, its live values
// keyed by their new ordinals, then sorted by them.
inline built values_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	std::vector<std::uint32_t> slots;
	for (const reader* r : in) {
		for (std::size_t i = 0; i < r->slot_count(); ++i) { slots.push_back(r->slot_at(i)); }
	}
	std::sort(slots.begin(), slots.end());
	slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
	values_detail::section_writer w(sink, m.memory, m.cancel, m.doc_count);
	std::vector<std::pair<ordinal_t, std::string_view>> found;
	std::vector<ordinal_t> ords;
	std::vector<std::string_view> values;
	std::uint64_t charged = 0;  // the gathered pairs' high-water mark
	for (const std::uint32_t slot : slots) {
		m.check();
		found.clear();
		for (std::size_t s = 0; s < in.size(); ++s) {
			const column* c = in[s]->get(slot);
			if (!c) { continue; }
			auto it = c->begin();
			for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
				if (!m.inputs[s].live(o)) { continue; }
				const ordinal_t to = m.inputs[s].new_ordinal[o];
				if (to == no_more_docs) { throw std::logic_error("strata: merge: a live document without a new ordinal"); }
				found.emplace_back(to, it.value());
			}
		}
		if (found.empty()) { continue; }  // only deleted documents held it
		if (found.capacity() * sizeof(found[0]) > charged) {
			m.charge(found.capacity() * sizeof(found[0]) - charged);
			charged = found.capacity() * sizeof(found[0]);
		}
		std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		ords.clear();
		values.clear();
		for (const auto& [o, v] : found) {
			ords.push_back(o);
			values.push_back(v);
		}
		w.column(slot, ords, values);
	}
	w.finish();
	return {format::bit::values};
}

}  // namespace prospect
