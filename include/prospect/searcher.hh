// Search across an index's segments (Phase 3 design, "Statistics and ranking"): weights
// read the snapshot's statistics, deleted documents are skipped, each segment prunes
// against the k-th score found so far, and hits rank by (score descending, docid
// ascending).
#pragma once

#include "prospect/search.hh"
#include "strata/core/deleted_bits.hh"
#include "strata/core/fields.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prospect {

// A segment as a search sees it: its deleted ordinals (bit per ordinal), or none, and how
// many are set (index::segments gives it); unknown_dead scans the bitmap instead.
struct segment_snapshot {
	static constexpr std::uint64_t unknown_dead = ~std::uint64_t(0);
	const segment* seg = nullptr;
	const strata::deleted_bits* deleted = nullptr;
	std::uint64_t dead = unknown_dead;
	// The field table of the index the segment belongs to: queries name fields, and each
	// segment resolves a name to its own index's id (D14).
	const strata::field_table* fields = nullptr;
	std::optional<std::uint32_t> field_id(std::string_view name) const { return table().find(name); }
	std::optional<std::uint32_t> key_field() const { return table().key_field(); }
	const strata::field_table& table() const {
		if (!fields) { throw std::logic_error("strata: a segment snapshot without its index's field table"); }
		return *fields;
	}
	bool any_deleted() const {
		if (!deleted || dead == 0) { return false; }
		if (dead != unknown_dead) { return true; }
		for (auto w : *deleted) {
			if (w) { return true; }
		}
		return false;
	}
};

// Skips deleted ordinals of the scorer it wraps; counting shortcuts that read document
// frequencies are off (review C3), and windows clear deleted bits (I3).
class live_scorer final : public scorer {
public:
	live_scorer(scorer_ptr inner, const strata::deleted_bits& deleted) : inner_(std::move(inner)), deleted_(&deleted) {}
	ordinal_t doc() const noexcept override { return inner_->doc(); }
	ordinal_t next() override { return skip(inner_->next()); }
	ordinal_t advance(ordinal_t t) override { return skip(inner_->advance(t)); }
	bool two_phase() const noexcept override { return inner_->two_phase(); }
	ordinal_t approx_next() override { return inner_->two_phase() ? inner_->approx_next() : next(); }
	ordinal_t approx_advance(ordinal_t t) override { return inner_->two_phase() ? inner_->approx_advance(t) : advance(t); }
	bool matches() override { return !dead(inner_->doc()) && inner_->matches(); }
	std::uint64_t cost() const noexcept override { return inner_->cost(); }
	double score() override { return inner_->score(); }
	double max_score(ordinal_t from, ordinal_t upto) override { return inner_->max_score(from, upto); }
	ordinal_t block_end(ordinal_t t) override { return inner_->block_end(t); }
	double max_score_total() const noexcept override { return inner_->max_score_total(); }
	std::size_t score_terms() const noexcept override { return inner_->score_terms(); }
	void set_min_competitive_score(double s) override { inner_->set_min_competitive_score(s); }
	double current_bound() override { return inner_->current_bound(); }
	std::unique_ptr<scorer> fresh() const override {
		auto f = inner_->fresh();
		return f ? std::make_unique<live_scorer>(std::move(f), *deleted_) : nullptr;
	}
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override {
		inner_->fill(base, end, bits);
		if (base % 64 == 0) {  // count()'s windows: whole words
			const std::size_t first = base / 64, words = std::size_t(end - base + 63) / 64;
			for (std::size_t w = 0; w < words && first + w < deleted_->size(); ++w) { bits[w] &= ~(*deleted_)[first + w]; }
			return;
		}
		for (ordinal_t d = base; d < end; ++d) {
			if (dead(d)) { bits[(d - base) >> 6] &= ~(std::uint64_t(1) << ((d - base) & 63)); }
		}
	}

private:
	bool dead(ordinal_t d) const noexcept { return d != no_more_docs && ((*deleted_)[d >> 6] >> (d & 63)) & 1; }
	ordinal_t skip(ordinal_t d) {
		while (d != no_more_docs && dead(d)) { d = inner_->next(); }
		return d;
	}
	scorer_ptr inner_;
	const strata::deleted_bits* deleted_;
};

struct doc_hit {
	std::uint64_t docid;
	double score;
	bool operator==(const doc_hit&) const = default;
};

