// Search across an index's segments (Phase 3 design, "Statistics and ranking"): weights
// read the snapshot's statistics, deleted documents are skipped, each segment prunes
// against the k-th score found so far, and hits merge by (score descending, docid
// ascending).
#pragma once

#include "strata/search.hh"

#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace strata {

// A segment as a search sees it: its deleted ordinals (bit per ordinal), or none.
struct segment_snapshot {
	const segment* seg = nullptr;
	const std::vector<std::uint64_t>* deleted = nullptr;
	bool any_deleted() const {
		if (!deleted) { return false; }
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
		if (auto info = s.seg->term(term)) { df += info->df; }
	}
	return df;
}

// Builds a query's scorer for one segment (null when it matches nothing there).
using scorer_builder = std::function<scorer_ptr(const segment&)>;

// The k best documents across segments, best first, ties by docid; with count, also the
// number of live matches. Each segment after the first prunes below the k-th score found
// so far, made strictly lower, so a document tying it with a smaller docid still enters.
inline std::vector<doc_hit> search(std::span<const segment_snapshot> segs, const scorer_builder& build, std::size_t k, std::uint64_t* count = nullptr) {
	std::vector<doc_hit> best;
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
		for (const auto& h : hits) { best.push_back({s.seg->docid(h.ord), h.score}); }
		std::sort(best.begin(), best.end(), better);
		if (best.size() > k) { best.resize(k); }
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
