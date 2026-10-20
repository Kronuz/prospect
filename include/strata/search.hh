/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Query evaluation over one segment: weighting, scorers, and collectors.

#pragma once

#include "strata/reader.hh"

#include <algorithm>
#include <array>
#include <optional>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace strata {

// --- weighting ----------------------------------------------------------------------------------

// Two presets of one weighting, both of the BM25 family (ARCHITECTURE, "One
// implementation per mechanism").
enum class scheme { bm25, bm25_xapian };

// The collection statistics weights read: one segment's, or a snapshot's across segments
// (deleted documents included until a merge drops them, as document frequencies are).
struct statistics {
	std::uint64_t doc_count = 0, total_length = 0;
	statistics() = default;
	statistics(std::uint64_t docs, std::uint64_t length) : doc_count(docs), total_length(length) {}
	statistics(const inverted_plugin::reader& seg) : doc_count(seg.doc_count()), total_length(seg.total_length()) {}  // NOLINT: implicit by design
	double average_length() const noexcept { return doc_count ? double(total_length) / double(doc_count) : 0.0; }
};

// A term's weighting function: score(tf, length) for one scheme.
//   bm25, Lucene's and Tantivy's form (k1 = 1.2, b = 0.75) over one-byte quantized
//     lengths: idf · tf / (tf + k1 · (1 − b + b · len / avglen)),
//     idf = ln(1 + (N − df + 0.5) / (df + 0.5)).
//   bm25_xapian, Xapian's BM25Weight (k1 = 1, k2 = 0, k3 = 1, b = 0.5, min_normlen = 0.5)
//     over exact lengths:
//     idf · (k3 + 1) · wqf / (k3 + wqf) · (k1 + 1) · tf / (K + tf),
//     K = k1 · ((1 − b) + b · max(len / avglen, min_normlen)), idf from (N − df + 0.5) / (df + 0.5).
struct weight {
	scheme kind = scheme::bm25;
	double k1 = 1.2, b = 0.75, min_normlen = 0;
	double idf = 0, avg = 1, inv_avg = 1;  // inv_avg = 1 / avg: one division per score
	// Lucene BM25 scores quantized lengths, as Lucene does: norm_k[code] is k1 * (1 - b + b *
	// decode_length(code) / avg), so a score is one table load and one division.
	std::array<double, 256> norm_k{};

	static weight bm25(const statistics& st, std::uint64_t df, double boost = 1.0) {
		weight w;
		w.idf = boost * std::log(1.0 + (double(st.doc_count) - double(df) + 0.5) / (double(df) + 0.5));
		w.avg = std::max(1.0, st.average_length());
		w.inv_avg = 1.0 / w.avg;
		for (unsigned c = 0; c < 256; ++c) { w.norm_k[c] = w.k1 * (1.0 - w.b + w.b * double(format::decode_length(std::uint8_t(c))) * w.inv_avg); }
		return w;
	}

	static weight bm25_xapian(const statistics& st, std::uint64_t df, std::uint32_t wqf = 1) {
		weight w;
		w.kind = scheme::bm25_xapian;
		w.k1 = 1.0;
		w.b = 0.5;
		w.min_normlen = 0.5;
		double tw = (double(st.doc_count) - double(df) + 0.5) / (double(df) + 0.5);
		if (tw < 2) { tw = tw * 0.5 + 1; }  // Xapian keeps small idf positive
		constexpr double k3 = 1.0;
		w.idf = std::log(tw) * (k3 + 1) * wqf / (k3 + wqf) * (w.k1 + 1);
		w.avg = st.average_length() > 0 ? st.average_length() : 1.0;
		w.inv_avg = 1.0 / w.avg;
		return w;
	}

	// Lucene BM25 by quantized length (segment::norm).
	double by_norm(std::uint32_t tf, std::uint8_t norm) const noexcept {
		return tf ? idf * double(tf) / (double(tf) + norm_k[norm]) : 0.0;
	}

	double operator()(std::uint32_t tf, std::uint32_t length) const noexcept {
		if (tf == 0) { return 0.0; }
		if (kind == scheme::bm25) { return by_norm(tf, format::encode_length(length)); }
		const double normlen = std::max(double(length) * inv_avg, min_normlen);
		const double K = k1 * ((1.0 - b) + b * normlen);
		return idf * double(tf) / (K + double(tf));
	}
};

// --- scorers -----------------------------------------------------------------------------------

struct top_collector;

// An upper bound on the exact sum of n non-negative terms whose floating-point sum is sum:
// summing in another order (as scoring does) can round up to n units in the last place
// further (Lucene's MathUtil.sumUpperBound).
inline double sum_upper(double sum, std::size_t n) noexcept { return sum * (1.0 + double(2 * n) * 0x1p-53); }

// The virtual scorer interface; every implementation satisfies strata::scorer.
struct scorer {
	virtual ~scorer() = default;
	virtual ordinal_t doc() const noexcept = 0;
	virtual ordinal_t next() = 0;
	virtual ordinal_t advance(ordinal_t target) = 0;
	virtual std::uint64_t cost() const noexcept = 0;
	virtual double score() = 0;
	// An upper bound on score() for documents in [from, upto], from skip data alone; the
	// scorer's position is unchanged.
	virtual double max_score(ordinal_t from, ordinal_t upto) = 0;
	// The last ordinal of the block holding target, or no_more_docs.
	virtual ordinal_t block_end(ordinal_t target) = 0;
	// An upper bound on every score this scorer returns.
	virtual double max_score_total() const noexcept = 0;
	// Only documents scoring above `s` are wanted from now on; scorers may skip the rest.
	virtual void set_min_competitive_score(double s) { (void)s; }
	// An upper bound on score() for the current document, cheaper than score().
	virtual double current_bound() { return max_score_total(); }
	// The number of matching documents when known without iterating (a term's df).
	virtual std::optional<std::uint64_t> exact_count() { return std::nullopt; }
	// Whether every document in the segment matches.
	virtual bool matches_all() const noexcept { return false; }
	// An unpositioned copy for an independent pass (counting), or null when not supported.
	virtual std::unique_ptr<scorer> fresh() const { return nullptr; }
	// Sets bit (d - base) in bits for every match d in [doc(), end), then stops at the
	// first match >= end. The window is at most 4,096 ordinals (bits holds 64 words) and
	// base <= doc().
	virtual void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) {
		for (ordinal_t d = doc(); d < end; d = next()) { bits[(d - base) >> 6] |= std::uint64_t(1) << ((d - base) & 63); }
	}
	// Like fill, also adding each match's score to acc[d - base].
	virtual void score_window(ordinal_t base, ordinal_t end, double* acc, std::uint64_t* bits) {
		for (ordinal_t d = doc(); d < end; d = next()) {
			acc[d - base] += score();
			bits[(d - base) >> 6] |= std::uint64_t(1) << ((d - base) & 63);
		}
	}
	// Two-phase iteration: approx_next/approx_advance move over a superset of the matches
	// (a phrase's documents holding all its terms) and matches() confirms the current one.
	// Parents leapfrog on approximations and confirm only documents every child agrees on.
	virtual bool two_phase() const noexcept { return false; }
	virtual ordinal_t approx_next() { return next(); }
	virtual ordinal_t approx_advance(ordinal_t target) { return advance(target); }
	virtual bool matches() { return true; }
	// Collects the k best hits itself (bulk scoring); false when the generic loop must.
	virtual bool collect_top_k(top_collector& c) { (void)c; return false; }
	// A strict lower bound on the k-th best score among this segment's matches, from stored
	// threshold seeds (F13); 0 for none. Valid only where every match is live.
	virtual double seed(std::size_t k) { (void)k; return 0.0; }
};

struct hit {
	ordinal_t ord;
	double score;
};

// The threshold one segment's collection hands down once the k-th score is known: the score
// itself, so blocks bounded at it are skipped (a later tie has a larger ordinal, hence a larger
// docid, and never enters). Built with STRATA_EXPERIMENT_NO_EQUAL_SKIP, the score just below
// it, as a reordered segment needs (reordering review, P1-R9): the experiment that measures it.
inline double equal_bound(double kth) noexcept {
#ifdef STRATA_EXPERIMENT_NO_EQUAL_SKIP
	return std::nextafter(kth, -std::numeric_limits<double>::infinity());
#else
	return kth;
#endif
}

