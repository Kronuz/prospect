// Search over several indexes as one (docs/mechanisms/multi-index-search.md): the shards of
// one Xapiand index, each a prospect index with its own docids from 1, are searched as a
// single database. Weights read the statistics of every shard together, hits are numbered
// the way Xapian numbers a database of several shards, and a search can be exhaustive: key
// then relevance with a host-supplied key, and a visitor that sees every match exactly once.
#pragma once

#include "prospect/searcher.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace prospect {

// --- docids --------------------------------------------------------------------------------------

// Xapian's numbering of a database of `shards` sub-databases: shard s (from 0) document d (from 1)
// is document (d - 1) * shards + s + 1. Throws when the number does not fit in 64 bits.
inline std::uint64_t interleave_docid(std::uint64_t did, std::size_t shard, std::size_t shards) {
	if (did == 0 || shards == 0 || shard >= shards) { throw std::invalid_argument("prospect: bad shard docid"); }
	if (did - 1 > (std::numeric_limits<std::uint64_t>::max() - shard - 1) / shards) { throw std::overflow_error("prospect: interleaved docid overflows"); }
	return (did - 1) * shards + shard + 1;
}

struct shard_docid {
	std::size_t shard;
	std::uint64_t did;
	bool operator==(const shard_docid&) const = default;
};

// The shard and the shard's own docid of an interleaved docid.
inline shard_docid split_docid(std::uint64_t docid, std::size_t shards) {
	if (docid == 0 || shards == 0) { throw std::invalid_argument("prospect: bad docid"); }
	return {std::size_t((docid - 1) % shards), (docid - 1) / shards + 1};
}

// --- the snapshot --------------------------------------------------------------------------------

// One shard: its segments (index::segments gives them), and the largest docid it ever gave out
// (0: the largest docid of its segments, which a deletion at the end of the shard understates).
struct shard_input {
	std::vector<segment_snapshot> segments;
	std::uint64_t last_docid = 0;
};

// The shortest and longest document length a snapshot can hold, the way a weight scheme bounds
// its scores: a bound over every stored document, deleted ones included; {0, 0} when empty.
struct length_bounds {
	std::uint32_t lower = 0, upper = 0;
	bool operator==(const length_bounds&) const = default;
};

// The shards of one search, fixed for its duration. Statistics are the shards' together
// (document count, total length, term frequencies), so a weight is the one a single index
// holding every document would give. Shards may be empty; they still take a slot in the numbering.
class multi_snapshot {
public:
	multi_snapshot() = default;
	explicit multi_snapshot(std::vector<shard_input> shards) : shards_(std::move(shards)) {
		first_.reserve(shards_.size() + 1);
		for (const auto& s : shards_) {
			first_.push_back(all_.size());
			all_.insert(all_.end(), s.segments.begin(), s.segments.end());
		}
		first_.push_back(all_.size());
		// The numbering must fit for every document the shards can hold.
		for (std::size_t i = 0; i < shards_.size(); ++i) {
			if (const auto last = last_docid_of(i)) { (void)interleave_docid(last, i, shards_.size()); }
		}
	}

	std::size_t shard_count() const noexcept { return shards_.size(); }
	// Every segment of every shard, shard by shard: what a weight reads its statistics from
	// (snapshot_statistics, snapshot_df, term_table).
	std::span<const segment_snapshot> segments() const noexcept { return all_; }
	std::span<const segment_snapshot> segments(std::size_t shard) const { return std::span(all_).subspan(first_.at(shard), first_.at(shard + 1) - first_[shard]); }

	// The statistics of the shards together.
	statistics stats() const { return snapshot_statistics(all_); }
	// A term's document frequency over the shards, by field name: each shard resolves the name to its own id (D14).
	std::uint64_t df(std::string_view field, std::string_view term) const { return snapshot_df(all_, field, term); }

	// The docid of the shard's document `did` in the numbering across the shards.
	std::uint64_t docid(std::size_t shard, std::uint64_t did) const { return interleave_docid(did, shard, shards_.size()); }

	// The largest docid across the shards: the largest interleaved last docid of a shard, 0 when none.
	std::uint64_t last_docid() const {
		std::uint64_t best = 0;
		for (std::size_t i = 0; i < shards_.size(); ++i) {
			if (const auto last = last_docid_of(i)) { best = std::max(best, interleave_docid(last, i, shards_.size())); }
		}
		return best;
	}

