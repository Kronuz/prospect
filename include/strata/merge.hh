// Merging segments (Phase 3 design, "Merges"): one segment holding every live document of
// its sources, in docid order or in an order the caller gives (a reordering merge,
// docs/design/reordering.md D3), with deleted documents and the terms only they held
// dropped. Per-document data (lengths, stored
// records, versions, keys) copies by docid; postings and positions re-encode term by term
// through segment_writer::flush_from, so a merge writes exactly what a flush would.
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
#include <unordered_map>
#include <vector>

namespace strata {

struct merge_source {
	const segment* seg = nullptr;
	const std::vector<std::uint64_t>* deleted = nullptr;  // bit per ordinal; null: none deleted
	bool live(ordinal_t o) const { return !deleted || !(((*deleted)[o >> 6] >> (o & 63)) & 1); }
};

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
	auto check = [&] {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
	};
	auto charge = [&](std::uint64_t n) {
		if (memory) { memory->grow(n); }
	};
	check();
	merge_plan plan;
	plan.ordinals.resize(sources.size());
	// Each live document's key term, if any.
	std::vector<std::unordered_map<ordinal_t, std::string>> keys(sources.size());
	if (!key_prefix.empty()) {
		for (std::size_t s = 0; s < sources.size(); ++s) {
			sources[s].seg->scan_dictionary(key_prefix, [&](std::string_view term, std::string_view) {
				if (!term.starts_with(key_prefix)) { return false; }
				auto info = sources[s].seg->term(term);
				postings_iterator it(*sources[s].seg, *info);
				for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
					if (sources[s].live(o)) {
						keys[s][o] = std::string(term);
						charge(64 + term.size());  // a map node and its key
					}
				}
				return true;
			});
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
		const segment& seg = *sources[s].seg;
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
			plan.ordinals[r.source][r.ord] = 0;  // seen; the writer assigns it below
			docs.push_back({sources[r.source].seg->docid(r.ord), r.source, r.ord});
		}
	}
	// Stored records: out of docid order, spilled once per source frame into `records`,
	// [source][ordinal] .. [ordinal + 1] their bytes there; otherwise a frame cache per source.
	std::vector<std::vector<std::uint64_t>> spilled;
	std::string_view spilled_bytes;
	if (records && (!order.empty() || keep_source_order)) {
		spilled.resize(sources.size());
		std::uint64_t at = 0;
		segment::stored_frame frame;
		for (std::size_t s = 0; s < sources.size(); ++s) {
			const segment& seg = *sources[s].seg;
			spilled[s].assign(seg.doc_count() + 1, at);  // this source's records start at `at`
			charge(spilled[s].capacity() * 8);
			const std::uint32_t frames = seg.stored_frames();
			ordinal_t next = 0;
			for (std::uint32_t f = 0; f < frames; ++f) {
				check();
				seg.decode_stored_frame(f, frame);
				for (ordinal_t o = frame.first; o < frame.first + frame.starts.size() - 1; ++o) {
					for (; next < o; ++next) { spilled[s][next + 1] = at; }
					if (sources[s].live(o)) {
						const auto rec = frame.record(o);
						records->out().write(rec.data(), rec.size());
						at += rec.size();
					}
					spilled[s][o + 1] = at;
					next = o + 1;
				}
			}
			for (; next < seg.doc_count(); ++next) { spilled[s][next + 1] = at; }
		}
		spilled_bytes = records->bytes();
		if (spilled_bytes.size() != at) { throw std::runtime_error("strata: merge: the stored records read back short"); }
	}
	std::vector<segment::stored_frame> frames(spilled.empty() ? sources.size() : 0);
	auto stored_of = [&](std::size_t s, ordinal_t o) -> std::string_view {
		if (spilled.empty()) { return sources[s].seg->stored(o, frames[s]); }
		return spilled_bytes.substr(spilled[s][o], spilled[s][o + 1] - spilled[s][o]);
	};
	segment_writer w;
	w.region_shift = region_shift;
	w.memory = memory;
	w.cancel = cancel;
	w.permuted = !order.empty() || keep_source_order;  // the docids section records the order written
	for (std::size_t i = 0; i < docs.size(); ++i) {
		check();
		const auto& d = docs[i];
		const segment& seg = *sources[d.source].seg;
		auto k = keys[d.source].find(d.ord);
		plan.ordinals[d.source][d.ord] = w.add_document(d.docid, seg.length(d.ord), stored_of(d.source, d.ord), seg.version(d.ord), k == keys[d.source].end() ? std::string_view() : std::string_view(k->second));
	}
	plan.doc_count = docs.size();
	// Terms: the union of the sources' dictionaries, in order, one cursor per source
	// (a cursor holds one dictionary block, not the dictionary).
	std::deque<segment::term_cursor> terms;
	std::vector<bool> more(sources.size());
	for (std::size_t s = 0; s < sources.size(); ++s) {
		more[s] = terms.emplace_back(*sources[s].seg).next();
	}
	// The dictionary's alphabet: every byte the sources' dictionaries use.
	std::array<bool, 256> used{};
	for (const auto& src : sources) {
		for (char c : src.seg->dictionary().alphabet_bytes()) { used[std::uint8_t(c)] = true; }
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
	w.flush_from(segment_id, sink, positions_spill, alphabet, [&](term_postings& tp) -> std::optional<std::string_view> {
		for (;;) {
			check();
			// The smallest term not yet merged.
			std::optional<std::string_view> next;
			for (std::size_t s = 0; s < sources.size(); ++s) {
				if (more[s] && (!next || terms[s].term() < *next)) { next = terms[s].term(); }
			}
			if (!next) { return std::nullopt; }
			current = *next;
			postings.clear();
			for (std::size_t s = 0; s < sources.size(); ++s) {
				if (!more[s] || terms[s].term() != current) { continue; }
				const format::term_info info = terms[s].info();
				more[s] = terms[s].next();
				postings_iterator it(*sources[s].seg, info);
				positions_reader pr(*sources[s].seg, info);
				for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
					if (!sources[s].live(o)) { continue; }
					const auto p = pr.read(it);
					postings.push_back({plan.ordinals[s][o], it.wdf(), std::vector<std::uint32_t>(p.begin(), p.end())});
				}
			}
			if (postings.empty()) { continue; }  // only deleted documents held it
			if (memory) {
				std::uint64_t now = postings.capacity() * sizeof(posting);
				for (const auto& p : postings) { now += p.positions.capacity() * sizeof(std::uint32_t); }
				if (now > charged) {
					memory->grow(now - charged);
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
	return plan;
}

}  // namespace strata