// The best k hits, as Tantivy's TopNComputer keeps them: a buffer of up to 2k hits that,
// when full, keeps its k best (nth_element) and raises the threshold to the k-th best
// score; amortized constant time per hit. One collector may span segments (P6-2,
// begin()): hits keep their segment and ordinal, and ties order by docid, read only when
// two scores tie (within a docid-ordered segment, ordinal order is docid order). A
// reordered segment always goes through begin() (reordering.md D5).
struct top_collector {
	explicit top_collector(std::size_t k, bool counting) : k(k), counting(counting) { hits.reserve(2 * k + 1); }
	// Scorers may skip whatever scores at or below this (0: nothing yet).
	double min_competitive() const noexcept { return min_; }
	// A threshold known before collection starts (another segment's k-th score, made
	// strictly lower so equal scores still enter); no hit at or below it enters.
	void seed(double floor) noexcept { min_ = floor_ = floor; }
	// Whether the current segment is reordered: its hits may arrive in any ordinal order, ties
	// comparing docids.
	bool reordered() const noexcept { return cur_ != none && !segs_[cur_]->core().docid_ordered(); }
	// Room for the segments begin() will name, so collection allocates once.
	void expect_segments(std::size_t n) { segs_.reserve(n); }
	// The following hits are seg's; hits already kept stay (P6-2).
	void begin(const segment& seg) {
		segs_.push_back(&seg);
		cur_ = std::uint32_t(segs_.size() - 1);
		count = 0;
		update_min();
	}
	void collect(ordinal_t d, double score) {
		if (!k) { return; }
		if (cur_ != none) {
			// Review R2: a tie with the k-th enters only with a smaller docid.
			if (floor_ > 0 && score <= floor_) { return; }
			if (full_ && (score < kth_.score || (score == kth_.score && !before_kth(d)))) { return; }
		} else if ((full_ || floor_ > 0) && score <= min_) {
			return;
		}
		hits.push_back({score, d, cur_, 0});
		if (hits.size() >= 2 * k) { truncate(); }
	}
	// The k best of one segment, as ordinals, best first.
	std::vector<hit> finish() {
		if (hits.size() > k) { truncate(); }
		std::sort(hits.begin(), hits.end(), better());
		std::vector<hit> out;
		out.reserve(hits.size());
		for (const auto& e : hits) { out.push_back({e.ord, e.score}); }
		return out;
	}
	// The k best by docid, best first, after begin().
	template <typename DocHit>
	std::vector<DocHit> finish_docs() {
		if (hits.size() > k) { truncate(); }
		std::sort(hits.begin(), hits.end(), better());
		std::vector<DocHit> out;
		out.reserve(hits.size());
		for (const auto& e : hits) { out.push_back({docid(e), e.score}); }
		return out;
	}
	std::size_t k;
	bool counting;           // every match must be counted: no skipping by score
	std::uint64_t count = 0;

private:
	static constexpr std::uint32_t none = ~std::uint32_t(0);
	struct entry {
		double score;
		ordinal_t ord;
		std::uint32_t seg;                // index into segs_, or none
		mutable std::uint64_t docid = 0;  // read once, when a tie first needs it (0: not yet)
	};
	std::uint64_t docid(const entry& e) const {
		if (e.seg == none) { return e.ord; }
		if (!e.docid) { e.docid = segs_[e.seg]->core().docid(e.ord); }
		return e.docid;
	}
	// Best first: by score, then by docid.
	struct order {
		const top_collector* c;
		bool operator()(const entry& a, const entry& b) const {
			if (a.score != b.score) { return a.score > b.score; }
			if (a.seg == b.seg && (a.seg == none || c->segs_[a.seg]->core().docid_ordered())) { return a.ord < b.ord; }
			const auto *sa = c->segs_[a.seg], *sb = c->segs_[b.seg];
			if (sa->core().max_docid() < sb->core().min_docid()) { return true; }  // disjoint ranges: no docid read
			if (sb->core().max_docid() < sa->core().min_docid()) { return false; }
			return c->docid(a) < c->docid(b);
		}
	};
	order better() const { return {this}; }
	// Whether ordinal d of the current segment precedes the k-th hit by docid.
	bool before_kth(ordinal_t d) const {
		if (kth_.seg == cur_ && segs_[cur_]->core().docid_ordered()) { return d < kth_.ord; }
		const segment* s = segs_[cur_];
		if (s->core().max_docid() < kth_docid_) { return true; }
		if (s->core().min_docid() > kth_docid_) { return false; }
		return s->core().docid(d) < kth_docid_;
	}
	void truncate() {
		std::nth_element(hits.begin(), hits.begin() + std::ptrdiff_t(k - 1), hits.end(), better());
		hits.resize(k);
		kth_ = hits[k - 1];
		if (kth_.seg != none) { kth_docid_ = docid(kth_); }
		full_ = true;
		update_min();
	}
	// One segment: later hits have larger ordinals, so a tie never enters. Across segments
	// or in a reordered one: only when every docid of the current segment exceeds the
	// k-th's (review R2).
	void update_min() noexcept {
		if (!full_) { return; }
		if (cur_ == none) {
			min_ = std::max(floor_, equal_bound(kth_.score));
		} else {
			min_ = std::max(floor_, segs_[cur_]->core().min_docid() > kth_docid_ ? kth_.score : std::nextafter(kth_.score, -std::numeric_limits<double>::infinity()));
		}
	}
	std::vector<entry> hits;
	std::vector<const segment*> segs_;
	entry kth_{0, 0, none, 0};
	std::uint64_t kth_docid_ = 0;
	std::uint32_t cur_ = none;
	double min_ = 0, floor_ = 0;
	bool full_ = false;
};

using scorer_ptr = std::unique_ptr<scorer>;

class term_scorer final : public scorer {
public:
	term_scorer(const inverted_plugin::reader& seg, const format::term_info& info, double boost = 1.0)
		: term_scorer(seg, info, weight::bm25(seg, info.df, boost)) {}

	term_scorer(const inverted_plugin::reader& seg, const format::term_info& info, const weight& w) : seg_(&seg), it_(seg, info), weight_(w) {
		// Whole-list bound from the term's competitive pairs.
		for (const auto& p : info.impacts) { total_bound_ = std::max(total_bound_, weight_(p.wdf, format::decode_length(p.length))); }
	}

	ordinal_t doc() const noexcept override { return it_.doc(); }
	ordinal_t next() override { return skip_blocks(it_.next()); }
	ordinal_t advance(ordinal_t t) override { return skip_blocks(it_.advance(t)); }
	ordinal_t approx_next() override { return skip_blocks(it_.next()); }
	ordinal_t approx_advance(ordinal_t t) override { return skip_blocks(it_.advance(t)); }
	std::uint64_t cost() const noexcept override { return it_.cost(); }
	double score() override { return score_of(it_.doc(), it_.wdf()); }
	void set_min_competitive_score(double s) override { min_ = s; }
	ordinal_t block_end(ordinal_t t) override { return it_.block_end(t); }
	double max_score_total() const noexcept override { return total_bound_; }
	std::optional<std::uint64_t> exact_count() override { return it_.cost(); }  // one segment, no deletes
	bool matches_all() const noexcept override { return it_.cost() == seg_->doc_count(); }
	std::unique_ptr<scorer> fresh() const override { return std::make_unique<term_scorer>(*seg_, it_.info(), weight_); }

	// Bulk top-k: skips groups and blocks that cannot compete, then scores each remaining
	// block in one loop over its decoded postings. Counting leaves this to the generic loop.
	bool collect_top_k(top_collector& c) override {
		if (c.counting) { return false; }
		if (c.reordered() && it_.groups() > 1) { return collect_groups(c); }
		for (ordinal_t d = it_.next(); d != no_more_docs; d = it_.doc()) {
			min_ = c.min_competitive();
			if ((d = skip_blocks(d)) == no_more_docs) { break; }
			const ordinal_t last = it_.block_end(d);
			it_.for_each_until(last == no_more_docs ? no_more_docs : last + 1,
				[&](ordinal_t ord, std::uint32_t wdf) { c.collect(ord, score_of(ord, wdf)); });
		}
		return true;
	}
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override { it_.fill(base, end, bits); }

