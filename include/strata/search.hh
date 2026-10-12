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
	double idf = 0, avg = 1;

	static weight lucene(const segment& seg, std::uint64_t df, double boost = 1.0) {
		weight w;
		w.idf = boost * std::log(1.0 + (double(seg.doc_count()) - double(df) + 0.5) / (double(df) + 0.5));
		w.avg = std::max(1.0, seg.average_length());
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
		return w;
	}

	double operator()(std::uint32_t tf, std::uint32_t length) const noexcept {
		if (tf == 0) { return 0.0; }
		if (kind == scheme::lucene_bm25) {
			const double norm = k1 * (1.0 - b + b * double(length) / avg);
			return idf * double(tf) / (double(tf) + norm);
		}
		const double normlen = std::max(double(length) / avg, min_normlen);
		const double K = k1 * ((1.0 - b) + b * normlen);
		return idf * double(tf) / (K + double(tf));
	}
};

// --- scorers -----------------------------------------------------------------------------------

// The virtual scorer interface; every implementation satisfies strata::scorer.
struct scorer {
	virtual ~scorer() = default;
	virtual ordinal_t doc() const noexcept = 0;
	virtual ordinal_t next() = 0;
	virtual ordinal_t advance(ordinal_t target) = 0;
	virtual std::uint64_t cost() const noexcept = 0;
	virtual double score() = 0;
	// An upper bound on score() over [doc(), upto].
	virtual double max_score(ordinal_t upto) = 0;
	// The last ordinal of the block holding target, or no_more_docs.
	virtual ordinal_t block_end(ordinal_t target) = 0;
	// An upper bound on every score this scorer returns.
	virtual double max_score_total() const noexcept = 0;
	// Only documents scoring above `s` are wanted from now on; scorers may skip the rest.
	virtual void set_min_competitive_score(double s) { (void)s; }
	// An upper bound on score() for the current document, cheaper than score().
	virtual double current_bound() { return max_score_total(); }
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
	std::uint64_t cost() const noexcept override { return it_.cost(); }
	double score() override { return weight_(it_.wdf(), seg_->length(it_.doc())); }
	void set_min_competitive_score(double s) override { min_ = s; }
	ordinal_t block_end(ordinal_t t) override { return it_.block_end(t); }
	double max_score_total() const noexcept override { return total_bound_; }

	double max_score(ordinal_t upto) override {
		double best = 0;
		ordinal_t at = it_.doc() == no_more_docs ? upto : it_.doc();
		while (at <= upto) {
			const auto& pairs = it_.block_impacts(at);
			for (const auto& p : pairs) { best = std::max(best, weight_(p.wdf, format::decode_length(p.length))); }
			ordinal_t end = it_.block_end(at);
			if (end == no_more_docs || end >= upto) { break; }
			at = end + 1;
		}
		return best;
	}

	postings_iterator& postings() noexcept { return it_; }

private:
	// Moves past blocks whose bound cannot beat the minimum competitive score.
	ordinal_t skip_blocks(ordinal_t d) {
		while (min_ > 0 && d != no_more_docs) {
			if (block_checked_until_ != no_more_docs && d <= block_checked_until_) { return d; }
			const ordinal_t end = it_.block_end(d);
			double bound = 0;
			for (const auto& p : it_.block_impacts(d)) { bound = std::max(bound, weight_(p.wdf, format::decode_length(p.length))); }
			if (bound > min_) {
				block_checked_until_ = end;
				return d;
			}
			if (end == no_more_docs) { return no_more_docs; }
			d = it_.advance(end + 1);
		}
		return d;
	}

	const segment* seg_;
	postings_iterator it_;
	weight weight_;
	double total_bound_ = 0, min_ = 0;
	ordinal_t block_checked_until_ = no_more_docs;
};