// The snapshot's statistics: documents and total length over every segment, deleted ones
// included (D13).
inline statistics snapshot_statistics(std::span<const segment_snapshot> segs) {
	statistics st;
	for (const auto& s : segs) {
		st.doc_count += s.seg->core().doc_count();
		st.total_length += s.seg->get<inverted_plugin>().total_length();
	}
	return st;
}

// A term of the field named `field` in one segment, routed by the field (D7): a term of the key
// field is a key leaf from the keys reader, at most one ordinal (deleted copies included, which
// the live filter removes), wdf 0, no positions, so its upper bound is 0; any other term is
// the field's in the dictionary. A field the segment's index does not know has no terms.
inline std::optional<format::term_info> find_term(const segment_snapshot& s, std::string_view field, std::string_view term) {
	const auto id = s.field_id(field);
	if (!id) { return std::nullopt; }
	if (s.key_field() != id) { return s.seg->get<inverted_plugin>().find_term(*id, term); }
	const auto ord = s.seg->get<keys_plugin>().find(term, live_bits{});
	if (!ord) { return std::nullopt; }
	format::term_info t;
	t.flags = format::single_posting | format::no_wdf | format::no_positions;
	t.df = 1;
	t.postings = *ord;
	return t;
}

// A term's document frequency over every segment, deleted documents included, by field name.
inline std::uint64_t snapshot_df(std::span<const segment_snapshot> segs, std::string_view field, std::string_view term) {
	std::uint64_t df = 0;
	for (const auto& s : segs) {
		if (auto info = find_term(s, field, term)) { df += info->df; }
	}
	return df;
}

// A query's terms resolved once per segment (P6-3): one dictionary walk per term and
// segment, shared by the term's snapshot document frequency and every segment's scorer.
// A term's column fills on first use; the competitive pairs of a short list (below 128
// postings) are computed only when a scorer asks for the term in that segment. Terms are
// named by field name: each segment resolves it to its own index's id (D14).
class term_table {
public:
	explicit term_table(std::span<const segment_snapshot> segs) : segs_(segs), st_(snapshot_statistics(segs)) {}

	// The snapshot's statistics (D13).
	const statistics& stats() const noexcept { return st_; }

	// The term's document frequency over every segment, deleted documents included.
	std::uint64_t df(std::string_view field, std::string_view term) { return column(field, term).df; }

	// The term's document frequency in one of the snapshot's segments (0 when absent),
	// without computing its competitive pairs.
	std::uint64_t df(const segment& seg, std::string_view field, std::string_view term) {
		const auto& e = column(field, term).infos[index_of(seg)];
		return e ? e->df : 0;
	}

	// The term's info in one of the snapshot's segments, ready for a scorer; null when
	// the segment lacks it.
	const format::term_info* info(const segment& seg, std::string_view field, std::string_view term) {
		column_t& c = column(field, term);
		const std::size_t s = index_of(seg);
		auto& e = c.infos[s];
		if (!e) { return nullptr; }
		if (!c.complete[s]) {
			seg.get<inverted_plugin>().complete_impacts(*e);
			c.complete[s] = 1;
		}
		return &*e;
	}

private:
	struct column_t {
		std::string field, term;
		std::uint64_t df = 0;
		std::vector<std::optional<format::term_info>> infos;  // by segment
		std::vector<std::uint8_t> complete;                   // impacts computed, by segment
	};
	column_t& column(std::string_view field, std::string_view term) {
		for (auto& c : columns_) {
			if (c.term == term && c.field == field) { return c; }
		}
		column_t& c = columns_.emplace_back();
		c.field = field;
		c.term = term;
		c.infos.reserve(segs_.size());
		for (const auto& s : segs_) {
			c.infos.push_back(find_term(s, field, term));
			if (c.infos.back()) { c.df += c.infos.back()->df; }
		}
		c.complete.assign(segs_.size(), 0);
		return c;
	}
	std::size_t index_of(const segment& seg) const {
		for (std::size_t i = 0; i < segs_.size(); ++i) {
			if (segs_[i].seg == &seg) { return i; }
		}
		throw std::invalid_argument("strata: a segment outside the term table's snapshot");
	}
	std::span<const segment_snapshot> segs_;
	statistics st_;
	std::deque<column_t> columns_;  // stable addresses as columns are added
};