	// M3t, in a reordered segment, where a reordering gathers high bounds into few groups:
	// the tail, then the groups by descending bound, each from a fresh iterator with the
	// per-block skip inside, until a group's bound cannot beat the threshold.
	bool collect_groups(top_collector& c) {
		auto score_until = [&](postings_iterator& p, ordinal_t end) { p.for_each_until(end, [&](ordinal_t ord, std::uint32_t wdf) { c.collect(ord, score_of(ord, wdf)); }); };
		const std::size_t groups = it_.groups();
		if (it_.info().df % format::block && total_bound_ > c.min_competitive()) {
			postings_iterator p(*seg_, it_.info());
			if (p.advance(it_.group_last(groups - 1) + 1) != no_more_docs) { score_until(p, no_more_docs); }
		}
		order_.resize(groups);
		for (std::size_t g = 0; g < groups; ++g) { order_[g] = {group_bound(g), g}; }
		std::sort(order_.begin(), order_.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		for (const auto& [bound, g] : order_) {
			if (bound <= c.min_competitive()) { break; }
			postings_iterator p(*seg_, it_.info());
			const ordinal_t last = it_.group_last(g);
			for (ordinal_t d = p.advance(it_.group_first(g)); d != no_more_docs && d <= last; d = p.doc()) {
				const std::size_t b = p.block_index_for(d);
				const ordinal_t block_last = p.block_last(b);
				if (block_bound(b) <= c.min_competitive()) {
					if (p.advance(block_last + 1) == no_more_docs) { break; }
					continue;
				}
				score_until(p, block_last + 1);
			}
		}
		return true;
	}

	// The bound of the smallest skip entry covering [from, upto]: its block, its group,
	// or the whole term (Lucene's MaxScoreCache); one memoized evaluation per call.
	double max_score(ordinal_t from, ordinal_t upto) override {
		const std::size_t b = it_.block_index_for(from);
		if (b >= it_.blocks()) { return total_bound_; }  // the tail
		if (it_.block_last(b) >= upto) { return block_bound(b); }
		const std::size_t g = b / 32;
		if (it_.group_last(g) >= upto) { return group_bound(g); }
		return total_bound_;
	}

	// Adds each posting's score in [doc(), end) to acc[d - base] and sets its bit, then
	// stops at the first posting >= end.
	void score_window(ordinal_t base, ordinal_t end, double* acc, std::uint64_t* bits) override {
		it_.for_each_until(end, [&](ordinal_t d, std::uint32_t wdf) {
			const ordinal_t i = d - base;
			acc[i] += score_of(d, wdf);
			bits[i >> 6] |= std::uint64_t(1) << (i & 63);
		});
	}

	postings_iterator& postings() noexcept { return it_; }

	// The larger of the stored pairs' scores under this weight, made strictly lower by more
	// than rounding can move a score (F13).
	double seed(std::size_t k) override {
		const format::seed_pairs* p = it_.seeds(k);
		if (!p) { return 0.0; }
		const bool lucene = weight_.kind == scheme::bm25;
		const double by_code = lucene ? weight_.by_norm(p->code_wdf, p->code) : weight_(p->code_wdf, format::max_length_of(p->code));
		const double exact = lucene ? weight_.by_norm(p->exact_wdf, format::encode_length(p->exact)) : weight_(p->exact_wdf, p->exact);
		return std::max(by_code, exact) * (1.0 - 0x1p-40);
	}

	// Lucene BM25 reads the one-byte norm; Xapian BM25 reads the exact length.
	double score_of(ordinal_t ord, std::uint32_t wdf) const {
		return weight_.kind == scheme::bm25 ? weight_.by_norm(wdf, seg_->norm(ord)) : weight_(wdf, seg_->length(ord));
	}
	const inverted_plugin::reader& seg() const noexcept { return *seg_; }

private:
	double bound(std::span<const format::impact> pairs) const {
		double best = 0;
		for (const auto& p : pairs) { best = std::max(best, weight_(p.wdf, format::decode_length(p.length))); }
		return best;
	}
	// Block bounds are cached for one group of 32 blocks at a time.
	double block_bound(std::size_t b) {
		if (b >= it_.blocks()) { return total_bound_; }
		if (b / 32 != cached_group_) {
			cached_group_ = b / 32;
			std::fill(std::begin(block_bounds_), std::end(block_bounds_), -1.0);
		}
		double& v = block_bounds_[b % 32];
		if (v < 0) { v = bound(it_.block_impacts_at(b)); }
		return v;
	}
	double group_bound(std::size_t g) {
		if (group_bounds_.empty()) { group_bounds_.assign(it_.groups(), -1.0); }
		double& b = group_bounds_[g];
		if (b < 0) { b = bound(it_.group_impacts(g)); }
		return b;
	}

	// Moves past groups, then blocks, whose bound cannot beat the minimum competitive score.
	ordinal_t skip_blocks(ordinal_t d) {
		while (min_ > 0 && d != no_more_docs) {
			if (checked_until_ != no_more_docs && d <= checked_until_) { return d; }
			std::size_t g = it_.groups() ? it_.group_of(d) : 0;
			if (g < it_.groups() && group_bound(g) <= min_) {
				// Find the next group that can compete before decoding any blocks.
				while (g < it_.groups() && group_bound(g) <= min_) { ++g; }
				if (g == it_.groups() && total_bound_ <= min_) { return no_more_docs; }
				const ordinal_t to = g < it_.groups() ? it_.group_first(g) : it_.group_last(g - 1) + 1;
				if (to <= d) { corrupt("skip group order"); }  // else the loop never ends
				d = it_.advance(to);
				continue;
			}
			const std::size_t b = it_.block_index_for(d);
			if (b >= it_.blocks()) {
				if (total_bound_ <= min_) { return no_more_docs; }
				checked_until_ = no_more_docs - 1;
				return d;
			}
			if (!it_.any_block_impact(b, [&](const format::impact& p) { return weight_(p.wdf, format::decode_length(p.length)) > min_; })) {
				d = it_.advance(it_.block_last(b) + 1);
				continue;
			}
			checked_until_ = it_.block_last(b);
			return d;
		}
		return d;
	}

	const inverted_plugin::reader* seg_;
	postings_iterator it_;
	weight weight_;
	double total_bound_ = 0, min_ = 0;
	ordinal_t checked_until_ = no_more_docs;
	std::vector<double> group_bounds_;
	std::vector<std::pair<double, std::size_t>> order_;  // collect_groups' visiting order
	std::size_t cached_group_ = std::size_t(-1);
	double block_bounds_[32];

};

// Documents every child matches; score = sum.
class conjunction_scorer final : public scorer {
public:
	// Children iterate cheapest first but their scores sum in the given order, so a
	// document scores the same in every segment whatever the terms' frequencies there.
	explicit conjunction_scorer(std::vector<scorer_ptr> children)
		: started_(children.size(), false), window_bounds_(children.size(), 0.0), rank_(children.size()), parts_(children.size()) {
		std::vector<std::size_t> by_cost(children.size());
		for (std::size_t i = 0; i < by_cost.size(); ++i) { by_cost[i] = i; }
		std::stable_sort(by_cost.begin(), by_cost.end(), [&](std::size_t a, std::size_t b) { return children[a]->cost() < children[b]->cost(); });
		for (std::size_t i = 0; i < by_cost.size(); ++i) {
			children_.push_back(std::move(children[by_cost[i]]));
			rank_[i] = by_cost[i];
		}
		for (const auto& c : children_) {
			bound_ += c->max_score_total();
			two_phase_ = two_phase_ || c->two_phase();
			auto* t = dynamic_cast<term_scorer*>(c.get());
			if (t) { terms_.push_back(t); }
		}
		if (terms_.size() != children_.size()) { terms_.clear(); }
	}

	// Whether ANDing window bits costs less than leapfrogging: filling costs about 0.05 ns
	// per posting in a term dense enough for bitset blocks (over a quarter of the
	// documents) and 1.5 ns otherwise; leapfrogging costs about 40 ns per lead posting
	// and other term (measured on the gate VM).
	bool prefer_windows() const {
		const double n = double(terms_[0]->seg().doc_count());
		double fill = 0;
		for (const auto* t : terms_) { fill += double(t->cost()) * (double(t->cost()) * 4 >= n ? 0.05 : 1.5); }
		return fill < double(terms_[0]->cost()) * double(terms_.size() - 1) * 40.0;
	}

