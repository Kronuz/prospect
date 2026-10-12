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
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace strata {

// --- weighting ----------------------------------------------------------------------------------

enum class scheme { lucene_bm25, xapian_bm25 };

// A term's weighting function: score(tf, length) for one scheme.
//   Lucene BM25 (k1 = 1.2, b = 0.75): idf · tf / (tf + k1 · (1 − b + b · len / avglen)),
//     idf = ln(1 + (N − df + 0.5) / (df + 0.5)).
//   Xapian BM25 (k1 = 1, k2 = 0, k3 = 1, b = 0.5, min_normlen = 0.5):
//     idf · (k3 + 1) · wqf / (k3 + wqf) · (k1 + 1) · tf / (K + tf),
//     K = k1 · ((1 − b) + b · max(len / avglen, min_normlen)), idf from (N − df + 0.5) / (df + 0.5).
struct weight {
	scheme kind = scheme::lucene_bm25;
	double k1 = 1.2, b = 0.75, min_normlen = 0;
	double idf = 0, avg = 1, inv_avg = 1;  // inv_avg = 1 / avg: one division per score
	// Lucene BM25 scores quantized lengths, as Lucene does: norm_k[code] is k1 * (1 - b + b *
	// decode_length(code) / avg), so a score is one table load and one division.
	std::array<double, 256> norm_k{};

	static weight lucene(const segment& seg, std::uint64_t df, double boost = 1.0) {
		weight w;
		w.idf = boost * std::log(1.0 + (double(seg.doc_count()) - double(df) + 0.5) / (double(df) + 0.5));
		w.avg = std::max(1.0, seg.average_length());
		w.inv_avg = 1.0 / w.avg;
		for (unsigned c = 0; c < 256; ++c) { w.norm_k[c] = w.k1 * (1.0 - w.b + w.b * double(format::decode_length(std::uint8_t(c))) * w.inv_avg); }
		return w;
	}

	static weight xapian(const segment& seg, std::uint64_t df, std::uint32_t wqf = 1) {
		weight w;
		w.kind = scheme::xapian_bm25;
		w.k1 = 1.0;
		w.b = 0.5;
		w.min_normlen = 0.5;
		double tw = (double(seg.doc_count()) - double(df) + 0.5) / (double(df) + 0.5);
		if (tw < 2) { tw = tw * 0.5 + 1; }  // Xapian keeps small idf positive
		constexpr double k3 = 1.0;
		w.idf = std::log(tw) * (k3 + 1) * wqf / (k3 + wqf) * (w.k1 + 1);
		w.avg = seg.average_length() > 0 ? seg.average_length() : 1.0;
		w.inv_avg = 1.0 / w.avg;
		return w;
	}

	// Lucene BM25 by quantized length (segment::norm).
	double by_norm(std::uint32_t tf, std::uint8_t norm) const noexcept {
		return tf ? idf * double(tf) / (double(tf) + norm_k[norm]) : 0.0;
	}

	double operator()(std::uint32_t tf, std::uint32_t length) const noexcept {
		if (tf == 0) { return 0.0; }
		if (kind == scheme::lucene_bm25) { return by_norm(tf, format::encode_length(length)); }
		const double normlen = std::max(double(length) * inv_avg, min_normlen);
		const double K = k1 * ((1.0 - b) + b * normlen);
		return idf * double(tf) / (K + double(tf));
	}
};

// --- scorers -----------------------------------------------------------------------------------

struct top_collector;

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
	// first match >= end; requires base <= doc() and end - base <= 64 * words of bits.
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
};

struct hit {
	ordinal_t ord;
	double score;
};

