// Merging segments (Phase 3 design, "Merges"): one segment holding every live document of
// its sources, in docid order (ordinal order is docid order in the result), with deleted
// documents and the terms only they held dropped. Per-document data (lengths, stored
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

// The merged segment's documents: how many, and each source ordinal's new ordinal.
struct merge_plan {
	std::uint64_t doc_count = 0;
	std::vector<std::vector<ordinal_t>> ordinals;  // [source][old ordinal] = new, or no_more_docs
};

// Writes the merge of `sources` to sink (positions through spill). `key_prefix` names the
// documents' key terms (empty: none), which the result's key filter covers. Throws
// std::logic_error if two live documents share a docid. With `cancel`, it checks it per
// document, per term, and per stored frame, and throws task::cancelled; with `memory`,
// its working memory is charged there.
inline merge_plan merge_segments(std::span<const merge_source> sources, const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill,
	std::string_view key_prefix = {}, std::uint8_t region_shift = format::default_region_shift, const task::cancel_token* cancel = nullptr,
	flush_charge* memory = nullptr) {
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
	std::sort(docs.begin(), docs.end(), [](const doc& a, const doc& b) { return a.docid < b.docid; });
	segment_writer w;
	w.region_shift = region_shift;
	w.memory = memory;
	w.cancel = cancel;
	for (std::size_t i = 0; i < docs.size(); ++i) {
		check();
		const auto& d = docs[i];
		if (i && docs[i - 1].docid == d.docid) { throw std::logic_error("strata: merge: two live copies of a docid"); }
		const segment& seg = *sources[d.source].seg;
		auto k = keys[d.source].find(d.ord);
		plan.ordinals[d.source][d.ord] = w.add_document(d.docid, seg.length(d.ord), seg.stored(d.ord), seg.version(d.ord), k == keys[d.source].end() ? std::string_view() : std::string_view(k->second));
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