	// Counting fast paths for conjunctions of terms: ANDing each term's window bits, or
	// leapfrogging on the postings directly, whichever prefer_windows() estimates cheaper.
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override {
		if (terms_.empty() || min_ > 0) {
			scorer::fill(base, end, bits);
			return;
		}
		const std::size_t words = std::size_t(end - base + 63) / 64;
		if (prefer_windows()) {
			std::uint64_t all[64], one[64];
			const double share = double(end - base) / double(terms_[0]->seg().doc_count());
			for (std::size_t i = 0; i < terms_.size(); ++i) {
				auto& it = terms_[i]->postings();
				if (it.doc() < base) { it.advance(base); }
				if (i) {
					// Few candidates left (counted only up to where probing stops paying): test
					// each by advancing to it, which a bitset block answers from its words,
					// instead of filling the term's whole window.
					const double limit = double(terms_[i]->cost()) * share / 128;
					std::size_t left = 0;
					for (std::size_t w = 0; limit >= 1 && w < words && double(left) < limit; ++w) { left += std::size_t(std::popcount(all[w])); }
					if (limit >= 1 && double(left) < limit) {
						if (left == 0) { break; }
						for (std::size_t w = 0; w < words; ++w) {
							for (std::uint64_t word = all[w]; word; word &= word - 1) {
								const ordinal_t d = base + ordinal_t(w * 64 + std::size_t(std::countr_zero(word)));
								if (it.advance(d) != d) { all[w] &= ~(std::uint64_t(1) << std::countr_zero(word)); }
							}
						}
						continue;
					}
				}
				std::uint64_t* into = i ? one : all;
				std::fill(into, into + words, 0);
				if (it.doc() < end) { it.fill(base, end, into); }
				if (i) {
					for (std::size_t w = 0; w < words; ++w) { all[w] &= one[w]; }
				}
			}
			for (std::size_t w = 0; w < words; ++w) { bits[w] |= all[w]; }
			doc_ = align(terms_[0]->postings().doc());
			return;
		}
		auto& lead = terms_[0]->postings();
		ordinal_t d = doc_;
		while (d < end) {
			bits[(d - base) >> 6] |= std::uint64_t(1) << ((d - base) & 63);
			d = lead.next();
			for (std::size_t i = 1; i < terms_.size() && d != no_more_docs;) {
				const ordinal_t e = terms_[i]->postings().advance(d);
				if (e == d) {
					++i;
				} else {
					d = lead.advance(e);
					i = 1;
				}
			}
		}
		doc_ = d;
	}
	// Bulk top-k for a conjunction of terms, on the postings without virtual calls: the
	// lead's blocks are windows, skipped whole when the summed bounds cannot compete;
	// inside one, a document whose lead score plus the other terms' window bounds cannot
	// compete is skipped before they move. Scores sum in score()'s order. Counting leaves
	// this to the generic loop.
	bool collect_top_k(top_collector& c) override {
		if (c.counting || terms_.empty()) { return false; }
		const std::size_t n = terms_.size();
		auto& lead = terms_[0]->postings();
		for (ordinal_t d = lead.next(); d != no_more_docs;) {
			const ordinal_t end = lead.block_end(d);
			const ordinal_t upto = end == no_more_docs ? no_more_docs - 1 : end;
			// The window's bounds, computed once a threshold exists: at the window's start,
			// or where one first appears inside it (bounding the rest of the window).
			double rest = 0;
			bool bounded = false;
			auto bound = [&](ordinal_t from) {
				bounded = true;
				rest = 0;
				for (std::size_t i = 1; i < n; ++i) { rest += terms_[i]->max_score(from, upto); }
				return terms_[0]->max_score(from, upto) + rest;
			};
			if (const double min = c.min_competitive(); min > 0 && sum_upper(bound(d), n) <= min) {
				if (end == no_more_docs) { break; }
				d = lead.advance(end + 1);
				continue;
			}
			while (d <= upto) {
				const double s = parts_[rank_[0]] = terms_[0]->score_of(d, lead.wdf());
				if (const double min = c.min_competitive(); min > 0) {
					if (!bounded) { bound(d); }
					if (sum_upper(s + rest, n) <= min) {
						d = lead.next();
						continue;
					}
				}
				bool agreed = true;
				for (std::size_t i = 1; i < n; ++i) {
					auto& it = terms_[i]->postings();
					const ordinal_t e = it.advance(d);
					if (e != d) {
						d = lead.advance(e);
						agreed = false;
						break;
					}
					parts_[rank_[i]] = terms_[i]->score_of(d, it.wdf());
				}
				if (agreed) {
					c.collect(d, sum_parts());
					d = lead.next();
				}
			}
		}
		return true;
	}

	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = confirm(align(children_[0]->approx_next())); }
	ordinal_t advance(ordinal_t t) override { return doc_ = confirm(align(children_[0]->approx_advance(t))); }
	bool two_phase() const noexcept override { return two_phase_; }
	ordinal_t approx_next() override { return doc_ = align(children_[0]->approx_next()); }
	ordinal_t approx_advance(ordinal_t t) override { return doc_ = align(children_[0]->approx_advance(t)); }
	bool matches() override {
		for (auto& c : children_) {
			if (c->two_phase() && !c->matches()) { return false; }
		}
		return true;
	}
	std::uint64_t cost() const noexcept override { return children_[0]->cost(); }
	void set_min_competitive_score(double s) override { min_ = s; }
	double score() override {
		parts_[rank_[0]] = lead_scored_ == doc_ ? lead_score_ : children_[0]->score();
		for (std::size_t i = 1; i < children_.size(); ++i) { parts_[rank_[i]] = children_[i]->score(); }
		return sum_parts();
	}
	double max_score(ordinal_t from, ordinal_t upto) override {
		double s = 0;
		for (auto& c : children_) { s += c->max_score(from, upto); }
		return s;
	}
	ordinal_t block_end(ordinal_t t) override { return children_[0]->block_end(t); }
	double max_score_total() const noexcept override { return bound_; }
	std::unique_ptr<scorer> fresh() const override {
		std::vector<scorer_ptr> kids;
		for (const auto& c : children_) {
			if (auto f = c->fresh()) { kids.push_back(std::move(f)); } else { return nullptr; }
		}
		return std::make_unique<conjunction_scorer>(std::move(kids));
	}

private:
	// Confirms approximate matches until one holds.
	ordinal_t confirm(ordinal_t d) {
		while (d != no_more_docs && two_phase_ && !matches()) { d = align(children_[0]->approx_next()); }
		return d;
	}

	// Leapfrog from the cheapest child until every child agrees. With a minimum competitive
	// score, the lead's blocks are windows: a window whose summed bounds cannot compete is
	// skipped whole, and inside a window a document whose lead score plus the other
	// children's window bounds cannot compete is skipped before the others move.
	ordinal_t align(ordinal_t target) {
		auto& lead = children_[0];
		while (target != no_more_docs) {
			if (min_ > 0) {
				if (window_end_ == no_more_docs || target > window_end_) {
					const ordinal_t end = lead->block_end(target);
					const ordinal_t upto = end == no_more_docs ? target : end;
					double sum = 0;
					for (std::size_t i = 0; i < children_.size(); ++i) { sum += window_bounds_[i] = children_[i]->max_score(target, upto); }
					if (sum_upper(sum, children_.size()) <= min_) {
						if (end == no_more_docs) { return no_more_docs; }
						target = lead->approx_advance(end + 1);
						continue;
					}
					window_end_ = end;
					rest_bound_ = 0;  // summed, not subtracted from sum, so rounding stays upward
					for (std::size_t i = 1; i < children_.size(); ++i) { rest_bound_ += window_bounds_[i]; }
				}
				if (lead->two_phase() && !lead->matches()) {
					target = lead->approx_next();
					continue;
				}
				lead_score_ = lead->score();
				lead_scored_ = target;
				if (sum_upper(lead_score_ + rest_bound_, children_.size()) <= min_) {
					target = lead->approx_next();
					continue;
				}
			}
			bool agreed = true;
			for (std::size_t i = 1; i < children_.size(); ++i) {
				ordinal_t d = children_[i]->doc() >= target && started_[i] ? children_[i]->doc() : children_[i]->approx_advance(target);
				started_[i] = true;
				if (d != target) {
					target = lead->approx_advance(d);
					agreed = false;
					break;
				}
			}
			if (agreed) { return target; }
		}
		return no_more_docs;
	}

	double sum_parts() const noexcept {
		double s = parts_[0];
		for (std::size_t i = 1; i < parts_.size(); ++i) { s += parts_[i]; }
		return s;
	}

	std::vector<scorer_ptr> children_;
	std::vector<std::uint8_t> started_;  // whether each child has been positioned
	std::vector<double> window_bounds_;
	std::vector<std::size_t> rank_;      // each child's position in the given order
	std::vector<double> parts_;          // the current document's scores, in the given order
	ordinal_t doc_ = 0, window_end_ = no_more_docs, lead_scored_ = no_more_docs;
	double bound_ = 0, min_ = 0, rest_bound_ = 0, lead_score_ = 0;
	bool two_phase_ = false;
	std::vector<term_scorer*> terms_;  // every child when all are terms, else empty
};

// Documents any child matches; score = sum of the matching children's scores.
class disjunction_scorer final : public scorer {
public:
	// Clauses ordered by bound, lowest first, for the MaxScore partition; their scores sum
	// in the given order, so a document scores the same in every segment.
	explicit disjunction_scorer(std::vector<scorer_ptr> children) : rank_(children.size()), parts_(children.size()) {
		std::vector<std::size_t> by_bound(children.size());
		for (std::size_t i = 0; i < by_bound.size(); ++i) { by_bound[i] = i; }
		std::stable_sort(by_bound.begin(), by_bound.end(), [&](std::size_t a, std::size_t b) { return children[a]->max_score_total() < children[b]->max_score_total(); });
		for (std::size_t i = 0; i < by_bound.size(); ++i) {
			children_.push_back(std::move(children[by_bound[i]]));
			rank_[i] = by_bound[i];
		}
		for (const auto& c : children_) {
			cost_ += c->cost();
			bound_ += c->max_score_total();
		}
		essential_from_ = 0;
	}

