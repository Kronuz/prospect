// Merging segments (Phase 3 design, "Merges"; layers.md L6): one segment holding every live
// document of its sources, in docid order or in an order the caller gives (a reordering
// merge, docs/design/reordering.md D3), with deleted documents and the terms only they
// held dropped. The core plans the output (its documents, their order, each input's
// ordinal map, and their docids, versions, and keys); each plugin then rebuilds its own
// sections from its readers, in layout order, so a merge writes exactly what a flush of
// the same documents would.
#pragma once

#include "strata/reader.hh"
#include "strata/writer.hh"
#include "task.hh"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace strata {

template <class Layout>
struct basic_merge_source {
	const basic_segment<Layout>* seg = nullptr;
	const std::vector<std::uint64_t>* deleted = nullptr;  // bit per ordinal; null: none deleted
	bool live(ordinal_t o) const { return !deleted || !(((*deleted)[o >> 6] >> (o & 63)) & 1); }
};
using merge_source = basic_merge_source<search_layout_12>;

// A document of a merge's sources: which source, and its ordinal there.
struct merge_ref {
	std::uint32_t source;
	ordinal_t ord;
};

// The merged segment's documents: how many, and each source ordinal's new ordinal.
struct merge_plan {
	std::uint64_t doc_count = 0;
	std::vector<std::vector<ordinal_t>> ordinals;  // [source][old ordinal] = new, or no_more_docs
};

// One input as a plugin's merge sees it (L6).
struct merge_input {
	const core_segment* core = nullptr;                   // docids and document count
	const std::vector<std::uint64_t>* deleted = nullptr;  // bit per ordinal; null: none deleted
	std::span<const ordinal_t> new_ordinal;               // [old ordinal] = new, or no_more_docs
	bool live(ordinal_t o) const { return !deleted || !(((*deleted)[o >> 6] >> (o & 63)) & 1); }
};

// What the core hands every plugin's merge (L6): the output's documents in output order.
struct merge_context {
	std::uint32_t version = format::version;  // the format being written
	std::uint8_t region_shift = format::default_region_shift;
	std::uint32_t doc_count = 0;              // documents in the output
	std::span<const merge_input> inputs;
	std::span<const merge_ref> order;         // [new ordinal] = its source
	std::span<const doc_fields> fields;       // [new ordinal] = docid, version, key
	bool docid_order = true;                  // false when the output is reordered (P1)
	const task::cancel_token* cancel = nullptr;
	flush_charge* memory = nullptr;           // the merge's working memory, when budgeted
	scratch* records = nullptr;               // out of docid order: where stored records spill
	void check() const {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
	}
	void charge(std::uint64_t n) const {
		if (memory) { memory->grow(n); }
	}
};