// Builds a query's scorer for one segment (null when it matches nothing there).
using scorer_builder = std::function<scorer_ptr(const segment&)>;

// The k best documents across segments, best first, ties by docid; with count, also the
// number of live matches. One collector spans the segments (P6-2), keyed by docid, so
// each segment prunes from the first competitive hit against the k best so far, and from
// the start above the largest threshold seed of a segment without deletes (F13).
// Everything it takes by reference must outlive the awaited task; search_pinned owns its inputs.
inline task::task<std::vector<doc_hit>> search_task(std::span<const segment_snapshot> segs, const scorer_builder& build, std::size_t k, std::uint64_t* count, slice sl) {
	if (count) { *count = 0; }
	std::vector<scorer_ptr> queries(segs.size());
	double floor = 0.0;
	for (std::size_t i = 0; i < segs.size(); ++i) {
		scorer_ptr& q = queries[i];
		if (!(q = build(*segs[i].seg))) { continue; }
		if (segs[i].any_deleted()) {
			q = std::make_unique<live_scorer>(std::move(q), *segs[i].deleted);  // a seed may count a deleted posting
		} else if (k) {
			floor = std::max(floor, q->seed(k));
		}
	}
	if (segs.size() == 1) {  // one segment: ordinal order is docid order, nothing spans segments
		const auto& s = segs[0];
		scorer_ptr& q = queries[0];
		if (!q) { co_return std::vector<doc_hit>{}; }
		std::vector<doc_hit> out;
		for (const auto& h : co_await top_k_task(*s.seg, *q, k, count, floor, sl)) { out.push_back({s.seg->core().docid(h.ord), h.score}); }
		co_return out;
	}
	std::size_t docs = 0;
	for (const auto& s : segs) { docs += s.seg->core().doc_count(); }
	top_collector c(k, false, docs);
	c.expect_segments(segs.size());
	if (floor > 0) { c.seed(floor); }
	for (std::size_t i = 0; i < segs.size(); ++i) {
		co_await sl.checkpoint();
		const auto& s = segs[i];
		scorer_ptr& q = queries[i];
		if (!q) { continue; }
		// A count known from statistics leaves the collection free to prune.
		c.counting = false;
		if (count) {
			if (auto n = co_await q->exact_count(sl)) {
				*count += *n;
			} else {
				c.counting = true;
			}
		}
		if (!k) {
			if (c.counting) { *count += co_await count_task(*q, sl); }
			continue;
		}
		c.begin(*s.seg);
		double min = c.counting ? 0.0 : c.min_competitive();
		if (min > 0) { q->set_min_competitive_score(min); }
		if (co_await q->collect_top_k(c, sl)) {
			if (c.counting) { *count += c.count; }
			continue;
		}
		std::uint64_t n = 0;
		window_gate gate;
		for (ordinal_t d = q->next(); d != no_more_docs; d = q->next()) {
			if (gate.due(d)) { co_await sl.checkpoint(); }
			++n;
			if (c.counting && c.min_competitive() > 0 && q->current_bound() <= c.min_competitive()) { continue; }  // counted, cannot compete
			c.collect(d, q->score());
			if (!c.counting && c.min_competitive() != min) { q->set_min_competitive_score(min = c.min_competitive()); }
		}
		if (c.counting) { *count += n; }
	}
	co_return c.finish_docs<doc_hit>();
}

// search_task run to completion on the calling thread; the caller's view outlives the call.
inline std::vector<doc_hit> search(std::span<const segment_snapshot> segs, const scorer_builder& build, std::size_t k, std::uint64_t* count = nullptr) {
	return run_unsliced(search_task(segs, build, k, count, slice{}));
}

// The number of live matches across segments.
inline task::task<std::uint64_t> search_count_task(std::span<const segment_snapshot> segs, const scorer_builder& build, slice sl) {
	std::uint64_t n = 0;
	for (const auto& s : segs) {
		co_await sl.checkpoint();
		scorer_ptr q = build(*s.seg);
		if (!q) { continue; }
		if (s.any_deleted()) { q = std::make_unique<live_scorer>(std::move(q), *s.deleted); }
		n += co_await count_task(*q, sl);
	}
	co_return n;
}

inline std::uint64_t search_count(std::span<const segment_snapshot> segs, const scorer_builder& build) {
	return run_unsliced(search_count_task(segs, build, slice{}));
}

}  // namespace prospect