	// MaxScore: the lowest-bound clauses whose bounds sum to at most `s` cannot produce a
	// competitive document alone, so candidates come only from the rest ("essential").
	void set_min_competitive_score(double s) override {
		double sum = 0;
		std::size_t from = 0;
		while (from < children_.size() && sum_upper(sum + children_[from]->max_score_total(), from + 1) <= s) { sum += children_[from++]->max_score_total(); }
		const bool was_linear = started_ && linear();
		if (from > essential_from_ && started_) {
			// Drop newly non-essential clauses from the candidate heap.
			std::erase_if(heap_, [&](const entry& e) { return e.second < from; });
			std::make_heap(heap_.begin(), heap_.end(), later);
		}
		essential_from_ = std::max(essential_from_, from);
		if (was_linear && !linear()) {
			// Leaving linear mode: the essential clauses enter the heap where they stand.
			for (std::size_t i = essential_from_; i < children_.size(); ++i) { heap_push(i, children_[i]->doc()); }
		}
	}

	double current_bound() override {
		double b = 0;
		for (auto& c : children_) {
			if (c->doc() == doc_) { b += c->max_score_total(); }
		}
		// Non-essential clauses may match without being positioned yet.
		for (std::size_t i = 0; i < essential_from_; ++i) { b += children_[i]->doc() == doc_ ? 0 : children_[i]->max_score_total(); }
		return b;
	}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override {
		if (linear()) { return advance_linear(started_ ? doc_ + 1 : 0); }
		if (!started_) {
			started_ = true;
			for (std::size_t i = 0; i < children_.size(); ++i) { heap_push(i, children_[i]->next()); }
		} else {
			advance_matching(doc_ + 1);
		}
		return doc_ = heap_.empty() ? no_more_docs : heap_.front().first;
	}
	ordinal_t advance(ordinal_t t) override {
		if (linear()) { return advance_linear(t); }
		if (!started_) {
			started_ = true;
			for (std::size_t i = 0; i < children_.size(); ++i) { heap_push(i, children_[i]->advance(t)); }
		} else {
			advance_matching(t);
		}
		return doc_ = heap_.empty() ? no_more_docs : heap_.front().first;
	}
	std::uint64_t cost() const noexcept override { return cost_; }
	double score() override {
		for (std::size_t i = 0; i < children_.size(); ++i) {
			auto& c = children_[i];
			if (i < essential_from_ && c->doc() < doc_) { c->advance(doc_); }  // non-essential: positioned on demand
			parts_[rank_[i]] = c->doc() == doc_ ? c->score() : 0.0;
		}
		return sum_parts();
	}
	double max_score(ordinal_t from, ordinal_t upto) override {
		double s = 0;
		for (auto& c : children_) { s += c->max_score(from, upto); }
		return s;
	}
	ordinal_t block_end(ordinal_t t) override {
		ordinal_t e = no_more_docs;
		for (std::size_t i = essential_from_; i < children_.size(); ++i) { e = std::min(e, children_[i]->block_end(t)); }
		return e;
	}
	double max_score_total() const noexcept override { return bound_; }

	// A clause matching every document makes the union all of them; two clauses with known
	// counts give |A| + |B| - |A and B|, counted on fresh copies.
	std::optional<std::uint64_t> exact_count() override;

	// Block-max MaxScore over windows of 4,096 ordinals (see the definition).
	bool collect_top_k(top_collector& c) override;
	// A match scores the sum of its clauses' non-negative scores, at least any one's (F13).
	double seed(std::size_t k) override {
		double s = 0.0;
		for (const auto& c : children_) { s = std::max(s, c->seed(k)); }
		return s;
	}

	std::unique_ptr<scorer> fresh() const override {
		std::vector<scorer_ptr> kids(children_.size());
		for (std::size_t i = 0; i < children_.size(); ++i) {
			if (auto f = children_[i]->fresh()) { kids[rank_[i]] = std::move(f); } else { return nullptr; }
		}
		return std::make_unique<disjunction_scorer>(std::move(kids));
	}

	// Each child fills its own matches (positioning non-essential ones first).
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override {
		for (std::size_t i = 0; i < children_.size(); ++i) {
			auto& c = children_[i];
			if (c->doc() < base || (!started_)) { c->advance(base); }
			if (c->doc() < end) { c->fill(base, end, bits); }
		}
		started_ = true;
		heap_.clear();
		for (std::size_t i = essential_from_; i < children_.size(); ++i) { heap_push(i, children_[i]->doc()); }
		doc_ = heap_.empty() ? no_more_docs : heap_.front().first;
	}

private:
	// A few clauses with no MaxScore split advance directly to the minimum, without a heap.
	bool linear() const noexcept { return children_.size() <= 4 && essential_from_ == 0 && heap_.empty(); }
	ordinal_t advance_linear(ordinal_t t) {
		ordinal_t d = no_more_docs;
		for (auto& c : children_) {
			if (!started_ || c->doc() < t) { c->advance(t); }
			d = std::min(d, c->doc());
		}
		started_ = true;
		return doc_ = d;
	}

	using entry = std::pair<ordinal_t, std::size_t>;
	static bool later(const entry& a, const entry& b) noexcept { return a.first > b.first; }
	void heap_push(std::size_t i, ordinal_t d) {
		if (d == no_more_docs || i < essential_from_) { return; }
		heap_.push_back({d, i});
		std::push_heap(heap_.begin(), heap_.end(), later);
	}
	void advance_matching(ordinal_t t) {
		while (!heap_.empty() && heap_.front().first < t) {
			std::pop_heap(heap_.begin(), heap_.end(), later);
			std::size_t i = heap_.back().second;
			heap_.pop_back();
			heap_push(i, children_[i]->advance(t));
		}
	}

	double sum_parts() const noexcept {
		double s = 0;
		for (double p : parts_) { s += p; }
		return s;
	}

	std::vector<scorer_ptr> children_;
	std::vector<std::size_t> rank_;  // each child's position in the given order
	std::vector<double> parts_;      // the current document's scores, in the given order
	std::vector<entry> heap_;
	ordinal_t doc_ = 0;
	std::uint64_t cost_ = 0;
	double bound_ = 0;
	std::size_t essential_from_ = 0;
	bool started_ = false;
};

// Documents the required child matches; adds the optional child's score where it matches
// too (Xapian AND_MAYBE, Lucene MUST + SHOULD).
class required_optional_scorer final : public scorer {
public:
	required_optional_scorer(scorer_ptr required, scorer_ptr optional) : req_(std::move(required)), opt_(std::move(optional)) {}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ == no_more_docs ? doc_ : doc_ = prune(req_->next()); }
	ordinal_t advance(ordinal_t t) override { return doc_ == no_more_docs ? doc_ : doc_ = prune(req_->advance(t)); }
	std::uint64_t cost() const noexcept override { return req_->cost(); }
	double score() override {
		const ordinal_t d = req_->doc();
		double s = req_scored_ == d ? req_score_ : req_->score();
		if (optional_at(d) == d) { s += opt_->score(); }
		return s;
	}
	double max_score(ordinal_t from, ordinal_t upto) override { return req_->max_score(from, upto) + opt_->max_score(from, upto); }
	ordinal_t block_end(ordinal_t t) override { return req_->block_end(t); }
	double max_score_total() const noexcept override { return req_->max_score_total() + opt_->max_score_total(); }
	void set_min_competitive_score(double s) override {
		min_ = s;
		// Rounded down, so the required side's minimum never exceeds the exact difference.
		req_->set_min_competitive_score(std::nextafter(s - opt_->max_score_total(), -std::numeric_limits<double>::infinity()));
	}
	std::optional<std::uint64_t> exact_count() override { return req_->exact_count(); }  // the optional side never filters
	std::unique_ptr<scorer> fresh() const override {
		auto r = req_->fresh(), o = opt_->fresh();
		if (!r || !o) { return nullptr; }
		return std::make_unique<required_optional_scorer>(std::move(r), std::move(o));
	}