	// Bounds on the length of any document: the smallest minimum and largest maximum of the
	// segments that hold documents.
	length_bounds doc_length_bounds() const {
		length_bounds b;
		bool any = false;
		for (const auto& s : all_) {
			if (!s.seg->core().doc_count()) { continue; }
			const auto& inv = s.seg->get<inverted_plugin>();
			b.lower = any ? std::min(b.lower, inv.min_length()) : inv.min_length();
			b.upper = std::max(b.upper, inv.max_length());
			any = true;
		}
		return b;
	}

private:
	std::uint64_t last_docid_of(std::size_t shard) const {
		if (shards_[shard].last_docid) { return shards_[shard].last_docid; }
		std::uint64_t last = 0;
		for (const auto& s : shards_[shard].segments) {
			if (s.seg->core().doc_count()) { last = std::max<std::uint64_t>(last, s.seg->core().max_docid()); }
		}
		return last;
	}
	std::vector<shard_input> shards_;
	std::vector<segment_snapshot> all_;
	std::vector<std::size_t> first_;  // shard s owns all_[first_[s], first_[s + 1])
};

// --- searching -----------------------------------------------------------------------------------

// How hits with equal rank are ordered by docid across the shards; dont_care orders ascending.
enum class docid_order : std::uint8_t { ascending, descending, dont_care };

// A matching document, as a key maker and a visitor see it. The scorer is positioned on it
// for the duration of the call, so score() is cheap and only computed when asked.
class match {
public:
	std::size_t shard = 0;
	const segment* seg = nullptr;
	ordinal_t ord = 0;
	std::uint64_t did = 0;    // the document's docid in its shard
	std::uint64_t docid = 0;  // across the shards
	double score() const {
		if (!scored_) {
			score_ = scorer_->score();
			scored_ = true;
		}
		return score_;
	}

private:
	friend class exhaustive_search;
	scorer* scorer_ = nullptr;
	mutable double score_ = 0;
	mutable bool scored_ = false;
};

// A key from a matching document (Xapian's KeyMaker): hits sort by memcmp of their keys.
using host_key = std::function<std::string(const match&)>;
// Sees a matching document (Xapian's MatchSpy).
using match_visitor = std::function<void(const match&)>;

struct search_options {
	std::size_t k = 10;  // hits kept: the window's end (first + maxitems)
	// Also report the number of matches. An exhaustive search always does.
	bool count = false;
	// Hits sort by key ascending, then by relevance, then by docid; `reverse_key` flips the key.
	// An empty `key` sorts by relevance, then by docid.
	host_key key;
	bool reverse_key = false;
	docid_order order = docid_order::ascending;
	// Called once for every live match, in shard, segment and ordinal order. Setting it makes
	// the search exhaustive: nothing is skipped by score.
	match_visitor visit;

	bool exhaustive() const { return bool(key) || bool(visit) || order == docid_order::descending; }
};

struct search_result {
	std::vector<doc_hit> hits;  // best first, docids across the shards
	std::optional<std::uint64_t> count;
};

// Every live match of every shard, once each: the collector behind key sorts, spies and
// descending docids. Hits are kept in a heap of the k best, so memory is k hits plus their keys.
class exhaustive_search {
public:
	exhaustive_search(const multi_snapshot& snap, const scorer_builder& build, const search_options& o) : snap_(snap), build_(build), o_(o) {}

	task::task<search_result> run(slice sl) {
		const std::size_t shards = snap_.shard_count();
		for (std::size_t s = 0; s < shards; ++s) {
			for (const auto& seg : snap_.segments(s)) { co_await scan(s, seg, shards, sl); }
		}
		std::sort_heap(heap_.begin(), heap_.end(), less());
		search_result out;
		out.count = count_;
		out.hits.reserve(heap_.size());
		for (const auto& e : heap_) { out.hits.push_back({e.docid, e.score}); }
		co_return out;
	}

private:
	struct entry {
		std::string key;
		double score;
		std::uint64_t docid;
	};
	// Whether a ranks before b.
	bool before(const entry& a, const entry& b) const {
		if (o_.key) {
			const int c = a.key.compare(b.key);
			if (c != 0) { return o_.reverse_key ? c > 0 : c < 0; }
		}
		if (a.score != b.score) { return a.score > b.score; }
		return o_.order == docid_order::descending ? a.docid > b.docid : a.docid < b.docid;
	}
	// A max-heap on rank: the worst hit kept is on top.
	struct by_rank {
		const exhaustive_search* self;
		bool operator()(const entry& a, const entry& b) const { return self->before(a, b); }
	};
	by_rank less() const { return {this}; }

