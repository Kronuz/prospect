// Reordering a merge (docs/design/reordering.md D4): the merge's document-term graph written
// as a forward index in bounded passes, then recursive graph bisection over it, giving the
// order merge_segments writes. Documents are numbered by rank: their place in docid order.
#pragma once

#include "strata/bisection.hh"
#include "strata/merge.hh"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata::reorder {

struct settings {
	std::uint32_t min_df = 4096;            // graph terms hold at least this many live documents
	double max_df = 1.0;                    // and at most this share of them
	std::uint64_t memory = 256ull << 20;    // reorder_memory: bisection's working set, a pass's rows
	bisection::presets bisection;
};

// Bisection's anonymous working set (D4, "Memory and the fallback"): the order and each
// document's gain and position (20 bytes a document), and two degree counters per graph term
// for each of the 8 threads the preset allows.
inline std::uint64_t working_set(std::uint64_t docs, std::uint64_t terms) noexcept { return 20 * docs + 64 * terms; }

// The sources' live documents in docid order: [rank] = the document.
inline std::vector<merge_ref> docid_order(std::span<const merge_source> sources) {
	struct doc {
		std::uint64_t docid;
		merge_ref ref;
	};
	std::vector<doc> docs;
	for (std::uint32_t s = 0; s < sources.size(); ++s) {
		for (ordinal_t o = 0; o < sources[s].seg->doc_count(); ++o) {
			if (sources[s].live(o)) { docs.push_back({sources[s].seg->docid(o), {s, o}}); }
		}
	}
	std::sort(docs.begin(), docs.end(), [](const doc& a, const doc& b) { return a.docid < b.docid; });
	std::vector<merge_ref> refs(docs.size());
	for (std::size_t i = 0; i < docs.size(); ++i) { refs[i] = docs[i].ref; }
	return refs;
}

struct forward_stats {
	std::uint32_t docs = 0, terms = 0;
	std::uint64_t edges = 0, passes = 0;
	std::uint64_t rows_bytes = 0;  // the file: rows, zero padding to 8, then the offsets
};

// The graph of a written forward index.
inline bisection::graph forward_graph(std::string_view file, const forward_stats& st) {
	const std::uint64_t at = (st.rows_bytes + 7) & ~std::uint64_t(7), size = 8 * (std::uint64_t(st.docs) + 1);
	if (file.size() != at + size) { throw std::runtime_error("strata: reorder: the forward index reads back short"); }
	return {st.terms, st.docs, file.substr(0, st.rows_bytes), file.substr(at, size)};
}