private:
	// The optional side's first match at or after d, positioning it there.
	ordinal_t optional_at(ordinal_t d) {
		if (opt_started_ && opt_->doc() >= d) { return opt_->doc(); }
		opt_started_ = true;
		return opt_->advance(d);
	}

	// With a minimum competitive score, the first required match at or after d that can
	// compete. Ported from Lucene's ReqOptSumScorer (Apache-2.0; see NOTICE): windows whose
	// two bounds cannot compete are skipped, and where the required bound alone cannot, the
	// optional side is required too and the two leapfrog. Inside a window, a required match
	// whose score plus the optional bound cannot compete is skipped before the optional moves.
	ordinal_t prune(ordinal_t d) {
		while (min_ > 0 && d != no_more_docs) {
			if (window_end_ == no_more_docs || d > window_end_) { open_window(d); }
			if (sum_upper(req_bound_ + opt_bound_, 2) <= min_) {
				d = past_window();
				continue;
			}
			if (sum_upper(req_bound_, 2) <= min_) {
				if (const ordinal_t o = optional_at(d); o != d) {
					// Past the window, the next window decides, unless the required side can
					// never compete alone (Lucene's upperBound).
					const bool alone = sum_upper(req_->max_score_total(), 2) > min_;
					d = o > window_end_ && alone ? past_window() : o == no_more_docs ? no_more_docs : req_->advance(o);
					continue;
				}
			}
			req_score_ = req_->score();
			req_scored_ = d;
			if (sum_upper(req_score_ + opt_bound_, 2) > min_) { return d; }
			d = req_->next();
		}
		return d;
	}

	// The window from d to the end of the required side's block, and both sides' bounds over
	// it; the optional side adds nothing when its next match lies past it (Lucene's
	// getMaxScore). Lucene also ends a window at the optional side's block, which measured
	// slower here: more windows, each bounded again.
	void open_window(ordinal_t d) {
		window_end_ = std::min(req_->block_end(d), no_more_docs - 1);
		req_bound_ = req_->max_score(d, window_end_);
		opt_bound_ = opt_started_ && opt_->doc() > window_end_ ? 0.0 : opt_->max_score(d, window_end_);
	}
	ordinal_t past_window() { return window_end_ >= no_more_docs - 1 ? no_more_docs : req_->advance(window_end_ + 1); }

	scorer_ptr req_, opt_;
	bool opt_started_ = false;
	double min_ = 0, req_score_ = 0, req_bound_ = 0, opt_bound_ = 0;
	ordinal_t doc_ = 0, req_scored_ = no_more_docs, window_end_ = no_more_docs;
};

// Documents `include` matches and `exclude` does not; scores come from `include`.
class exclusion_scorer final : public scorer {
public:
	exclusion_scorer(scorer_ptr include, scorer_ptr exclude) : in_(std::move(include)), ex_(std::move(exclude)) {}
	ordinal_t doc() const noexcept override { return in_->doc(); }
	ordinal_t next() override { return skip(in_->next()); }
	ordinal_t advance(ordinal_t t) override { return skip(in_->advance(t)); }
	std::uint64_t cost() const noexcept override { return in_->cost(); }
	double score() override { return in_->score(); }
	double max_score(ordinal_t from, ordinal_t upto) override { return in_->max_score(from, upto); }
	ordinal_t block_end(ordinal_t t) override { return in_->block_end(t); }
	double max_score_total() const noexcept override { return in_->max_score_total(); }
	void set_min_competitive_score(double s) override { in_->set_min_competitive_score(s); }
	double current_bound() override { return in_->current_bound(); }
	// |include| - |include and exclude| for a term minus a term.
	std::optional<std::uint64_t> exact_count() override;

	// Counting windows: include's window bits AND NOT exclude's, when filling the exclude
	// side costs less than stepping it once per included document (about a block each).
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override {
		if (ex_->cost() > in_->cost() * format::block) {
			scorer::fill(base, end, bits);
			return;
		}
		std::uint64_t inc[64] = {}, exc[64] = {};
		const std::size_t words = std::size_t(end - base + 63) / 64;
		in_->fill(base, end, inc);
		if (!ex_started_ || ex_->doc() < base) {
			ex_started_ = true;
			ex_->advance(base);
		}
		if (ex_->doc() < end) { ex_->fill(base, end, exc); }
		for (std::size_t w = 0; w < words; ++w) { bits[w] |= inc[w] & ~exc[w]; }
		skip(in_->doc());
	}
	std::unique_ptr<scorer> fresh() const override {
		auto i = in_->fresh(), e = ex_->fresh();
		if (!i || !e) { return nullptr; }
		return std::make_unique<exclusion_scorer>(std::move(i), std::move(e));
	}

private:
	ordinal_t skip(ordinal_t d) {
		while (d != no_more_docs) {
			if (!ex_started_ || ex_->doc() < d) {
				ex_started_ = true;
				ex_->advance(d);
			}
			if (ex_->doc() != d) { return d; }
			d = in_->next();
		}
		return d;
	}
	scorer_ptr in_, ex_;
	bool ex_started_ = false;
};

// Documents where the terms occur at consecutive positions; Lucene-style scoring with the
// phrase frequency as tf and the summed idf of its terms.
class phrase_scorer final : public scorer {
public:
	// Lucene scores the phrase frequency with the terms' summed idf; Xapian (term_weights
	// given, one per phrase term) sums each term's own weight for the document. A term
	// repeated in the phrase is iterated and decoded once.
	// window > 0 makes it NEAR: the terms (repeats counted) at distinct positions, in any
	// order, spanning fewer than window positions; NEAR needs term_weights.
	// Lucene mode reads `stats` and each term's `df` (index-wide, one per phrase term) when
	// given, else the segment's.
	phrase_scorer(const inverted_plugin::reader& seg, const std::vector<format::term_info>& infos, std::vector<weight> term_weights = {}, std::uint32_t window = 0,
		const statistics* stats = nullptr, std::span<const std::uint64_t> df = {})
		: seg_(&seg), term_weights_(std::move(term_weights)), window_(window) {
		if (window_ && term_weights_.size() != infos.size()) { throw std::invalid_argument("strata: NEAR needs a weight per term"); }
		if (!df.empty() && df.size() != infos.size()) { throw std::invalid_argument("strata: one document frequency per phrase term"); }
		const statistics st = stats ? *stats : statistics(seg);
		double idf = 0;
		for (std::size_t k = 0; k < infos.size(); ++k) {
			const auto& info = infos[k];
			idf += weight::bm25(st, df.empty() ? info.df : df[k]).idf;
			std::size_t u = 0;
			while (u < infos_.size() && !(infos_[u].postings == info.postings && infos_[u].positions == info.positions && infos_[u].df == info.df)) { ++u; }
			if (u == infos_.size()) {
				infos_.push_back(info);
				terms_.push_back(std::make_unique<postings_iterator>(seg, info));
				readers_.push_back(std::make_unique<positions_reader>(seg, info));
				if (info.df < cost_) {
					cost_ = info.df;
					lead_ = u;
				}
			}
			slots_.push_back({u, std::uint32_t(k)});
		}
		for (std::size_t u = 0; u < terms_.size(); ++u) {
			if (u != lead_) { others_.push_back(u); }
		}
		std::sort(others_.begin(), others_.end(), [&](std::size_t a, std::size_t b) { return infos_[a].df < infos_[b].df; });
		started_.assign(terms_.size(), false);
		positions_.resize(terms_.size());
		decoded_.assign(terms_.size(), no_more_docs);
		counts_.resize(terms_.size());
		weight_ = weight::bm25(st, 1);
		weight_.idf = idf;
		bound_ = idf;  // tf / (tf + norm) < 1
		if (!term_weights_.empty()) {
			bound_ = 0;
			for (const auto& sl : slots_) {
				double best = 0;
				for (const auto& p : infos_[sl.term].impacts) { best = std::max(best, term_weights_[sl.offset](p.wdf, format::decode_length(p.length))); }
				bound_ += best;
			}
		}
	}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = find(terms_[lead_]->next()); }
	ordinal_t advance(ordinal_t t) override { return doc_ = find(terms_[lead_]->advance(t)); }
	bool two_phase() const noexcept override { return true; }
	ordinal_t approx_next() override { return doc_ = align(terms_[lead_]->next()); }
	ordinal_t approx_advance(ordinal_t t) override { return doc_ = align(terms_[lead_]->advance(t)); }
	bool matches() override {
		if (checked_ != doc_) {
			checked_ = doc_;
			matched_ = phrase_freq(false) > 0;
		}
		return matched_;
	}
	std::uint64_t cost() const noexcept override { return cost_; }
	double score() override {
		if (term_weights_.empty()) {
			if (freq_doc_ != doc_) {
				freq_doc_ = doc_;
				freq_ = phrase_freq(true);
			}
			return weight_.by_norm(freq_, seg_->norm(doc_));
		}
		double s = 0;
		const std::uint32_t len = seg_->length(doc_);
		for (const auto& sl : slots_) { s += term_weights_[sl.offset](terms_[sl.term]->wdf(), len); }
		return s;
	}
	// Lucene mode: the phrase frequency is at most each term's position count, which is its
	// wdf when counts are wdf, so those terms' pairs bound the phrase weight and the
	// smallest such bound holds; other terms do not bound it. Xapian mode sums the terms'
	// own bounds.
	double max_score(ordinal_t from, ordinal_t upto) override {
		if (!term_weights_.empty()) {
			double sum = 0;
			for (const auto& sl : slots_) { sum += range_bound(*terms_[sl.term], term_weights_[sl.offset], from, upto); }
			return std::min(sum, bound_);
		}
		double best = bound_;
		for (auto& t : terms_) {
			if (t->info().flags & format::counts_are_wdf) { best = std::min(best, range_bound(*t, weight_, from, upto)); }
		}
		return best;
	}
	void set_min_competitive_score(double s) override { min_ = s; }
	ordinal_t block_end(ordinal_t t) override { return terms_[lead_]->block_end(t); }
	double max_score_total() const noexcept override { return bound_; }

