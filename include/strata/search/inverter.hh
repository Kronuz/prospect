// The search layout's inverter for the core's index: the tuple buffer (tuple_buffer.hh) behind the
// interface core/index.hh asks of a layout's `inverter` (layers.md L10): documents inverted as they
// are accepted at one flush thread, flushed as the segment a segment_writer would write from the
// same documents, and digested as their records.
#pragma once

#include "strata/search/reader.hh"
#include "strata/search/tuple_buffer.hh"
#include "strata/search/writer.hh"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace strata {

class inverting_buffer : public detail::tuple_buffer {
public:
	// A document that passed stage 1, its terms and tokens or split text inverted; returns its
	// ordinal. `key` is borrowed from the transaction until the next flush.
	ordinal_t add(std::uint64_t docid, std::uint64_t version, std::string_view key, const document& d) {
		terms_in_.clear();
		for (const auto& t : d.terms) { terms_in_.push_back({t.term, t.wdf, t.positions}); }
		return d.split_text ? tuple_buffer::add_split(docid, terms_in_, d.text, d.stored, version, key)
							: tuple_buffer::add(docid, terms_in_, d.text, d.tokens, d.length, d.stored, version, key);
	}

	// Whether the transaction's documents fit the buffer's 32-bit structures.
	template <class Transaction>
	bool fits(const Transaction& t) const noexcept {
		std::size_t entries = 0;
		for (const auto& o : t.ops) { entries += o.doc.terms.size() + (o.doc.split_text ? o.doc.text.size() / 2 + 1 : o.doc.tokens.size()); }
		return tuple_buffer::fits(t.ops.size(), entries);
	}

	// Writes the segment of the documents at `order` (or all) to `out`, positions gathered in `positions`.
	void flush(const std::array<std::uint8_t, 16>& segment_id, std::span<const ordinal_t> order, const builder_settings& settings, output& out, spill& positions) {
		segment_writer w;
		w.memory = settings.memory;
		w.key_prefix = std::string(settings.key_prefix);
		w.structures = settings.structures;
		tuple_buffer::flush(segment_id, order, w, out, positions);
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
		}
		if (configured & inverted_plugin::structure) {
			scan([&](std::string_view term, ordinal_t o, std::uint32_t wdf, std::span<const std::uint32_t> positions) {
				if (!dead(o)) { inverted_plugin::posting_record(docid(o), term, wdf, positions, emit); }
			});
		}
	}

private:
	std::vector<posting_input> terms_in_;  // add()'s scratch
};

}  // namespace strata