// The best k hits, as Tantivy's TopNComputer keeps them: a buffer of up to 2k hits that,
// when full, keeps its k best (nth_element) and raises the threshold to the k-th best
// score; amortized constant time per hit. Ties keep the smaller ordinal (hits arrive in
// increasing ordinal order).
struct top_collector {
	explicit top_collector(std::size_t k, bool counting) : k(k), counting(counting) { hits.reserve(2 * k + 1); }
	// True when a is better than b.
	static bool better(const hit& a, const hit& b) noexcept { return a.score != b.score ? a.score > b.score : a.ord < b.ord; }
	// Only documents scoring above this can enter (0 until k hits are known).
	double min_competitive() const noexcept { return threshold; }
	void collect(ordinal_t d, double score) {
		if (!k || (truncated && score <= threshold)) { return; }
		hits.push_back({d, score});
		if (hits.size() >= 2 * k) { truncate(); }
	}
	// The k best, best first.
	std::vector<hit> finish() {
		if (hits.size() > k) { truncate(); }
		std::sort(hits.begin(), hits.end(), better);
		return std::move(hits);
	}
	std::size_t k;
	bool counting;           // every match must be counted: no skipping by score
	std::uint64_t count = 0;

private:
	void truncate() {
		std::nth_element(hits.begin(), hits.begin() + std::ptrdiff_t(k - 1), hits.end(), better);
		hits.resize(k);
		threshold = hits[k - 1].score;  // the k-th best; every kept hit scores at least this
		truncated = true;
	}
	std::vector<hit> hits;
	double threshold = 0;
	bool truncated = false;
};

using scorer_ptr = std::unique_ptr<scorer>;

class term_scorer final : public scorer {
public:
	term_scorer(const segment& seg, const format::term_info& info, double boost = 1.0)
		: term_scorer(seg, info, weight::lucene(seg, info.df, boost)) {}

	term_scorer(const segment& seg, const format::term_info& info, const weight& w) : seg_(&seg), it_(seg, info), weight_(w) {
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

	// Lucene BM25 reads the one-byte norm; Xapian BM25 reads the exact length.
	double score_of(ordinal_t ord, std::uint32_t wdf) const {
		return weight_.kind == scheme::lucene_bm25 ? weight_.by_norm(wdf, seg_->norm(ord)) : weight_(wdf, seg_->length(ord));
	}
	const segment& seg() const noexcept { return *seg_; }

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
				d = it_.advance(g < it_.groups() ? it_.group_first(g) : it_.group_last(g - 1) + 1);
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

	const segment* seg_;
	postings_iterator it_;
	weight weight_;
	double total_bound_ = 0, min_ = 0;
	ordinal_t checked_until_ = no_more_docs;
	std::vector<double> group_bounds_;
	std::size_t cached_group_ = std::size_t(-1);
	double block_bounds_[32];

};

// Documents every child matches; score = sum.
class conjunction_scorer final : public scorer {
public:
	explicit conjunction_scorer(std::vector<scorer_ptr> children)
		: children_(std::move(children)), started_(children_.size(), false), window_bounds_(children_.size(), 0.0) {
		std::sort(children_.begin(), children_.end(), [](const scorer_ptr& a, const scorer_ptr& b) { return a->cost() < b->cost(); });
		for (const auto& c : children_) {
			bound_ += c->max_score_total();
			two_phase_ = two_phase_ || c->two_phase();
			auto* t = dynamic_cast<term_scorer*>(c.get());
			if (t) { terms_.push_back(t); }
		}
		if (terms_.size() != children_.size()) { terms_.clear(); }
	}