	task::task<void> scan(std::size_t shard, const segment_snapshot& snap, std::size_t shards, slice sl) {
		scorer_ptr q = build_(*snap.seg);
		if (!q) { co_return; }
		if (snap.any_deleted()) { q = std::make_unique<live_scorer>(std::move(q), *snap.deleted); }
		match m;
		m.shard = shard;
		m.seg = snap.seg;
		m.scorer_ = q.get();
		window_gate gate;
		for (ordinal_t d = q->next(); d != no_more_docs; d = q->next()) {
			if (gate.due(d)) { co_await sl.checkpoint(); }
			++count_;
			m.ord = d;
			m.did = snap.seg->core().docid(d);
			m.docid = interleave_docid(m.did, shard, shards);
			m.scored_ = false;
			if (o_.visit) { o_.visit(m); }
			if (o_.k) { consider(m); }
		}
	}

	void consider(const match& m) {
		entry e{o_.key ? o_.key(m) : std::string(), 0.0, m.docid};
		const bool full = heap_.size() == o_.k;
		if (full && o_.key) {
			// A worse key than the worst kept cannot enter whatever its score: skip the scoring.
			const int c = e.key.compare(heap_.front().key);
			if (o_.reverse_key ? c < 0 : c > 0) { return; }
		}
		e.score = m.score();
		if (!full) {
			heap_.push_back(std::move(e));
			std::push_heap(heap_.begin(), heap_.end(), less());
		} else if (before(e, heap_.front())) {
			std::pop_heap(heap_.begin(), heap_.end(), less());
			heap_.back() = std::move(e);
			std::push_heap(heap_.begin(), heap_.end(), less());
		}
	}

	const multi_snapshot& snap_;
	const scorer_builder& build_;
	const search_options& o_;
	std::vector<entry> heap_;
	std::uint64_t count_ = 0;
};

// Searches the shards as one index. Without a key, a visitor or a descending docid order,
// each shard is searched with the pruned top-k of search() and the lists are merged, which is
// exact: a document in the top k of the whole is in the top k of its own shard, and within
// a shard docid order is interleaved docid order. The builder must weigh with the snapshot's
// statistics (snapshot_df and statistics over segments()).
// Everything it takes by reference must outlive the awaited task; search_pinned owns its inputs.
inline task::task<search_result> search_task(const multi_snapshot& snap, const scorer_builder& build, const search_options& o, slice sl) {
	if (o.exhaustive()) { co_return co_await exhaustive_search(snap, build, o).run(sl); }
	search_result out;
	std::uint64_t total = 0;
	const std::size_t shards = snap.shard_count();
	for (std::size_t s = 0; s < shards; ++s) {
		std::uint64_t n = 0;
		for (const auto& h : co_await search_task(snap.segments(s), build, o.k, o.count ? &n : nullptr, sl)) { out.hits.push_back({interleave_docid(h.docid, s, shards), h.score}); }
		total += n;
	}
	const auto best = [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; };
	if (out.hits.size() > o.k) {
		std::partial_sort(out.hits.begin(), out.hits.begin() + std::ptrdiff_t(o.k), out.hits.end(), best);
		out.hits.resize(o.k);
	} else {
		std::sort(out.hits.begin(), out.hits.end(), best);
	}
	if (o.count) { out.count = total; }
	co_return out;
}

// search_task run to completion on the calling thread; the caller's view outlives the call.
inline search_result search(const multi_snapshot& snap, const scorer_builder& build, const search_options& o = {}) {
	return run_unsliced(search_task(snap, build, o, slice{}));
}

// A search that owns its inputs, for a reactor: `pin` is the host's handle on the view the snapshot
// points into (strata's index::view, or any movable owner), and the snapshot, builder and options are
// moved into the frame, so the view stays pinned across every suspension and is released when the
// search completes, throws task::cancelled, or its task is destroyed while suspended.
template <typename Pin>
task::task<search_result> search_pinned(Pin pin, multi_snapshot snap, scorer_builder build, search_options o, task::budget& budget) {
	Pin held = std::move(pin);
	co_return co_await search_task(snap, build, o, slice{budget});
}

}  // namespace prospect
