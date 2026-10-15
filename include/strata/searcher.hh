// Search across an index's segments (Phase 3 design, "Statistics and ranking"): weights
// read the snapshot's statistics, deleted documents are skipped, each segment prunes
// against the k-th score found so far, and hits merge by (score descending, docid
// ascending).
#pragma once

#include "strata/search.hh"

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

namespace strata {

// A segment as a search sees it: its deleted ordinals (bit per ordinal), or none, and how
// many are set (index::segments gives it); unknown_dead scans the bitmap instead.
struct segment_snapshot {
	static constexpr std::uint64_t unknown_dead = ~std::uint64_t(0);
	const segment* seg = nullptr;
	const std::vector<std::uint64_t>* deleted = nullptr;
	std::uint64_t dead = unknown_dead;
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
	live_scorer(scorer_ptr inner, const std::vector<std::uint64_t>& deleted) : inner_(std::move(inner)), deleted_(&deleted) {}
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
	const std::vector<std::uint64_t>* deleted_;
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
		st.doc_count += s.seg->doc_count();
		st.total_length += s.seg->total_length();
	}
	return st;
}

// A term's document frequency over every segment, deleted documents included.
inline std::uint64_t snapshot_df(std::span<const segment_snapshot> segs, std::string_view term) {
	std::uint64_t df = 0;
	for (const auto& s : segs) {
		if (auto info = s.seg->find_term(term)) { df += info->df; }
	}
	return df;
}

// A query's terms resolved once per segment (P6-3): one dictionary walk per term and
// segment, shared by the term's snapshot document frequency and every segment's scorer.
// A term's column fills on first use; the competitive pairs of a short list (below 128
// postings) are computed only when a scorer asks for the term in that segment.
class term_table {
public:
	explicit term_table(std::span<const segment_snapshot> segs) : segs_(segs), st_(snapshot_statistics(segs)) {}

	// The snapshot's statistics (D13).
	const statistics& stats() const noexcept { return st_; }

	// The term's document frequency over every segment, deleted documents included.
	std::uint64_t df(std::string_view term) { return column(term).df; }

	// The term's document frequency in one of the snapshot's segments (0 when absent),
	// without computing its competitive pairs.
	std::uint64_t df(const segment& seg, std::string_view term) {
		const auto& e = column(term).infos[index_of(seg)];
		return e ? e->df : 0;
	}

	// The term's info in one of the snapshot's segments, ready for a scorer; null when
	// the segment lacks it.
	const format::term_info* info(const segment& seg, std::string_view term) {
		column_t& c = column(term);
		const std::size_t s = index_of(seg);
		auto& e = c.infos[s];
		if (!e) { return nullptr; }
		if (!c.complete[s]) {
			seg.complete_impacts(*e);
			c.complete[s] = 1;
		}
		return &*e;
	}

private:
	struct column_t {
		std::string term;
		std::uint64_t df = 0;
		std::vector<std::optional<format::term_info>> infos;  // by segment
		std::vector<std::uint8_t> complete;                   // impacts computed, by segment
	};
	column_t& column(std::string_view term) {
		for (auto& c : columns_) {
			if (c.term == term) { return c; }
		}
		column_t& c = columns_.emplace_back();
		c.term = term;
		c.infos.reserve(segs_.size());
		for (const auto& s : segs_) {
			c.infos.push_back(s.seg->find_term(term));
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
// number of live matches. Each segment after the first prunes below the k-th score found
// so far, made strictly lower, so a document tying it with a smaller docid still enters;
// a segment returns only hits above that floor.
inline std::vector<doc_hit> search(std::span<const segment_snapshot> segs, const scorer_builder& build, std::size_t k, std::uint64_t* count = nullptr) {
	std::vector<doc_hit> best, seg_hits, merged;
	auto better = [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; };
	if (count) { *count = 0; }
	for (const auto& s : segs) {
		scorer_ptr q = build(*s.seg);
		if (!q) { continue; }
		if (s.any_deleted()) { q = std::make_unique<live_scorer>(std::move(q), *s.deleted); }
		const double floor = best.size() == k && k ? std::nextafter(best.back().score, -std::numeric_limits<double>::infinity()) : 0.0;
		std::uint64_t n = 0;
		const auto hits = top_k(*q, k, count ? &n : nullptr, floor);
		if (count) { *count += n; }
		if (hits.empty()) { continue; }
		// Hits leave the segment as (score, docid). They arrive sorted by (score, ordinal),
		// which is docid order unless the segment's ordinals are permuted; then ties re-sort.
		seg_hits.clear();
		for (const auto& h : hits) { seg_hits.push_back({s.seg->docid(h.ord), h.score}); }
		if (!std::is_sorted(seg_hits.begin(), seg_hits.end(), better)) { std::sort(seg_hits.begin(), seg_hits.end(), better); }
		// Both runs are sorted by (score descending, docid ascending): one linear merge, cut at k.
		merged.clear();
		std::size_t i = 0, j = 0;
		while (merged.size() < k && (i < best.size() || j < seg_hits.size())) {
			if (j < seg_hits.size() && (i == best.size() || better(seg_hits[j], best[i]))) {
				merged.push_back(seg_hits[j++]);
			} else {
				merged.push_back(best[i++]);
			}
		}
		best.swap(merged);
	}
	return best;
}

// The number of live matches across segments.
inline std::uint64_t search_count(std::span<const segment_snapshot> segs, const scorer_builder& build) {
	std::uint64_t n = 0;
	for (const auto& s : segs) {
		scorer_ptr q = build(*s.seg);
		if (!q) { continue; }
		if (s.any_deleted()) { q = std::make_unique<live_scorer>(std::move(q), *s.deleted); }
		n += count(*q);
	}
	return n;
}

}  // namespace strata
