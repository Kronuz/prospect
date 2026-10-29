// The search layout's inverter for the core's index: the tuple buffer (tuple_buffer.hh) behind the
// interface strata/core/index.hh asks of a layout's `inverter` (layers.md L10): documents
// inverted as they are accepted at one flush thread, flushed as the segment a segment_writer
// would write from the same documents, and digested as their records.
#pragma once

#include "prospect/reader.hh"
#include "prospect/tuple_buffer.hh"
#include "prospect/values.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace prospect {

class inverting_buffer : public detail::tuple_buffer {
public:
	// A document that passed stage 1, each field's terms and tokens or split text inverted under
	// the id `res` gives its reference; returns its ordinal. `key` is borrowed from the
	// transaction until the next flush.
	ordinal_t add(std::uint64_t docid, std::uint64_t version, std::string_view key, const document& d, const field_resolution& res) {
		terms_in_.clear();
		texts_in_.clear();
		std::uint64_t given = 0;  // the host's lengths of the fields without a split text
		for (const auto& f : d.fields) {
			const auto id = res.resolve(f.field);
			if (!id) { throw std::logic_error("strata: a field reference that stage 1 accepted does not resolve"); }
			for (const auto& t : f.terms) { terms_in_.push_back({t.term, t.wdf, t.positions, *id}); }
			if (!f.text.empty() || !f.tokens.empty()) { texts_in_.push_back({*id, f.text, f.tokens, f.split_text}); }
			if (!f.split_text) { given += f.length; }
		}
		const ordinal_t ord = tuple_buffer::add(docid, terms_in_, texts_in_, given, d.length, d.stored, version, key);
		if (!d.values.empty()) { keep_values(ord, d.values); }
		return ord;
	}

	// Calls f(slot, bytes) for each value the document at `ord` was given, by slot.
	template <class F>
	void each_value(ordinal_t ord, F&& f) const {
		const auto it = std::lower_bound(value_docs_.begin(), value_docs_.end(), ord);
		if (it == value_docs_.end() || *it != ord) { return; }
		const std::size_t d = std::size_t(it - value_docs_.begin());
		for (std::size_t e = d ? value_doc_ends_[d - 1] : 0; e < value_doc_ends_[d]; ++e) {
			const std::uint64_t begin = e ? value_ends_[e - 1].end : 0;
			f(value_ends_[e].slot, std::string_view(value_bytes_).substr(begin, value_ends_[e].end - begin));
		}
	}
	bool has_values() const noexcept { return !value_docs_.empty(); }

	// Whether the transaction's documents fit the buffer's 32-bit structures.
	template <class Transaction>
	bool fits(const Transaction& t) const noexcept {
		std::size_t entries = 0;
		for (const auto& o : t.ops) {
			for (const auto& f : o.doc.fields) { entries += f.terms.size() + (f.split_text ? f.text.size() / 2 + 1 : f.tokens.size()); }
		}
		return tuple_buffer::fits(t.ops.size(), entries);
	}

	// Writes the segment of the documents at `order` (or all) to `out`, positions gathered in `positions`.
	void flush(const std::array<std::uint8_t, 16>& segment_id, std::span<const ordinal_t> order, const builder_settings& settings, output& out, spill& positions) {
		segment_writer w;
		w.memory = settings.memory;
		w.structures = settings.structures;
		if (!value_docs_.empty() && !(settings.structures & values_plugin::structure)) { throw std::invalid_argument("strata: values in a segment without values"); }
		tuple_buffer::flush(segment_id, order, w, out, positions, [&](segment_writer& writer, const auto& plan) {
			if (value_docs_.empty()) { return; }
			const std::size_t docs = plan.all ? doc_count() : plan.order.size();
			for (std::size_t i = 0; i < docs; ++i) {
				each_value(plan.all ? ordinal_t(i) : plan.order[i], [&](std::uint32_t slot, std::string_view bytes) { writer.builder<values_plugin>().add_value(ordinal_t(i), slot, bytes); });
			}
		});
	}

	// The buffered documents' digest records (F21): the same as their documents' own. `dead`
	// says which ordinals are gone; `document_record(docid, version, key)` is the core's.
	template <class Dead, class DocumentRecord, class Emit>
	void digest(format::structure_bits configured, Dead&& dead, DocumentRecord&& document_record, Emit&& emit) const {
		for (ordinal_t o = 0; o < doc_count(); ++o) {
			if (dead(o)) { continue; }
			document_record(docid(o), version(o), key(o));
			if (configured & inverted_plugin::structure) { inverted_plugin::length_record(docid(o), length(o), emit); }
			if (configured & stored_plugin::structure) { stored_plugin::record(docid(o), stored(o), emit); }
			if (configured & values_plugin::structure) { each_value(o, [&](std::uint32_t slot, std::string_view bytes) { values_plugin::record(docid(o), slot, bytes, emit); }); }
		}
		if (configured & inverted_plugin::structure) {
			scan([&](std::string_view term, ordinal_t o, std::uint32_t wdf, std::span<const std::uint32_t> positions) {
				if (!dead(o)) { inverted_plugin::posting_record(docid(o), term, wdf, positions, emit); }
			});
		}
	}

private:
	// The document's values, kept beside the buffer's columns: only documents with values take
	// an entry, so a buffer of documents without them holds nothing here.
	void keep_values(ordinal_t ord, const std::vector<slot_value>& values) {
		value_docs_.push_back(ord);
		for (const auto& v : values) {
			value_bytes_ += v.bytes;
			value_ends_.push_back({v.slot, value_bytes_.size()});
		}
		value_doc_ends_.push_back(value_ends_.size());
		if (memory) {
			const std::uint64_t now = value_bytes_.capacity() + value_docs_.capacity() * sizeof(ordinal_t) + value_ends_.capacity() * sizeof(value_end) +
				value_doc_ends_.capacity() * sizeof(std::uint64_t);
			if (now > values_charged_) {
				memory->grow(now - values_charged_);
				values_charged_ = now;
			}
		}
	}

	struct value_end {
		std::uint32_t slot;
		std::uint64_t end;  // into value_bytes_
	};
	std::vector<posting_input> terms_in_;  // add()'s scratch
	std::vector<text_input> texts_in_;
	std::vector<ordinal_t> value_docs_;    // ordinals with values, ascending
	std::vector<std::uint64_t> value_doc_ends_;  // per such ordinal: its end in value_ends_
	std::vector<value_end> value_ends_;
	std::string value_bytes_;
	std::uint64_t values_charged_ = 0;
};

}  // namespace prospect