// Writes the forward index of the sources' live documents (D4, "The forward index") to out:
// a first pass over the candidate terms (summed source df ≥ min_df) counts each one's live df,
// which decides membership, and each document's degree; then ranges of documents whose rows
// fit memory / 2 are filled, one pass over the graph terms each, and written out. Every
// document gets a row, empty when it holds no graph term.
inline forward_stats write_forward(std::span<const merge_source> sources, output& out, const settings& set, const task::cancel_token* cancel = nullptr,
	flush_charge* memory = nullptr) {
	auto check = [&] {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
	};
	std::uint64_t charged = 0;
	auto charge = [&](std::uint64_t n) {
		if (memory) { memory->grow(n); }
		charged += n;
	};
	constexpr std::uint32_t none = ~std::uint32_t(0);
	// [source][ordinal] = rank, or none for a deleted document.
	std::vector<std::vector<std::uint32_t>> rank(sources.size());
	std::uint64_t n = 0;
	{
		const auto refs = docid_order(sources);
		if (refs.size() >= none) { throw std::length_error("strata: reorder: more documents than ranks"); }
		for (std::size_t s = 0; s < sources.size(); ++s) {
			rank[s].assign(sources[s].seg->doc_count(), none);
			charge(rank[s].capacity() * 4);
		}
		for (std::uint32_t r = 0; r < refs.size(); ++r) { rank[refs[r].source][refs[r].ord] = r; }
		n = refs.size();
	}
	const bool any_deleted = std::any_of(sources.begin(), sources.end(), [](const merge_source& m) { return m.deleted != nullptr; });
	const double max_docs = set.max_df * double(n);
	// A graph term: where each source holds it.
	struct holder {
		std::uint32_t source;
		format::term_info info;
	};
	std::vector<std::vector<holder>> graph_terms;
	std::vector<std::uint32_t> degree(n, 0), ranks;
	charge(degree.capacity() * 4);
	std::uint64_t edges = 0;
	{
		std::deque<segment::term_cursor> cursors;
		std::vector<bool> more(sources.size());
		for (std::size_t s = 0; s < sources.size(); ++s) { more[s] = cursors.emplace_back(*sources[s].seg).next(); }
		std::string current;
		std::vector<holder> here;
		for (;;) {
			check();
			std::optional<std::string_view> next;
			for (std::size_t s = 0; s < sources.size(); ++s) {
				if (more[s] && (!next || cursors[s].term() < *next)) { next = cursors[s].term(); }
			}
			if (!next) { break; }
			current = *next;
			here.clear();
			std::uint64_t df = 0;
			for (std::uint32_t s = 0; s < sources.size(); ++s) {
				if (!more[s] || cursors[s].term() != current) { continue; }
				here.push_back({s, cursors[s].info()});
				df += cursors[s].info().df;
				more[s] = cursors[s].next();
			}
			if (df < set.min_df) { continue; }
			ranks.clear();
			if (any_deleted || double(df) <= max_docs) {
				for (const auto& h : here) {
					postings_iterator it(*sources[h.source].seg, h.info);
					for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
						if (const std::uint32_t r = rank[h.source][o]; r != none) { ranks.push_back(r); }
					}
				}
				df = ranks.size();
			}
			if (df < set.min_df || double(df) > max_docs) { continue; }
			if (graph_terms.size() == none) { throw std::length_error("strata: reorder: more graph terms than ids"); }
			for (std::uint32_t r : ranks) { ++degree[r]; }
			edges += ranks.size();
			graph_terms.push_back(here);
			charge(sizeof(holder) * here.size() + 32);
		}
	}
	ranks = {};
	forward_stats st;
	st.docs = std::uint32_t(n);
	st.terms = std::uint32_t(graph_terms.size());
	st.edges = edges;
	// The rows, range by range, in memory / 2: an edge's cell (4 bytes) and its worst encoding (5).
	const std::uint64_t per_range = std::max<std::uint64_t>(1, set.memory / 2 / 9);
	std::vector<std::uint64_t> offsets{0};
	offsets.reserve(n + 1);
	charge(offsets.capacity() * 8);
	std::vector<std::uint32_t> cells, fill, start;
	std::string buf;
	std::uint64_t pass_charge = 0;  // the largest range's buffers
	std::vector<ordinal_t> first(sources.size(), 0);  // per docid-ordered source, the range's first ordinal
	for (std::uint64_t r0 = 0; r0 < n;) {
		std::uint64_t r1 = r0, sum = 0;
		while (r1 < n && (r1 == r0 || sum + degree[r1] <= per_range)) { sum += degree[r1++]; }
		++st.passes;
		cells.assign(sum, 0);
		fill.assign(r1 - r0 + 1, 0);
		for (std::uint64_t r = r0; r < r1; ++r) { fill[r - r0 + 1] = fill[r - r0] + degree[r]; }
		start.assign(fill.begin(), fill.end() - 1);
		if (const std::uint64_t now = 4 * (cells.capacity() + fill.capacity() + start.capacity()) + 5 * sum; now > pass_charge) {
			charge(now - pass_charge);
			pass_charge = now;
		}
		for (std::size_t s = 0; s < sources.size(); ++s) {
			const auto& rk = rank[s];
			while (first[s] < rk.size() && (rk[first[s]] == none || rk[first[s]] < r0)) { ++first[s]; }
		}
		for (std::uint32_t t = 0; t < graph_terms.size(); ++t) {
			check();
			for (const auto& h : graph_terms[t]) {
				const segment& seg = *sources[h.source].seg;
				const auto& rk = rank[h.source];
				const bool ordered = seg.docid_ordered();  // ranks rise with ordinals
				postings_iterator it(seg, h.info);
				ordinal_t o = !ordered ? it.next() : first[h.source] < rk.size() ? it.advance(ordinal_t(first[h.source])) : no_more_docs;
				for (; o != no_more_docs; o = it.next()) {
					const std::uint32_t r = rk[o];
					if (r == none) { continue; }
					if (r >= r1) {
						if (ordered) { break; }
						continue;
					}
					if (r >= r0) { cells[start[r - r0]++] = t; }
				}
			}
		}
		buf.clear();
		for (std::uint64_t r = r0; r < r1; ++r) {
			std::uint32_t prev = 0;
			for (std::uint32_t i = fill[r - r0]; i < fill[r - r0 + 1]; ++i) {
				for (std::uint32_t v = cells[i] - prev;; v >>= 7) {
					if (v < 0x80) {
						buf.push_back(char(v));
						break;
					}
					buf.push_back(char(0x80 | (v & 0x7f)));
				}
				prev = cells[i];
			}
			offsets.push_back(st.rows_bytes + buf.size());
		}
		out.write(buf.data(), buf.size());
		st.rows_bytes += buf.size();
		r0 = r1;
	}
	static constexpr char zeros[8] = {};
	out.write(zeros, std::size_t((8 - st.rows_bytes % 8) % 8));
	out.write(reinterpret_cast<const char*>(offsets.data()), offsets.size() * 8);
	if (memory) { memory->shrink(std::min(memory->held(), charged)); }
	return st;
}

// The order a reordering merge writes: [new ordinal] = the source document, or empty when
// docid order stands (no graph, bisection keeps it, or its working set exceeds memory).
inline std::vector<merge_ref> order(std::span<const merge_source> sources, scratch& file, const settings& set = {}, const task::cancel_token* cancel = nullptr,
	flush_charge* memory = nullptr, forward_stats* stats = nullptr) {
	const forward_stats st = write_forward(sources, file.out(), set, cancel, memory);
	if (stats) { *stats = st; }
	if (st.terms == 0 || st.docs <= set.bisection.min_partition || working_set(st.docs, st.terms) > set.memory) { return {}; }
	if (memory) { memory->grow(working_set(st.docs, st.terms)); }
	const auto perm = bisection::order(forward_graph(file.bytes(), st), set.bisection, cancel);
	if (memory) { memory->shrink(std::min(memory->held(), working_set(st.docs, st.terms))); }
	bool identity = true;
	for (std::uint32_t i = 0; i < perm.size() && identity; ++i) { identity = perm[i] == i; }
	if (identity) { return {}; }
	const auto refs = docid_order(sources);
	std::vector<merge_ref> out(perm.size());
	for (std::size_t i = 0; i < perm.size(); ++i) { out[i] = refs[perm[i]]; }
	return out;
}

}  // namespace strata::reorder

namespace strata {

// Format 12's order source: bisection over the inverted plugin's postings (D4).
template <>
struct order_source<search_layout_12> {
	static constexpr bool exists = true;
	static std::vector<merge_ref> order(std::span<const merge_source> sources, scratch& file, const reorder::settings& set, const task::cancel_token* cancel,
		flush_charge* memory) {
		return reorder::order(sources, file, set, cancel, memory);
	}
};

}  // namespace strata