private:
	struct slot {
		std::size_t term;      // index into terms_
		std::uint32_t offset;  // position in the phrase
	};

	// The next document at or after d holding every term, leapfrogging from the rarest.
	ordinal_t align(ordinal_t d) {
		while (d != no_more_docs) {
			bool all = true;
			for (const std::size_t i : others_) {
				ordinal_t e = started_[i] && terms_[i]->doc() >= d ? terms_[i]->doc() : terms_[i]->advance(d);
				started_[i] = true;
				if (e != d) {
					d = terms_[lead_]->advance(e);
					all = false;
					break;
				}
			}
			if (all) { return d; }
		}
		return no_more_docs;
	}

	// The next document at or after d where the phrase occurs. With a minimum competitive
	// score, the lead's blocks whose phrase bound cannot compete are skipped unverified.
	ordinal_t find(ordinal_t d) {
		d = align(d);
		while (d != no_more_docs) {
			if (min_ > 0 && (window_end_ == no_more_docs || d > window_end_)) {
				// The lead's block bound alone (one skip entry) is a valid, cheap bound.
				const ordinal_t end = terms_[lead_]->block_end(d);
				if (lead_bound(d, end == no_more_docs ? d : end) <= min_) {
					if (end == no_more_docs) { return no_more_docs; }
					d = align(terms_[lead_]->advance(end + 1));
					continue;
				}
				window_end_ = end;
			}
			checked_ = d;
			if ((matched_ = phrase_freq(false) > 0)) { return d; }
			d = align(terms_[lead_]->next());
		}
		return no_more_docs;
	}

	double lead_bound(ordinal_t from, ordinal_t upto) {
		if (!term_weights_.empty()) { return max_score(from, upto); }
		if (!(terms_[lead_]->info().flags & format::counts_are_wdf)) { return bound_; }
		return std::min(bound_, range_bound(*terms_[lead_], weight_, from, upto));
	}

	// The current document's positions of unique term u, decoded once per document.
	std::span<const std::uint32_t> positions(std::size_t u) {
		if (decoded_[u] != terms_[u]->doc()) {
			positions_[u] = readers_[u]->read(*terms_[u]);
			decoded_[u] = terms_[u]->doc();
		}
		return positions_[u];
	}

	// Occurrences of the phrase in the current document, or with all false 1 at the first
	// one: starts from the (term, offset) slot whose term has the fewest positions and
	// intersects the others into it, stopping once nothing is left.
	std::uint32_t phrase_freq(bool all) {
		if (window_) { return near_freq(); }
		for (std::size_t u = 0; u < terms_.size(); ++u) { counts_[u] = readers_[u]->count(*terms_[u]); }
		order_.assign(slots_.begin(), slots_.end());
		for (std::size_t i = 1; i < order_.size(); ++i) {  // insertion sort: a handful of slots
			for (std::size_t j = i; j > 0 && counts_[order_[j].term] < counts_[order_[j - 1].term]; --j) { std::swap(order_[j], order_[j - 1]); }
		}
		const slot first = order_[0];
		const auto lead = positions(first.term);
		if (!all) {
			// Each other slot's cursor only moves forward as the candidates rise; its positions
			// are decoded when first needed.
			lists_.assign(order_.size() - 1, {{}, unread});
			for (std::uint32_t p : lead) {
				if (p < first.offset) { continue; }
				std::size_t k = 1;
				for (; k < order_.size(); ++k) {
					auto& [pos, j] = lists_[k - 1];
					if (j == unread) {
						pos = positions(order_[k].term);
						j = 0;
					}
					const std::uint32_t want = p - first.offset + order_[k].offset;
					while (j < pos.size() && pos[j] < want) { ++j; }
					if (j == pos.size()) { return 0; }
					if (pos[j] != want) { break; }
				}
				if (k == order_.size()) { return 1; }
			}
			return 0;
		}
		candidates_.clear();
		for (std::uint32_t p : lead) {
			if (p >= first.offset) { candidates_.push_back(p - first.offset); }
		}
		for (std::size_t k = 1; k < order_.size() && !candidates_.empty(); ++k) {
			const auto pos = positions(order_[k].term);
			const std::uint32_t offset = order_[k].offset;
			std::size_t keep = 0, j = 0;
			for (std::uint32_t c : candidates_) {
				const std::uint32_t want = c + offset;
				while (j < pos.size() && pos[j] < want) { ++j; }
				if (j < pos.size() && pos[j] == want) { candidates_[keep++] = c; }
			}
			candidates_.resize(keep);
		}
		return std::uint32_t(candidates_.size());
	}

	// NEAR: 1 when some window of fewer than window_ positions holds every term as often as
	// it repeats in the query (distinct positions), else 0. Slides over the terms' positions
	// merged in order.
	std::uint32_t near_freq() {
		need_.assign(terms_.size(), 0);
		for (const auto& sl : slots_) { ++need_[sl.term]; }
		merged_.clear();
		for (std::size_t u = 0; u < terms_.size(); ++u) {
			const auto pos = positions(u);
			if (pos.size() < need_[u]) { return 0; }
			for (std::uint32_t p : pos) { merged_.push_back({p, u}); }
		}
		std::sort(merged_.begin(), merged_.end());
		have_.assign(terms_.size(), 0);
		std::size_t satisfied = 0, lo = 0;
		for (std::size_t hi = 0; hi < merged_.size(); ++hi) {
			if (++have_[merged_[hi].second] == need_[merged_[hi].second]) { ++satisfied; }
			while (merged_[hi].first - merged_[lo].first >= window_) {
				if (have_[merged_[lo].second]-- == need_[merged_[lo].second]) { --satisfied; }
				++lo;
			}
			if (satisfied == terms_.size()) { return 1; }
		}
		return 0;
	}

	// A bound on w over [from, upto] from one term's skip data: the smallest skip entry
	// covering the range (its block, its group, or the whole term).
	static double range_bound(postings_iterator& it, const weight& w, ordinal_t from, ordinal_t upto) {
		auto pairs_bound = [&](std::span<const format::impact> pairs) {
			double best = 0;
			for (const auto& p : pairs) { best = std::max(best, w(p.wdf, format::decode_length(p.length))); }
			return best;
		};
		const std::size_t b = it.block_index_for(from);
		if (b >= it.blocks()) { return pairs_bound(it.info().impacts); }
		if (it.block_last(b) >= upto) { return pairs_bound(it.block_impacts_at(b)); }
		const std::size_t g = b / 32;
		if (it.group_last(g) >= upto) { return pairs_bound(it.group_impacts(g)); }
		return pairs_bound(it.info().impacts);
	}

	const inverted_plugin::reader* seg_;
	std::vector<format::term_info> infos_;  // unique terms
	std::vector<std::unique_ptr<postings_iterator>> terms_;
	std::vector<std::unique_ptr<positions_reader>> readers_;
	std::vector<slot> slots_, order_;
	std::vector<std::span<const std::uint32_t>> positions_;
	std::vector<ordinal_t> decoded_;  // the document positions_[u] belongs to
	std::vector<std::uint32_t> candidates_, counts_, need_, have_;
	std::vector<std::pair<std::uint32_t, std::size_t>> merged_;  // NEAR: (position, term)
	static constexpr std::size_t unread = ~std::size_t(0);
	std::vector<std::pair<std::span<const std::uint32_t>, std::size_t>> lists_;  // (positions, cursor or unread)
	std::vector<std::size_t> others_;  // the terms but the lead, rarest first
	std::vector<weight> term_weights_;
	std::uint32_t window_ = 0;
	weight weight_ = weight::bm25(*seg_, 1);
	std::uint64_t cost_ = ~std::uint64_t(0);
	std::uint32_t freq_ = 0;
	bool matched_ = false;
	ordinal_t checked_ = no_more_docs;   // the document matched_ belongs to
	ordinal_t freq_doc_ = no_more_docs;  // the document freq_ belongs to
	std::size_t lead_ = 0;
	ordinal_t doc_ = 0, window_end_ = no_more_docs;
	double bound_ = 0, min_ = 0;
	std::vector<std::uint8_t> started_;
};

// --- collectors --------------------------------------------------------------------------------

inline std::uint64_t count(scorer& s);