// Documents every child matches; score = sum.
class conjunction_scorer final : public scorer {
public:
	explicit conjunction_scorer(std::vector<scorer_ptr> children) : children_(std::move(children)), started_(children_.size(), false) {
		std::sort(children_.begin(), children_.end(), [](const scorer_ptr& a, const scorer_ptr& b) { return a->cost() < b->cost(); });
		for (const auto& c : children_) { bound_ += c->max_score_total(); }
	}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = align(children_[0]->next()); }
	ordinal_t advance(ordinal_t t) override { return doc_ = align(children_[0]->advance(t)); }
	std::uint64_t cost() const noexcept override { return children_[0]->cost(); }
	void set_min_competitive_score(double s) override { min_ = s; }
	double score() override {
		double s = 0;
		for (auto& c : children_) { s += c->score(); }
		return s;
	}
	double max_score(ordinal_t upto) override {
		double s = 0;
		for (auto& c : children_) { s += c->max_score(upto); }
		return s;
	}
	ordinal_t block_end(ordinal_t t) override { return children_[0]->block_end(t); }
	double max_score_total() const noexcept override { return bound_; }

private:
	// Leapfrog from the cheapest child's candidate until every child agrees. With a minimum
	// competitive score, windows up to the nearest block end whose summed bounds cannot
	// compete are skipped whole.
	ordinal_t align(ordinal_t target) {
		while (target != no_more_docs) {
			if (min_ > 0 && (window_end_ == no_more_docs || target > window_end_)) {
				ordinal_t end = no_more_docs;
				for (auto& c : children_) { end = std::min(end, c->block_end(target)); }
				double bound = 0;
				for (auto& c : children_) { bound += c->max_score(end == no_more_docs ? target : end); }
				if (bound <= min_) {
					if (end == no_more_docs) { return no_more_docs; }
					target = children_[0]->advance(end + 1);
					continue;
				}
				window_end_ = end;
			}
			bool agreed = true;
			for (std::size_t i = 1; i < children_.size(); ++i) {
				ordinal_t d = children_[i]->doc() >= target && started_[i] ? children_[i]->doc() : children_[i]->advance(target);
				started_[i] = true;
				if (d != target) {
					target = children_[0]->advance(d);
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
	ordinal_t doc_ = 0, window_end_ = no_more_docs;
	double bound_ = 0, min_ = 0;
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
	double max_score(ordinal_t upto) override {
		double s = 0;
		for (auto& c : children_) { s += c->max_score(upto); }
		return s;
	}
	ordinal_t block_end(ordinal_t t) override {
		ordinal_t e = no_more_docs;
		for (std::size_t i = essential_from_; i < children_.size(); ++i) { e = std::min(e, children_[i]->block_end(t)); }
		return e;
	}
	double max_score_total() const noexcept override { return bound_; }

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
	ordinal_t next() override { return req_->next(); }
	ordinal_t advance(ordinal_t t) override { return req_->advance(t); }
	std::uint64_t cost() const noexcept override { return req_->cost(); }
	double score() override {
		double s = req_->score();
		const ordinal_t d = req_->doc();
		if (opt_->doc() < d || !opt_started_) {
			opt_started_ = true;
			opt_->advance(d);
		}
		if (opt_->doc() == d) { s += opt_->score(); }
		return s;
	}
	double max_score(ordinal_t upto) override { return req_->max_score(upto) + opt_->max_score(upto); }
	ordinal_t block_end(ordinal_t t) override { return req_->block_end(t); }
	double max_score_total() const noexcept override { return req_->max_score_total() + opt_->max_score_total(); }
	void set_min_competitive_score(double s) override { req_->set_min_competitive_score(s - opt_->max_score_total()); }

private:
	scorer_ptr req_, opt_;
	bool opt_started_ = false;
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
	double max_score(ordinal_t upto) override { return in_->max_score(upto); }
	ordinal_t block_end(ordinal_t t) override { return in_->block_end(t); }
	double max_score_total() const noexcept override { return in_->max_score_total(); }
	void set_min_competitive_score(double s) override { in_->set_min_competitive_score(s); }
	double current_bound() override { return in_->current_bound(); }

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
	// given) sums each term's own weight for the document.
	phrase_scorer(const segment& seg, const std::vector<format::term_info>& infos, std::vector<weight> term_weights = {})
		: seg_(&seg), term_weights_(std::move(term_weights)) {
		double idf = 0;
		for (const auto& info : infos) {
			terms_.push_back(std::make_unique<postings_iterator>(seg, info));
			readers_.push_back(std::make_unique<positions_reader>(seg, info, terms_.back()->blocks(), terms_.back()->tail_count()));
			counts_wdf_.push_back(info.flags & format::counts_are_wdf);
			idf += weight::lucene(seg, info.df).idf;
			cost_ = std::min(cost_, std::uint64_t(info.df));
		}
		started_.assign(terms_.size(), false);
		weight_ = weight::lucene(seg, 1);
		weight_.idf = idf;
		bound_ = idf;  // tf / (tf + norm) < 1
		if (!term_weights_.empty()) {
			bound_ = 0;
			for (std::size_t i = 0; i < infos.size(); ++i) {
				double best = 0;
				for (const auto& p : infos[i].impacts) { best = std::max(best, term_weights_[i](p.wdf, format::decode_length(p.length))); }
				bound_ += best;
			}
		}
	}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = find(terms_[0]->next()); }
	ordinal_t advance(ordinal_t t) override { return doc_ = find(terms_[0]->advance(t)); }
	std::uint64_t cost() const noexcept override { return cost_; }
	double score() override {
		if (term_weights_.empty()) { return weight_(freq_, seg_->length(doc_)); }
		double s = 0;
		const std::uint32_t len = seg_->length(doc_);
		for (std::size_t i = 0; i < terms_.size(); ++i) { s += term_weights_[i](terms_[i]->wdf(), len); }
		return s;
	}
	double max_score(ordinal_t) override { return bound_; }
	ordinal_t block_end(ordinal_t t) override { return terms_[0]->block_end(t); }
	double max_score_total() const noexcept override { return bound_; }

private:
	ordinal_t find(ordinal_t d) {
		while (d != no_more_docs) {
			bool all = true;
			for (std::size_t i = 1; i < terms_.size(); ++i) {
				ordinal_t e = started_[i] && terms_[i]->doc() >= d ? terms_[i]->doc() : terms_[i]->advance(d);
				started_[i] = true;
				if (e != d) {
					d = terms_[0]->advance(e);
					all = false;
					break;
				}
			}
			if (all && (freq_ = phrase_freq()) > 0) { return d; }
			if (all) { d = terms_[0]->next(); }
		}
		return no_more_docs;
	}

	std::uint32_t phrase_freq() {
		positions_.resize(terms_.size());
		for (std::size_t i = 0; i < terms_.size(); ++i) {
			auto& it = *terms_[i];
			const std::uint32_t* counts = counts_wdf_[i] ? it.wdfs() : readers_[i]->counts(it.current_block());
			readers_[i]->read(it.position_index(counts), counts[it.index_in_block()], positions_[i]);
		}
		std::uint32_t n = 0;
		for (std::uint32_t p : positions_[0]) {
			bool ok = true;
			for (std::size_t i = 1; i < terms_.size() && ok; ++i) { ok = std::binary_search(positions_[i].begin(), positions_[i].end(), p + std::uint32_t(i)); }
			n += ok;
		}
		return n;
	}

	const segment* seg_;
	std::vector<std::unique_ptr<postings_iterator>> terms_;
	std::vector<std::unique_ptr<positions_reader>> readers_;
	std::vector<bool> counts_wdf_;
	std::vector<std::vector<std::uint32_t>> positions_;
	std::vector<weight> term_weights_;
	weight weight_ = weight::lucene(*seg_, 1);
	std::uint64_t cost_ = ~std::uint64_t(0);
	std::uint32_t freq_ = 0;
	ordinal_t doc_ = 0;
	double bound_ = 0;
	std::vector<bool> started_;
};

// --- collectors --------------------------------------------------------------------------------

struct hit {
	ordinal_t ord;
	double score;
};

// The k best hits by score (ties by ordinal), best first.
inline std::vector<hit> top_k(scorer& s, std::size_t k, std::uint64_t* count = nullptr) {
	auto worse = [](const hit& a, const hit& b) { return a.score != b.score ? a.score > b.score : a.ord < b.ord; };
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

inline std::uint64_t count(scorer& s) {
	std::uint64_t n = 0;
	for (ordinal_t d = s.next(); d != no_more_docs; d = s.next()) { ++n; }
	return n;
}

}  // namespace strata
