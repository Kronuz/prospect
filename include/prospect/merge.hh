// Merging search segments: the inverted plugin's merge (the union of the inputs' dictionaries,
// each term's live postings remapped) and merge_segments, the search layout's entry point.
// The core's plan and the other plugins' merges are strata/core/merge.hh's.
#pragma once

#include "prospect/reader.hh"
#include "prospect/strata.hh"
#include "prospect/writer.hh"
#include "strata/core/merge.hh"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prospect {

using merge_source = basic_merge_source<search_layout>;

// Terms: the union of the inputs' dictionaries in order, one cursor per input (a cursor
// holds one dictionary block, not the dictionary), each term's live postings remapped.
inline built inverted_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	inverted_builder b;
	b.memory = m.memory;
	b.norms = (m.structures & format::bit::norms) != 0;
	for (const auto& src : m.order) { (void)b.begin(in[src.source]->length(src.ord)); }
	std::deque<reader::term_cursor> terms;
	std::vector<bool> more(in.size());
	for (std::size_t s = 0; s < in.size(); ++s) { more[s] = terms.emplace_back(*in[s]).next(); }
	// The plan: each field's terms and bytes over the inputs (an upper bound where they overlap).
	std::map<std::uint32_t, fielddict::field_plan> by_field;
	for (const reader* r : in) { r->dictionary().add_plan(by_field); }
	std::vector<fielddict::field_plan> plan;
	for (auto& [field, p] : by_field) { plan.push_back(p); }
	std::string current;
	struct posting {
		ordinal_t ord;
		std::uint32_t wdf;
		std::vector<std::uint32_t> positions;  // absolute
	};
	std::vector<posting> postings;
	std::uint64_t charged = 0;  // the per-term postings' high-water mark
	b.finish(sink, sink.held(), std::move(plan), [&](term_postings& tp) -> std::optional<std::string_view> {
		for (;;) {
			m.check();
			// The smallest (field, term) not yet merged.
			std::size_t first = in.size();
			for (std::size_t s = 0; s < in.size(); ++s) {
				if (!more[s]) { continue; }
				if (first == in.size() || terms[s].field() < terms[first].field() || (terms[s].field() == terms[first].field() && terms[s].term() < terms[first].term())) { first = s; }
			}
			if (first == in.size()) { return std::nullopt; }
			const std::uint32_t field = terms[first].field();
			current.clear();
			fielddict::put_field(current, field);
			current += terms[first].term();
			const std::string_view term = std::string_view(current).substr(current.size() - terms[first].term().size());
			postings.clear();
			for (std::size_t s = 0; s < in.size(); ++s) {
				if (!more[s] || terms[s].field() != field || terms[s].term() != term) { continue; }
				const format::term_info info = terms[s].info();
				more[s] = terms[s].next();
				postings_iterator it(*in[s], info);
				positions_reader pr(*in[s], info);
				for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
					if (!m.inputs[s].live(o)) { continue; }
					const auto p = pr.read(it);
					postings.push_back({m.inputs[s].new_ordinal[o], it.wdf(), std::vector<std::uint32_t>(p.begin(), p.end())});
				}
			}
			if (postings.empty()) { continue; }  // only deleted documents held it
			if (m.memory) {
				std::uint64_t now = postings.capacity() * sizeof(posting);
				for (const auto& p : postings) { now += p.positions.capacity() * sizeof(std::uint32_t); }
				if (now > charged) {
					m.memory->grow(now - charged);
					charged = now;
				}
			}
			std::sort(postings.begin(), postings.end(), [](const posting& a, const posting& b) { return a.ord < b.ord; });
			tp.clear();
			for (const auto& p : postings) {
				if (!p.positions.empty()) { tp.any_positions = true; }
				tp.ords.push_back(p.ord);
				tp.wdfs.push_back(p.wdf);
				tp.counts.push_back(std::uint32_t(p.positions.size()));
				tp.cf += p.wdf;
				if (p.wdf) { tp.all_zero_wdf = false; }
				if (p.positions.size() != p.wdf) { tp.counts_are_wdf = false; }
				std::uint32_t prev = 0;
				for (auto q : p.positions) {
					tp.positions.push_back(q - prev);
					prev = q;
				}
			}
			return std::string_view(current);
		}
	});
	return {format::bit::inverted | (b.norms ? format::bit::norms : 0)};
}

// Writes the merge of `sources` to sink (positions through spill). Each live document keeps
// its key, from the keys reader. Throws std::logic_error if
// two live documents share a docid. With `cancel`, it throws task::cancelled when asked.
// Stored records are read a frame at a time per source; out of docid order, `records`
// (when given) takes each source's live records, every frame decoded once, and the output
// reads them from there (reordering review, P1-R4 (b)).
// The output is in docid order, unless `order` gives it: [new ordinal] = the source
// document, every live document exactly once (std::invalid_argument otherwise). Without
// `order`, a single source whose ordinals are not in docid order keeps its order.
inline merge_plan merge_segments(std::span<const merge_source> sources, const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill,
	const merge_options& options = {}) {
	return detail::merge_layout(sources, segment_id, sink, positions_spill, options);
}

}  // namespace prospect