// Terms: the union of the inputs' dictionaries in order, one cursor per input (a cursor
// holds one dictionary block, not the dictionary), each term's live postings remapped.
inline built inverted_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	inverted_builder b;
	b.memory = m.memory;
	for (const auto& src : m.order) { (void)b.begin(in[src.source]->length(src.ord)); }
	std::deque<reader::term_cursor> terms;
	std::vector<bool> more(in.size());
	for (std::size_t s = 0; s < in.size(); ++s) { more[s] = terms.emplace_back(*in[s]).next(); }
	// The dictionary's alphabet: every byte the inputs' dictionaries use.
	std::array<bool, 256> used{};
	for (const reader* r : in) {
		for (char c : r->dictionary().alphabet_bytes()) { used[std::uint8_t(c)] = true; }
	}
	std::string alphabet;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { alphabet.push_back(char(c)); }
	}
	std::string current;
	struct posting {
		ordinal_t ord;
		std::uint32_t wdf;
		std::vector<std::uint32_t> positions;  // absolute
	};
	std::vector<posting> postings;
	std::uint64_t charged = 0;  // the per-term postings' high-water mark
	b.finish(sink, sink.held(), alphabet, [&](term_postings& tp) -> std::optional<std::string_view> {
		for (;;) {
			m.check();
			// The smallest term not yet merged.
			std::optional<std::string_view> next;
			for (std::size_t s = 0; s < in.size(); ++s) {
				if (more[s] && (!next || terms[s].term() < *next)) { next = terms[s].term(); }
			}
			if (!next) { return std::nullopt; }
			current = *next;
			postings.clear();
			for (std::size_t s = 0; s < in.size(); ++s) {
				if (!more[s] || terms[s].term() != current) { continue; }
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
	return {1, b.legacy_stats()};
}

// Rebuilt from the core's fields: each new ordinal's version and key.
inline built keys_plugin::merge(const merge_context& m, std::span<const reader* const>, section_sink& sink) {
	keys_builder b;
	b.memory = m.memory;
	for (const auto& f : m.fields) { b.add(f); }
	b.finish(sink);
	return {1, {}};
}

// Rebuilt record by record in output order. In docid order each input's frames are read
// in turn through one cached frame per input; out of it, with `records`, every input's
// live records are first spilled there, each frame decoded once (reordering review,
// P1-R4 (b)).
inline built stored_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	std::vector<std::vector<std::uint64_t>> spilled;  // [input][ordinal] .. [ordinal + 1]: its bytes
	std::string_view spilled_bytes;
	if (m.records && !m.docid_order) {
		spilled.resize(in.size());
		std::uint64_t at = 0;
		reader::stored_frame frame;
		for (std::size_t s = 0; s < in.size(); ++s) {
			const reader& r = *in[s];
			const std::uint64_t docs = m.inputs[s].core->doc_count();
			spilled[s].assign(docs + 1, at);  // this input's records start at `at`
			m.charge(spilled[s].capacity() * 8);
			const std::uint32_t frames = r.stored_frames();
			ordinal_t next = 0;
			for (std::uint32_t f = 0; f < frames; ++f) {
				m.check();
				r.decode_stored_frame(f, frame);
				for (ordinal_t o = frame.first; o < frame.first + frame.starts.size() - 1; ++o) {
					for (; next < o; ++next) { spilled[s][next + 1] = at; }
					if (m.inputs[s].live(o)) {
						const auto rec = frame.record(o);
						m.records->out().write(rec.data(), rec.size());
						at += rec.size();
					}
					spilled[s][o + 1] = at;
					next = o + 1;
				}
			}
			for (; next < docs; ++next) { spilled[s][next + 1] = at; }
		}
		spilled_bytes = m.records->bytes();
		if (spilled_bytes.size() != at) { throw std::runtime_error("strata: merge: the stored records read back short"); }
	}
	std::vector<reader::stored_frame> frames(spilled.empty() ? in.size() : 0);
	stored_builder b;
	b.memory = m.memory;
	b.cancel = m.cancel;
	for (const auto& src : m.order) {
		m.check();
		const std::string_view rec = spilled.empty() ? in[src.source]->stored(src.ord, frames[src.source])
			: spilled_bytes.substr(spilled[src.source][src.ord], spilled[src.source][src.ord + 1] - spilled[src.source][src.ord]);
		if (rec.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		b.add(rec);
	}
	b.finish(sink);
	return {1, {}};
}

// Where a reordering merge's order comes from (L6, reordering review P1-R4 (d)): a layout
// names at most one source, which the core calls before any plugin's merge. None by default.
template <class Layout>
struct order_source {
	static constexpr bool exists = false;
};

namespace detail {

// The core of a merge (L6) over a layout: the plan, then each layout entry in order.
template <class... Entries>
merge_plan merge_layout(std::span<const basic_merge_source<layout<Entries...>>> sources, const std::array<std::uint8_t, 16>& segment_id, output& out,
	spill& positions_spill, std::string_view key_prefix, std::uint8_t region_shift, const task::cancel_token* cancel, flush_charge* memory,
	std::span<const merge_ref> order, scratch* records) {
	using segment_type = basic_segment<layout<Entries...>>;
	constexpr bool keyed = (std::is_same_v<Entries, keys_plugin> || ...);
	auto check = [&] {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
	};
	auto charge = [&](std::uint64_t n) {
		if (memory) { memory->grow(n); }
	};
	check();
	merge_plan plan;
	plan.ordinals.resize(sources.size());
	// Each live document's key, if any, from the keys reader (L3).
	std::vector<std::unordered_map<ordinal_t, std::string>> keys(sources.size());
	if constexpr (keyed) {
		if (!key_prefix.empty()) {
			for (std::size_t s = 0; s < sources.size(); ++s) {
				const segment_type& seg = *sources[s].seg;
				seg.template reader<keys_plugin>().scan(key_prefix, seg.template deps<keys_plugin>(), [&](std::string_view key, ordinal_t o) {
					if (sources[s].live(o)) {
						keys[s][o] = std::string(key);
						charge(64 + key.size());  // a map node and its key
					}
					return true;
				});
			}
		}
	}
	// Documents in docid order.
	struct doc {
		std::uint64_t docid;
		std::size_t source;
		ordinal_t ord;
	};
	std::vector<doc> docs;
	for (std::size_t s = 0; s < sources.size(); ++s) {
		const segment_type& seg = *sources[s].seg;
		plan.ordinals[s].assign(seg.doc_count(), no_more_docs);
		for (ordinal_t o = 0; o < seg.doc_count(); ++o) {
			if (sources[s].live(o)) { docs.push_back({seg.docid(o), s, o}); }
		}
		charge(plan.ordinals[s].capacity() * sizeof(ordinal_t));
	}
	charge(docs.capacity() * sizeof(doc));
	// A single permuted source keeps its order (docs are still in its ordinal order here).
	const bool keep_source_order = order.empty() && sources.size() == 1 && !sources[0].seg->docid_ordered();
	std::vector<doc> by_docid;
	if (keep_source_order) { by_docid = docs, charge(by_docid.capacity() * sizeof(doc)); }
	auto& sorted = keep_source_order ? by_docid : docs;
	std::sort(sorted.begin(), sorted.end(), [](const doc& a, const doc& b) { return a.docid < b.docid; });
	for (std::size_t i = 1; i < sorted.size(); ++i) {
		if (sorted[i - 1].docid == sorted[i].docid) { throw std::logic_error("strata: merge: two live copies of a docid"); }
	}
	if (!order.empty()) {
		if (order.size() != docs.size()) { throw std::invalid_argument("strata: merge order must name every live document once"); }
		docs.clear();
		for (const auto& r : order) {
			if (r.source >= sources.size() || r.ord >= sources[r.source].seg->doc_count() || !sources[r.source].live(r.ord) ||
				plan.ordinals[r.source][r.ord] != no_more_docs) {
				throw std::invalid_argument("strata: merge order must name every live document once");
			}
			plan.ordinals[r.source][r.ord] = 0;  // seen; numbered below
			docs.push_back({sources[r.source].seg->docid(r.ord), r.source, r.ord});
		}
	}
	// Ordinals are 32-bit and the largest value means no more documents.
	if (docs.size() > std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
	const bool docid_order = order.empty() && !keep_source_order;
	// The output in output order: its sources, docids, and per-document fields.
	std::vector<merge_ref> refs;
	std::vector<std::uint64_t> docids;
	std::vector<doc_fields> fields;
	refs.reserve(docs.size()), docids.reserve(docs.size()), fields.reserve(docs.size());
	charge(refs.capacity() * sizeof(merge_ref) + docids.capacity() * sizeof(std::uint64_t) + fields.capacity() * sizeof(doc_fields));
	for (std::size_t i = 0; i < docs.size(); ++i) {
		check();
		const auto& d = docs[i];
		if (d.docid == 0) { throw std::invalid_argument("strata: docids must increase within a segment"); }
		plan.ordinals[d.source][d.ord] = ordinal_t(i);
		refs.push_back({std::uint32_t(d.source), d.ord});
		docids.push_back(d.docid);
		doc_fields f{d.docid, 0, {}};
		if constexpr (keyed) {
			f.version = sources[d.source].seg->template reader<keys_plugin>().version(d.ord);
			if (auto k = keys[d.source].find(d.ord); k != keys[d.source].end()) { f.key = k->second; }
		}
		fields.push_back(f);
	}
	plan.doc_count = docs.size();
	std::vector<doc>().swap(docs);
	std::vector<doc>().swap(by_docid);

	std::vector<merge_input> inputs(sources.size());
	for (std::size_t s = 0; s < sources.size(); ++s) { inputs[s] = {&sources[s].seg->core(), sources[s].deleted, plan.ordinals[s]}; }
	merge_context m;
	m.region_shift = region_shift;
	m.doc_count = std::uint32_t(refs.size());
	m.inputs = inputs;
	m.order = refs;
	m.fields = fields;
	m.docid_order = docid_order;
	m.cancel = cancel;
	m.memory = memory;
	m.records = records;

	section_sink sink(out, region_shift, &positions_spill);
	begin_segment(sink, segment_id);
	std::string legacy_stats;
	auto merge_entry = [&]<class E>(std::type_identity<E>) {
		if constexpr (std::is_same_v<E, docids_section>) {
			write_docids(sink, memory, docids);
		} else {
			std::vector<const typename E::reader*> readers;
			readers.reserve(sources.size());
			for (const auto& src : sources) { readers.push_back(&src.seg->template reader<E>()); }
			built b = E::merge(m, readers, sink);
			if (!b.legacy_stats.empty()) { legacy_stats = std::move(b.legacy_stats); }
		}
	};
	(merge_entry(std::type_identity<Entries>{}), ...);
	finish_segment(sink, docids, legacy_stats);
	return plan;
}

}  // namespace detail

// Writes the merge of `sources` to sink (positions through spill). `key_prefix` names the
// documents' key terms (empty: none), which the result's key filter covers. Throws
// std::logic_error if two live documents share a docid. With `cancel`, it checks it per
// document, per term, and per stored frame, and throws task::cancelled; with `memory`,
// its working memory is charged there. Stored records are read a frame at a time per
// source; out of docid order, `records` (when given) takes each source's live records,
// every frame decoded once, and the output reads them from there (reordering review,
// P1-R4 (b)).
// The output is in docid order, unless `order` gives it: [new ordinal] = the source
// document, every live document exactly once (std::invalid_argument otherwise). Without
// `order`, a single source whose ordinals are not in docid order keeps its order.
inline merge_plan merge_segments(std::span<const merge_source> sources, const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill,
	std::string_view key_prefix = {}, std::uint8_t region_shift = format::default_region_shift, const task::cancel_token* cancel = nullptr,
	flush_charge* memory = nullptr, std::span<const merge_ref> order = {}, scratch* records = nullptr) {
	return detail::merge_layout(sources, segment_id, sink, positions_spill, key_prefix, region_shift, cancel, memory, order, records);
}

}  // namespace strata