	// Counting fast paths for conjunctions of terms: a dense lead ANDs each term's window
	// bits; a sparse lead leapfrogs on the postings directly.
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override {
		if (terms_.empty() || min_ > 0) {
			scorer::fill(base, end, bits);
			return;
		}
		const std::size_t words = std::size_t(end - base + 63) / 64;
		if (terms_[0]->cost() * 16 >= terms_[0]->seg().doc_count()) {
			std::uint64_t all[64], one[64];
			for (std::size_t i = 0; i < terms_.size(); ++i) {
				auto& it = terms_[i]->postings();
				if (it.doc() < base) { it.advance(base); }
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
		double s = lead_scored_ == doc_ ? lead_score_ : children_[0]->score();
		for (std::size_t i = 1; i < children_.size(); ++i) { s += children_[i]->score(); }
		return s;
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
					if (sum <= min_) {
						if (end == no_more_docs) { return no_more_docs; }
						target = lead->approx_advance(end + 1);
						continue;
					}
					window_end_ = end;
					rest_bound_ = sum - window_bounds_[0];
				}
				if (lead->two_phase() && !lead->matches()) {
					target = lead->approx_next();
					continue;
				}
				lead_score_ = lead->score();
				lead_scored_ = target;
				if (lead_score_ + rest_bound_ <= min_) {
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

	std::vector<scorer_ptr> children_;
	std::vector<bool> started_;  // whether each child has been positioned
	std::vector<double> window_bounds_;
	ordinal_t doc_ = 0, window_end_ = no_more_docs, lead_scored_ = no_more_docs;
	double bound_ = 0, min_ = 0, rest_bound_ = 0, lead_score_ = 0;
	bool two_phase_ = false;
	std::vector<term_scorer*> terms_;  // every child when all are terms, else empty
};

// Documents any child matches; score = sum of the matching children's scores.
class disjunction_scorer final : public scorer {
public:
	explicit disjunction_scorer(std::vector<scorer_ptr> children) : children_(std::move(children)) {
		// Clauses ordered by bound, lowest first, for the MaxScore partition.
		std::sort(children_.begin(), children_.end(), [](const scorer_ptr& a, const scorer_ptr& b) { return a->max_score_total() < b->max_score_total(); });
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
		while (from < children_.size() && sum + children_[from]->max_score_total() <= s) { sum += children_[from++]->max_score_total(); }
		if (from > essential_from_ && started_) {
			// Drop newly non-essential clauses from the candidate heap.
			std::erase_if(heap_, [&](const entry& e) { return e.second < from; });
			std::make_heap(heap_.begin(), heap_.end(), later);
		}
		essential_from_ = std::max(essential_from_, from);
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
		if (!started_) {
			started_ = true;
			for (std::size_t i = 0; i < children_.size(); ++i) { heap_push(i, children_[i]->next()); }
		} else {
			advance_matching(doc_ + 1);
		}
		return doc_ = heap_.empty() ? no_more_docs : heap_.front().first;
	}
	ordinal_t advance(ordinal_t t) override {
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
		double s = 0;
		for (std::size_t i = 0; i < children_.size(); ++i) {
			auto& c = children_[i];
			if (i < essential_from_ && c->doc() < doc_) { c->advance(doc_); }  // non-essential: positioned on demand
			if (c->doc() == doc_) { s += c->score(); }
		}
		return s;
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

	std::unique_ptr<scorer> fresh() const override {
		std::vector<scorer_ptr> kids;
		for (const auto& c : children_) {
			if (auto f = c->fresh()) { kids.push_back(std::move(f)); } else { return nullptr; }
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

	std::vector<scorer_ptr> children_;
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
	ordinal_t doc() const noexcept override { return req_->doc(); }
	ordinal_t next() override { return prune(req_->next()); }
	ordinal_t advance(ordinal_t t) override { return prune(req_->advance(t)); }
	std::uint64_t cost() const noexcept override { return req_->cost(); }
	double score() override {
		const ordinal_t d = req_->doc();
		double s = req_scored_ == d ? req_score_ : req_->score();
		if (opt_->doc() < d || !opt_started_) {
			opt_started_ = true;
			opt_->advance(d);
		}
		if (opt_->doc() == d) { s += opt_->score(); }
		return s;
	}
	double max_score(ordinal_t from, ordinal_t upto) override { return req_->max_score(from, upto) + opt_->max_score(from, upto); }
	ordinal_t block_end(ordinal_t t) override { return req_->block_end(t); }
	double max_score_total() const noexcept override { return req_->max_score_total() + opt_->max_score_total(); }
	void set_min_competitive_score(double s) override {
		min_ = s;
		req_->set_min_competitive_score(s - opt_->max_score_total());
	}
	std::optional<std::uint64_t> exact_count() override { return req_->exact_count(); }  // the optional side never filters
	std::unique_ptr<scorer> fresh() const override {
		auto r = req_->fresh(), o = opt_->fresh();
		if (!r || !o) { return nullptr; }
		return std::make_unique<required_optional_scorer>(std::move(r), std::move(o));
	}

private:
	// With a minimum competitive score, skips required matches whose score plus the
	// optional side's bound over the required block cannot compete, before moving the
	// optional side.
	ordinal_t prune(ordinal_t d) {
		while (min_ > 0 && d != no_more_docs) {
			if (window_end_ == no_more_docs || d > window_end_) {
				const ordinal_t end = req_->block_end(d);
				window_end_ = end;
				opt_bound_ = opt_->max_score(d, end == no_more_docs ? d : end);
			}
			req_score_ = req_->score();
			req_scored_ = d;
			if (req_score_ + opt_bound_ > min_) { return d; }
			d = req_->next();
		}
		return d;
	}

	scorer_ptr req_, opt_;
	bool opt_started_ = false;
	double min_ = 0, req_score_ = 0, opt_bound_ = 0;
	ordinal_t req_scored_ = no_more_docs, window_end_ = no_more_docs;
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
	phrase_scorer(const segment& seg, const std::vector<format::term_info>& infos, std::vector<weight> term_weights = {})
		: seg_(&seg), term_weights_(std::move(term_weights)) {
		double idf = 0;
		for (std::size_t k = 0; k < infos.size(); ++k) {
			const auto& info = infos[k];
			idf += weight::lucene(seg, info.df).idf;
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
		started_.assign(terms_.size(), false);
		positions_.resize(terms_.size());
		decoded_.assign(terms_.size(), no_more_docs);
		counts_.resize(terms_.size());
		weight_ = weight::lucene(seg, 1);
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
			freq_ = phrase_freq();
		}
		return freq_ > 0;
	}
	std::uint64_t cost() const noexcept override { return cost_; }
	double score() override {
		if (term_weights_.empty()) { return weight_.by_norm(freq_, seg_->norm(doc_)); }
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
			for (std::size_t i = 0; i < terms_.size(); ++i) {
				if (i == lead_) { continue; }
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
			if ((freq_ = phrase_freq()) > 0) { return d; }
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
	const std::vector<std::uint32_t>& positions(std::size_t u) {
		if (decoded_[u] != terms_[u]->doc()) {
			readers_[u]->read(*terms_[u], positions_[u]);
			decoded_[u] = terms_[u]->doc();
		}
		return positions_[u];
	}

	// Occurrences of the phrase in the current document: starts from the (term, offset)
	// slot whose term has the fewest positions and intersects the others into it,
	// stopping once nothing is left.
	std::uint32_t phrase_freq() {
		for (std::size_t u = 0; u < terms_.size(); ++u) { counts_[u] = readers_[u]->count(*terms_[u]); }
		order_.assign(slots_.begin(), slots_.end());
		for (std::size_t i = 1; i < order_.size(); ++i) {  // insertion sort: a handful of slots
			for (std::size_t j = i; j > 0 && counts_[order_[j].term] < counts_[order_[j - 1].term]; --j) { std::swap(order_[j], order_[j - 1]); }
		}
		const slot first = order_[0];
		const auto& lead = positions(first.term);
		candidates_.clear();
		for (std::uint32_t p : lead) {
			if (p >= first.offset) { candidates_.push_back(p - first.offset); }
		}
		for (std::size_t k = 1; k < order_.size() && !candidates_.empty(); ++k) {
			const auto& pos = positions(order_[k].term);
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

	const segment* seg_;
	std::vector<format::term_info> infos_;  // unique terms
	std::vector<std::unique_ptr<postings_iterator>> terms_;
	std::vector<std::unique_ptr<positions_reader>> readers_;
	std::vector<slot> slots_, order_;
	std::vector<std::vector<std::uint32_t>> positions_;
	std::vector<ordinal_t> decoded_;  // the document positions_[u] belongs to
	std::vector<std::uint32_t> candidates_, counts_;
	std::vector<weight> term_weights_;
	weight weight_ = weight::lucene(*seg_, 1);
	std::uint64_t cost_ = ~std::uint64_t(0);
	std::uint32_t freq_ = 0;
	ordinal_t checked_ = no_more_docs;  // the document freq_ belongs to
	std::size_t lead_ = 0;
	ordinal_t doc_ = 0, window_end_ = no_more_docs;
	double bound_ = 0, min_ = 0;
	std::vector<bool> started_;
};

// --- collectors --------------------------------------------------------------------------------

inline std::uint64_t count(scorer& s);

// The k best hits by score (ties by ordinal), best first. With count, also the number of
// matches; a count known from statistics leaves the collection free to prune.
inline std::vector<hit> top_k(scorer& s, std::size_t k, std::uint64_t* count = nullptr) {
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
	if (top_collector c(k, count != nullptr); s.collect_top_k(c)) {
		if (count) { *count = c.count; }
		return c.finish();
	}
	std::vector<hit> heap;
	std::uint64_t n = 0;
	for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) {
		++n;
		if (heap.size() == k && count && s.current_bound() <= heap.front().score) { continue; }  // counted, cannot compete
		double sc = s.score();
		if (heap.size() < k) {
			heap.push_back({d, sc});
			std::push_heap(heap.begin(), heap.end(), worse);
			if (heap.size() == k && !count) { s.set_min_competitive_score(heap.front().score); }
		} else if (k && sc > heap.front().score) {
			std::pop_heap(heap.begin(), heap.end(), worse);
			heap.back() = {d, sc};
			std::push_heap(heap.begin(), heap.end(), worse);
			if (!count) { s.set_min_competitive_score(heap.front().score); }
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
		const ordinal_t base = d & ~ordinal_t(4095), end = base + 4096;
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
// essential clauses, highest bound first, while it can still compete. When counting,
// every clause is essential and every match is counted.
inline bool disjunction_scorer::collect_top_k(top_collector& c) {
	if (started_) { return false; }
	started_ = true;
	constexpr ordinal_t window = 4096;
	const std::size_t n = children_.size();
	std::vector<double> acc(window, 0.0), bound(n);
	std::vector<std::size_t> order(n);
	std::uint64_t bits[window / 64];
	for (auto& ch : children_) { ch->next(); }
	ordinal_t window_min = 0;
	for (;;) {
		// Clauses that can still matter given the global bounds.
		const double min = c.counting ? 0.0 : c.min_competitive();
		double prefix = 0;
		std::size_t global_from = 0;
		if (min > 0) {
			while (global_from < n && prefix + children_[global_from]->max_score_total() <= min) { prefix += children_[global_from++]->max_score_total(); }
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
			while (essential < n && sum + bound[order[essential]] <= min) { sum += bound[order[essential++]]; }
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
			if (ch->doc() <= window_max) { ch->score_window(window_min, window_max + 1, acc.data(), bits); }
		}
		// Remaining bound of the non-essential clauses, highest first.
		double rest = 0;
		for (std::size_t e = 0; e < essential; ++e) { rest += bound[order[e]]; }
		for (std::size_t w = 0; w < window / 64; ++w) {
			for (std::uint64_t word = bits[w]; word; word &= word - 1) {
				const ordinal_t i = ordinal_t(w * 64 + std::size_t(std::countr_zero(word)));
				const ordinal_t d = window_min + i;
				double score = acc[i];
				acc[i] = 0;
				++c.count;
				double remaining = rest;
				for (std::size_t e = essential; e-- > 0;) {
					const double m = c.counting ? 0.0 : c.min_competitive();
					if (!c.counting && score + remaining <= m) { break; }
					auto& ch = children_[order[e]];
					if (ch->doc() < d) { ch->advance(d); }
					if (ch->doc() == d) { score += ch->score(); }
					remaining -= bound[order[e]];
				}
				c.collect(d, score);
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
	std::uint64_t common = 0;
	for (ordinal_t d = and_.next(); d != no_more_docs; d = and_.next()) { ++common; }
	return counts[0] + counts[1] - common;
}

}  // namespace strata