// The k best hits of seg by score (ties by docid), best first. With count, also the number
// of matches; a count known from statistics leaves the collection free to prune.
inline std::vector<hit> top_k(const segment& seg, scorer& s, std::size_t k, std::uint64_t* count = nullptr, double floor = 0) {
	if (k == 0) {
		if (count) { *count = strata::count(s); }
		return {};
	}
	auto worse = [](const hit& a, const hit& b) { return a.score != b.score ? a.score > b.score : a.ord < b.ord; };
	if (count) {
		if (auto n = s.exact_count()) {
			*count = *n;
			count = nullptr;
		}
	}
	// A floor (a strictly lower bound on the k-th score, from other segments) prunes from
	// the start, and no hit at or below it is kept; counting never prunes.
	if (count) { floor = 0; }
	if (floor > 0) { s.set_min_competitive_score(floor); }
	top_collector c(k, count != nullptr);
	if (floor > 0) { c.seed(floor); }
	if (!seg.core().docid_ordered()) {  // ties read docids, and the threshold stays below the k-th score
		c.begin(seg);
		if (s.collect_top_k(c)) {
			if (count) { *count = c.count; }
			return c.finish();
		}
		std::uint64_t n = 0;
		double min = c.counting ? 0.0 : c.min_competitive();
		for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {
			++n;
			if (c.counting && c.min_competitive() > 0 && s.current_bound() <= c.min_competitive()) { continue; }  // counted, cannot compete
			c.collect(d, s.score());
			if (!c.counting && c.min_competitive() != min) { s.set_min_competitive_score(min = c.min_competitive()); }
		}
		if (count) { *count = n; }
		return c.finish();
	}
	if (s.collect_top_k(c)) {
		if (count) { *count = c.count; }
		return c.finish();
	}
	std::vector<hit> heap;
	std::uint64_t n = 0;
	for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {
		++n;
		if (heap.size() == k && count && s.current_bound() <= heap.front().score) { continue; }  // counted, cannot compete
		double sc = s.score();
		if (floor > 0 && sc <= floor) { continue; }  // cannot enter the top k across segments
		if (heap.size() < k) {
			heap.push_back({d, sc});
			std::push_heap(heap.begin(), heap.end(), worse);
			if (heap.size() == k && !count) { s.set_min_competitive_score(std::max(floor, equal_bound(heap.front().score))); }
		} else if (k && sc > heap.front().score) {
			std::pop_heap(heap.begin(), heap.end(), worse);
			heap.back() = {d, sc};
			std::push_heap(heap.begin(), heap.end(), worse);
			if (!count) { s.set_min_competitive_score(std::max(floor, equal_bound(heap.front().score))); }
		}
	}
	if (count) { *count = n; }
	std::sort(heap.begin(), heap.end(), worse);
	return heap;
}

// The number of matches: exact when known, else counted in windows of 4,096 ordinals.
inline std::uint64_t count(scorer& s) {
	if (auto n = s.exact_count()) { return *n; }
	std::uint64_t n = 0, bits[64];
	for (ordinal_t d = s.next(); d != no_more_docs; d = s.doc()) {
		const ordinal_t base = d & ~ordinal_t(4095), end = base > no_more_docs - 4096 ? no_more_docs : base + 4096;
		std::fill(bits, bits + 64, 0);
		s.fill(base, end, bits);
		for (auto w : bits) { n += std::uint64_t(std::popcount(w)); }
	}
	return n;
}

// Windows of 4,096 ordinals. Per window, each clause's bound over the window splits the
// clauses, lowest bound first, into non-essential ones (bounds summing to at most the
// minimum competitive score) and essential ones. Essential clauses add their scores into
// a dense array with a match bitset; each matched document then consults the non-
// essential clauses, highest bound first, while it can still compete. When counting, a
// fresh copy counts the matches from window bits (count()) and the scoring prunes as top-k
// does; without a copy, every clause is essential and every match is counted here.
inline bool disjunction_scorer::collect_top_k(top_collector& c) {
	if (started_) { return false; }
	bool count_here = c.counting;
	if (count_here) {
		if (auto copy = fresh()) {
			c.count += count(*copy);
			count_here = false;
		}
	}
	started_ = true;
	constexpr ordinal_t window = 4096;
	const std::size_t n = children_.size();
	std::vector<double> acc(n * window, 0.0), bound(n), below;  // acc: one window per clause
	std::vector<std::size_t> order(n);
	std::uint64_t bits[window / 64];
	for (auto& ch : children_) { ch->next(); }
	ordinal_t window_min = 0;
	for (;;) {
		// Clauses that can still matter given the global bounds.
		const double min = count_here ? 0.0 : c.min_competitive();
		double prefix = 0;
		std::size_t global_from = 0;
		if (min > 0) {
			while (global_from < n && sum_upper(prefix + children_[global_from]->max_score_total(), global_from + 1) <= min) { prefix += children_[global_from++]->max_score_total(); }
		}
		ordinal_t first = no_more_docs;
		for (std::size_t i = global_from; i < n; ++i) {
			if (children_[i]->doc() < window_min) { children_[i]->advance(window_min); }
			first = std::min(first, children_[i]->doc());
		}
		if (first == no_more_docs) { break; }
		window_min = first;
		const ordinal_t window_max = window_min > no_more_docs - window ? no_more_docs - 1 : window_min + window - 1;
		// Per-window split.
		for (std::size_t i = 0; i < n; ++i) {
			order[i] = i;
			bound[i] = i < global_from ? children_[i]->max_score_total() : children_[i]->max_score(window_min, window_max);
		}
		std::size_t essential = 0;
		if (min > 0) {
			std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return bound[a] < bound[b]; });
			double sum = 0;
			while (essential < n && sum_upper(sum + bound[order[essential]], essential + 1) <= min) { sum += bound[order[essential++]]; }
			if (essential == n) {  // nothing in this window can compete
				window_min = window_max + 1;
				if (window_max == no_more_docs - 1) { break; }
				continue;
			}
		}
		std::fill(bits, bits + window / 64, 0);
		for (std::size_t e = essential; e < n; ++e) {
			auto& ch = children_[order[e]];
			if (ch->doc() < window_min) { ch->advance(window_min); }
			if (ch->doc() <= window_max) { ch->score_window(window_min, window_max + 1, acc.data() + order[e] * window, bits); }
		}
		// Bounds of the non-essential clauses below each one, summed rather than subtracted
		// so rounding never understates what remains.
		below.assign(essential + 1, 0.0);
		for (std::size_t e = 0; e < essential; ++e) { below[e + 1] = below[e] + bound[order[e]]; }
		for (std::size_t w = 0; w < window / 64; ++w) {
			for (std::uint64_t word = bits[w]; word; word &= word - 1) {
				const ordinal_t i = ordinal_t(w * 64 + std::size_t(std::countr_zero(word)));
				const ordinal_t d = window_min + i;
				// Each clause's score lands in parts_ by its given position; the running
				// sum only bounds what remains.
				double score = 0;
				for (std::size_t e = essential; e < n; ++e) {
					double& a = acc[order[e] * window + i];
					score += parts_[rank_[order[e]]] = a;
					a = 0;
				}
				if (count_here) { ++c.count; }
				bool competes = true;
				for (std::size_t e = essential; e-- > 0;) {
					if (!count_here && sum_upper(score + below[e + 1], n) <= c.min_competitive()) {
						competes = false;
						break;
					}
					auto& ch = children_[order[e]];
					if (ch->doc() < d) { ch->advance(d); }
					const double v = ch->doc() == d ? ch->score() : 0.0;
					score += parts_[rank_[order[e]]] = v;
				}
				if (competes) { c.collect(d, sum_parts()); }
			}
		}
		if (window_max == no_more_docs - 1) { break; }
		window_min = window_max + 1;
	}
	doc_ = no_more_docs;
	return true;
}

inline std::optional<std::uint64_t> exclusion_scorer::exact_count() {
	// Only term minus term: anything else counts faster in one pass than through
	// |include| - |include and exclude|.
	if (!dynamic_cast<term_scorer*>(in_.get()) || !dynamic_cast<term_scorer*>(ex_.get())) { return std::nullopt; }
	auto n = in_->exact_count();
	if (!n) { return std::nullopt; }
	auto i = in_->fresh(), e = ex_->fresh();
	if (!i || !e) { return std::nullopt; }
	std::vector<scorer_ptr> both;
	both.push_back(std::move(i));
	both.push_back(std::move(e));
	conjunction_scorer common(std::move(both));
	return *n - count(common);
}

inline std::optional<std::uint64_t> disjunction_scorer::exact_count() {
	std::vector<std::uint64_t> counts;
	for (auto& c : children_) {
		auto n = c->exact_count();
		if (n && c->matches_all()) { return n; }
		if (n) { counts.push_back(*n); }
	}
	if (children_.size() != 2 || counts.size() != 2) { return std::nullopt; }
	auto a = children_[0]->fresh(), b = children_[1]->fresh();
	if (!a || !b) { return std::nullopt; }
	std::vector<scorer_ptr> both;
	both.push_back(std::move(a));
	both.push_back(std::move(b));
	conjunction_scorer and_(std::move(both));
	return counts[0] + counts[1] - count(and_);
}

}  // namespace strata
