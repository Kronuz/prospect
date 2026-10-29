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

// The query operators Xapiand's language emits beyond AND, OR, AND_MAYBE and AND_NOT
// (docs/mechanisms/query-operators.md): SYNONYM, MAX, XOR, SCALE_WEIGHT, ELITE_SET, and a
// scorer over a host value source. Each is a prospect::scorer behind the existing interface.
// Semantics are pinned by Xapian's recorded vectors (strata-oracle, `ops`).

#pragma once

#include "prospect/reader.hh"
#include "prospect/search.hh"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace prospect {

// --- SYNONYM ------------------------------------------------------------------------------------

// The size SYNONYM gives its terms as one: a union of independent sets, N (1 - prod (1 - df/N)),
// rounded to the nearest document. One term keeps its own df.
inline std::uint64_t synonym_df(std::span<const std::uint64_t> dfs, std::uint64_t docs) noexcept {
	if (dfs.size() == 1) { return dfs[0]; }
	if (docs == 0) { return 0; }
	double miss = 1.0;
	for (auto df : dfs) { miss *= 1.0 - double(df) / double(docs); }
	return std::uint64_t(std::floor(double(docs) * (1.0 - miss) + 0.5));
}

// Terms counted as one: a document matches when it holds any term; its wdf is the terms' wdfs
// summed, capped at the document's length, and the weight is one term's weight at the union's
// estimated df (synonym_df).
class synonym_scorer final : public scorer {
public:
	synonym_scorer(const inverted_plugin::reader& seg, std::span<const format::term_info> infos, const weight& w) : seg_(&seg), infos_(infos.begin(), infos.end()), weight_(w) {
		std::uint64_t wdf_sum = 0;
		std::uint8_t shortest = 255;
		for (const auto& info : infos_) {
			its_.push_back(std::make_unique<postings_iterator>(seg, info));
			cost_ += info.df;
			std::uint32_t best = 0;
			for (const auto& p : info.impacts) {
				best = std::max(best, p.wdf);
				shortest = std::min(shortest, p.length);
			}
			wdf_sum += best;
		}
		// The weight rises with wdf and falls with length, and wdf is capped at the length, so
		// the best case is the length equal to the summed wdf, or the shortest length when
		// that is longer; widened a hair for the Lucene preset's length buckets.
		const std::uint32_t most = std::uint32_t(std::min<std::uint64_t>(wdf_sum, 0xffffffffu)), least = std::max<std::uint32_t>(1, format::decode_length(shortest));
		bound_ = infos_.empty() ? 0.0 : (most >= least ? weight_(most, most) : weight_(most, least)) * (1.0 + 0x1p-40);
	}

	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override {
		if (started_ && doc_ == no_more_docs) { return doc_; }
		if (!started_) {
			started_ = true;
			for (auto& it : its_) { it->next(); }
		} else {
			for (auto& it : its_) {
				if (it->doc() == doc_) { it->next(); }
			}
		}
		return doc_ = smallest();
	}
	ordinal_t advance(ordinal_t t) override {
		for (auto& it : its_) {
			if (!started_ || it->doc() < t) { it->advance(t); }
		}
		started_ = true;
		return doc_ = smallest();
	}
	std::uint64_t cost() const noexcept override { return cost_; }
	double score() override {
		std::uint64_t wdf = 0;
		for (auto& it : its_) {
			if (it->doc() == doc_) { wdf += it->wdf(); }
		}
		const std::uint32_t length = seg_->length(doc_);
		const std::uint32_t tf = std::uint32_t(std::min<std::uint64_t>(wdf, length));
		return weight_.kind == scheme::bm25 ? weight_.by_norm(tf, seg_->norm(doc_)) : weight_(tf, length);
	}
	double max_score(ordinal_t, ordinal_t) override { return bound_; }
	ordinal_t block_end(ordinal_t t) override {
		ordinal_t e = no_more_docs;
		for (auto& it : its_) { e = std::min(e, it->block_end(t)); }
		return e;
	}
	double max_score_total() const noexcept override { return bound_; }
	std::size_t score_terms() const noexcept override { return 1; }
	std::unique_ptr<scorer> fresh() const override { return std::make_unique<synonym_scorer>(*seg_, infos_, weight_); }

private:
	ordinal_t smallest() const noexcept {
		ordinal_t d = no_more_docs;
		for (const auto& it : its_) { d = std::min(d, it->doc()); }
		return d;
	}

	const inverted_plugin::reader* seg_;
	std::vector<format::term_info> infos_;
	std::vector<std::unique_ptr<postings_iterator>> its_;
	weight weight_;
	ordinal_t doc_ = 0;
	std::uint64_t cost_ = 0;
	double bound_ = 0;
	bool started_ = false;
};

// --- MAX and XOR --------------------------------------------------------------------------------

// Children merged in document order, each document kept or dropped by how many children hold
// it. A linear scan of the children per step: these operators carry a handful of them.
class merging_scorer : public scorer {
public:
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override {
		if (started_ && doc_ == no_more_docs) { return doc_; }
		if (!started_) {
			started_ = true;
			for (auto& c : children_) { c->next(); }
		} else {
			step_past(doc_);
		}
		return doc_ = settle();
	}
	ordinal_t advance(ordinal_t t) override {
		for (auto& c : children_) {
			if (!started_ || c->doc() < t) { c->advance(t); }
		}
		started_ = true;
		return doc_ = settle();
	}
	std::uint64_t cost() const noexcept override { return cost_; }
	ordinal_t block_end(ordinal_t t) override {
		ordinal_t e = no_more_docs;
		for (auto& c : children_) { e = std::min(e, c->block_end(t)); }
		return e;
	}
	std::size_t score_terms() const noexcept override { return terms_total(children_); }

protected:
	explicit merging_scorer(std::vector<scorer_ptr> children) : children_(std::move(children)) {
		for (const auto& c : children_) { cost_ += c->cost(); }
	}
	// Whether a document held by `holding` children matches.
	virtual bool keeps(std::size_t holding) const noexcept = 0;

	// Unpositioned copies of the children, or false when one cannot be copied.
	bool clone_children(std::vector<scorer_ptr>& out) const {
		for (const auto& c : children_) {
			auto f = c->fresh();
			if (!f) { return false; }
			out.push_back(std::move(f));
		}
		return true;
	}

	std::vector<scorer_ptr> children_;

private:
	void step_past(ordinal_t d) {
		for (auto& c : children_) {
			if (c->doc() == d) { c->next(); }
		}
	}
	ordinal_t settle() {
		for (;;) {
			ordinal_t d = no_more_docs;
			for (const auto& c : children_) { d = std::min(d, c->doc()); }
			if (d == no_more_docs) { return d; }
			std::size_t holding = 0;
			for (const auto& c : children_) { holding += c->doc() == d; }
			if (keeps(holding)) { return d; }
			step_past(d);
		}
	}

	ordinal_t doc_ = 0;
	std::uint64_t cost_ = 0;
	bool started_ = false;
};

// Documents any child matches; the score is the best child's (Xapian OP_MAX).
class max_scorer final : public merging_scorer {
public:
	explicit max_scorer(std::vector<scorer_ptr> children) : merging_scorer(std::move(children)) {
		for (const auto& c : children_) { bound_ = std::max(bound_, c->max_score_total()); }
	}
	double score() override {
		double best = 0.0;
		const ordinal_t d = doc();
		for (auto& c : children_) {
			if (c->doc() == d) { best = std::max(best, c->score()); }
		}
		return best;
	}
	double max_score(ordinal_t from, ordinal_t upto) override {
		double best = 0.0;
		for (auto& c : children_) { best = std::max(best, c->max_score(from, upto)); }
		return best;
	}
	double max_score_total() const noexcept override { return bound_; }
	// Every child's own score for the document it holds is at most the document's, so a child
	// may skip what cannot compete.
	void set_min_competitive_score(double s) override {
		for (auto& c : children_) { c->set_min_competitive_score(s); }
	}
	// A match scores at least what its best child's competitive pair scores: the larger seed.
	double seed(std::size_t k) override {
		double s = 0.0;
		for (const auto& c : children_) { s = std::max(s, c->seed(k)); }
		return s;
	}
	std::size_t score_terms() const noexcept override {
		std::size_t n = 0;
		for (const auto& c : children_) { n = std::max(n, c->score_terms()); }
		return n;
	}
	std::unique_ptr<scorer> fresh() const override {
		std::vector<scorer_ptr> kids;
		return clone_children(kids) ? std::make_unique<max_scorer>(std::move(kids)) : nullptr;
	}

private:
	bool keeps(std::size_t) const noexcept override { return true; }
	double bound_ = 0.0;
};

// Documents an odd number of children match; the score is the matching children's, summed
// (Xapian OP_XOR, n-ary: a nested XOR is the same operator, so the constructor splices the
// children of any unstarted child XOR into its own).
class xor_scorer final : public merging_scorer {
public:
	explicit xor_scorer(std::vector<scorer_ptr> children) : merging_scorer(splice(std::move(children))) {
		for (const auto& c : children_) { bound_ += c->max_score_total(); }
	}
	double score() override {
		double sum = 0.0;
		const ordinal_t d = doc();
		for (auto& c : children_) {
			if (c->doc() == d) { sum += c->score(); }
		}
		return sum;
	}
	double max_score(ordinal_t from, ordinal_t upto) override {
		double sum = 0.0;
		for (auto& c : children_) { sum += c->max_score(from, upto); }
		return sum;
	}
	double max_score_total() const noexcept override { return bound_; }
	std::unique_ptr<scorer> fresh() const override {
		std::vector<scorer_ptr> kids;
		return clone_children(kids) ? std::make_unique<xor_scorer>(std::move(kids)) : nullptr;
	}

private:
	static std::vector<scorer_ptr> splice(std::vector<scorer_ptr> in) {
		std::vector<scorer_ptr> out;
		for (auto& c : in) {
			if (auto* x = dynamic_cast<xor_scorer*>(c.get())) {
				auto inner = splice(std::move(x->children_));
				for (auto& g : inner) { out.push_back(std::move(g)); }
			} else {
				out.push_back(std::move(c));
			}
		}
		return out;
	}
	bool keeps(std::size_t holding) const noexcept override { return holding % 2 == 1; }
	double bound_ = 0.0;
};

// --- SCALE_WEIGHT -------------------------------------------------------------------------------

// The child's documents with scores multiplied by a factor (Xapian OP_SCALE_WEIGHT). A factor of
// zero keeps the matches and drops the weight: FILTER(a, b) is AND(a, scale(b, 0)).
class scale_scorer final : public scorer {
public:
	// Throws std::invalid_argument for a factor below zero or not a number, as Xapian refuses.
	scale_scorer(scorer_ptr child, double factor) : child_(std::move(child)), factor_(factor) {
		if (!(factor >= 0.0)) { throw std::invalid_argument("scale_scorer: the factor must be >= 0"); }
	}
	ordinal_t doc() const noexcept override { return child_->doc(); }
	ordinal_t next() override { return child_->next(); }
	ordinal_t advance(ordinal_t t) override { return child_->advance(t); }
	bool two_phase() const noexcept override { return child_->two_phase(); }
	ordinal_t approx_next() override { return child_->approx_next(); }
	ordinal_t approx_advance(ordinal_t t) override { return child_->approx_advance(t); }
	bool matches() override { return child_->matches(); }
	std::uint64_t cost() const noexcept override { return child_->cost(); }
	double score() override { return scaled(child_->score()); }
	double max_score(ordinal_t from, ordinal_t upto) override { return scaled(child_->max_score(from, upto)); }
	ordinal_t block_end(ordinal_t t) override { return child_->block_end(t); }
	double max_score_total() const noexcept override { return scaled(child_->max_score_total()); }
	std::size_t score_terms() const noexcept override { return child_->score_terms(); }
	// A score above s needs a child score above s / factor, lowered past the rounding of both.
	void set_min_competitive_score(double s) override {
		if (factor_ > 0.0 && s > 0.0) { child_->set_min_competitive_score(std::nextafter(std::nextafter(s / factor_, 0.0), 0.0)); }
	}
	double current_bound() override { return scaled(child_->current_bound()); }
	std::optional<std::uint64_t> exact_count() override { return child_->exact_count(); }
	bool matches_all() const noexcept override { return child_->matches_all(); }
	std::unique_ptr<scorer> fresh() const override {
		auto f = child_->fresh();
		return f ? std::make_unique<scale_scorer>(std::move(f), factor_) : nullptr;
	}
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override { child_->fill(base, end, bits); }
	double seed(std::size_t k) override { return scaled(child_->seed(k)); }

private:
	// Zero times an unbounded score is zero, not a number.
	double scaled(double x) const noexcept { return factor_ == 0.0 ? 0.0 : x * factor_; }
	scorer_ptr child_;
	double factor_;
};

// --- ELITE_SET ----------------------------------------------------------------------------------

// A subquery and what ranks it for ELITE_SET: a term's weight (weight::idf, wqf included).
struct elite_child {
	scorer_ptr scorer;
	double key;
};

// What ELITE_SET keeps when no size is given.
inline constexpr std::size_t default_elite_size = 10;

// The `size` children with the greatest keys, ORed (Xapian OP_ELITE_SET); a size of 0 is
// default_elite_size and one at or above the count keeps all. Equal keys keep the earlier child
// (Xapian's pick among them follows no order of the query), and the kept children stay in query
// order, so a document's score sums the same way in every segment. Null for no children.
inline scorer_ptr elite_set(std::vector<elite_child> children, std::size_t size = 0) {
	if (children.empty()) { return nullptr; }
	if (size == 0) { size = default_elite_size; }
	std::vector<scorer_ptr> kept;
	if (children.size() > size) {
		std::vector<std::size_t> order(children.size());
		for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
		std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return children[a].key > children[b].key; });
		order.resize(size);
		std::sort(order.begin(), order.end());
		for (auto i : order) { kept.push_back(std::move(children[i].scorer)); }
	} else {
		for (auto& c : children) { kept.push_back(std::move(c.scorer)); }
	}
	if (kept.size() == 1) { return std::move(kept[0]); }
	return std::make_unique<disjunction_scorer>(std::move(kept));
}

// --- A host value source ------------------------------------------------------------------------

// --- PHRASE and NEAR over alternatives ---------------------------------------------------------

// One position of a phrase that holds alternatives (a term's wildcard or edit-distance expansion,
// an OR or SYNONYM of terms): the documents holding any alternative, scored as that query scores
// them, and the alternatives' terms, whose positions fill the position.
struct phrase_slot {
	scorer_ptr scorer;
	std::vector<format::term_info> terms;
};

// Lucene's MultiPhraseQuery and Xapian's phrase over expansions: documents where the slots occur at
// their relative positions (consecutive by default; `offsets`, one per slot, leave gaps where words
// were removed), a slot being met by any of its terms. With window > 0 it is NEAR: one distinct
// position per slot, in any order, spanning fewer than window positions. A document scores the sum of
// its slots' scores, in the order given.
class multi_phrase_scorer final : public scorer {
public:
	multi_phrase_scorer(const inverted_plugin::reader& seg, std::vector<phrase_slot> slots, std::uint32_t window = 0, std::span<const std::uint32_t> offsets = {})
		: window_(window), cursors_(slots.size()), positions_(slots.size()) {
		if (slots.empty()) { throw std::invalid_argument("strata: a phrase needs a slot"); }
		if (!offsets.empty() && offsets.size() != slots.size()) { throw std::invalid_argument("strata: one offset per phrase slot"); }
		offsets_.resize(slots.size());
		for (std::size_t k = 0; k < slots.size(); ++k) { offsets_[k] = offsets.empty() ? std::uint32_t(k) : offsets[k] - offsets[0]; }
		std::vector<scorer_ptr> children;
		children.reserve(slots.size());
		for (std::size_t k = 0; k < slots.size(); ++k) {
			if (!slots[k].scorer) { throw std::invalid_argument("strata: a phrase slot needs a scorer"); }
			children.push_back(std::move(slots[k].scorer));
			for (const auto& info : slots[k].terms) {
				cursors_[k].push_back({std::make_unique<postings_iterator>(seg, info), std::make_unique<positions_reader>(seg, info)});
			}
		}
		all_ = std::make_unique<conjunction_scorer>(std::move(children));
	}

	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = confirm(all_->next()); }
	ordinal_t advance(ordinal_t t) override { return doc_ = confirm(all_->advance(t)); }
	bool two_phase() const noexcept override { return true; }
	ordinal_t approx_next() override { return doc_ = all_->next(); }
	ordinal_t approx_advance(ordinal_t t) override { return doc_ = all_->advance(t); }
	bool matches() override {
		if (checked_ != doc_) {
			checked_ = doc_;
			matched_ = occurs(doc_);
		}
		return matched_;
	}
	std::uint64_t cost() const noexcept override { return all_->cost(); }
	double score() override { return all_->score(); }
	double max_score(ordinal_t from, ordinal_t upto) override { return all_->max_score(from, upto); }
	ordinal_t block_end(ordinal_t t) override { return all_->block_end(t); }
	double max_score_total() const noexcept override { return all_->max_score_total(); }
	std::size_t score_terms() const noexcept override { return all_->score_terms(); }

private:
	struct cursor {
		std::unique_ptr<postings_iterator> postings;
		std::unique_ptr<positions_reader> reader;
		bool begun = false;
	};

	ordinal_t confirm(ordinal_t d) {
		while (d != no_more_docs) {
			checked_ = d;
			doc_ = d;
			if ((matched_ = occurs(d))) { return d; }
			d = all_->next();
		}
		return d;
	}

	// Whether the phrase occurs in document d, which every slot's scorer holds.
	bool occurs(ordinal_t d) {
		for (std::size_t k = 0; k < cursors_.size(); ++k) {
			auto& at = positions_[k];
			at.clear();
			std::size_t sources = 0;
			for (auto& c : cursors_[k]) {
				if (!c.begun || c.postings->doc() < d) {
					c.begun = true;
					c.postings->advance(d);
				}
				if (c.postings->doc() != d) { continue; }
				const auto read = c.reader->read(*c.postings);
				at.insert(at.end(), read.begin(), read.end());
				sources += !read.empty();
			}
			if (at.empty()) { return false; }
			if (sources > 1) {
				std::sort(at.begin(), at.end());
				at.erase(std::unique(at.begin(), at.end()), at.end());
			}
		}
		return window_ ? near() : exact();
	}

	// Some start from which every slot has a position at its offset.
	bool exact() const {
		std::size_t fewest = 0;
		for (std::size_t k = 1; k < positions_.size(); ++k) {
			if (positions_[k].size() < positions_[fewest].size()) { fewest = k; }
		}
		for (const std::uint32_t p : positions_[fewest]) {
			if (p < offsets_[fewest]) { continue; }
			const std::uint32_t start = p - offsets_[fewest];
			bool all = true;
			for (std::size_t k = 0; k < positions_.size() && all; ++k) {
				all = k == fewest || std::binary_search(positions_[k].begin(), positions_[k].end(), start + offsets_[k]);
			}
			if (all) { return true; }
		}
		return false;
	}

	// Some window of fewer than window_ positions that gives every slot a position of its own.
	bool near() {
		span_.clear();
		for (const auto& at : positions_) { span_.insert(span_.end(), at.begin(), at.end()); }
		std::sort(span_.begin(), span_.end());
		span_.erase(std::unique(span_.begin(), span_.end()), span_.end());
		if (span_.size() < positions_.size()) { return false; }
		for (std::size_t lo = 0; lo + positions_.size() <= span_.size(); ++lo) {
			std::size_t hi = lo;
			while (hi < span_.size() && span_[hi] - span_[lo] < window_) { ++hi; }
			if (hi - lo < positions_.size()) { continue; }
			owner_.assign(hi - lo, none);
			bool all = true;
			for (std::size_t k = 0; k < positions_.size() && all; ++k) {
				seen_.assign(hi - lo, 0);
				all = assign(k, lo, hi);
			}
			if (all) { return true; }
		}
		return false;
	}

	// Gives slot k a position of the window [lo, hi) that is free, or takes one from a slot that can use another.
	bool assign(std::size_t k, std::size_t lo, std::size_t hi) {
		for (std::size_t j = lo; j < hi; ++j) {
			if (seen_[j - lo] || !std::binary_search(positions_[k].begin(), positions_[k].end(), span_[j])) { continue; }
			seen_[j - lo] = 1;
			if (owner_[j - lo] == none || assign(owner_[j - lo], lo, hi)) {
				owner_[j - lo] = k;
				return true;
			}
		}
		return false;
	}

	static constexpr std::size_t none = ~std::size_t(0);
	std::unique_ptr<conjunction_scorer> all_;
	std::uint32_t window_;
	std::vector<std::uint32_t> offsets_;
	std::vector<std::vector<cursor>> cursors_;
	std::vector<std::vector<std::uint32_t>> positions_;   // each slot's positions in the current document, ascending
	std::vector<std::uint32_t> span_;
	std::vector<std::size_t> owner_;
	std::vector<std::uint8_t> seen_;
	ordinal_t doc_ = 0, checked_ = no_more_docs;
	bool matched_ = false;
};

// A column of opaque values keyed by ordinal: the values plugin's reader in a segment. Not every
// ordinal holds a value.
template <typename S>
concept slot_values = std::copy_constructible<S> && requires(const S s, ordinal_t o) {
	{ s.next_present(o) } -> std::same_as<ordinal_t>;       // the first ordinal >= o holding a value, or no_more_docs
	{ s.value(o) } -> std::convertible_to<std::string_view>;  // the bytes at an ordinal that holds one
	{ s.count() } -> std::convertible_to<std::uint64_t>;    // how many ordinals hold a value
};

// Xapiand's PostingSource: a test of one slot's value and a weight for the values that pass.
// accept and weight are const and thread-safe; accept is false for a value that does not decode.
template <typename S>
concept value_source = requires(const S s, std::string_view v) {
	{ s.accept(v) } -> std::convertible_to<bool>;
	{ s.weight(v) } -> std::convertible_to<double>;
	{ s.max_weight() } -> std::convertible_to<double>;  // 0 when the source does not say
};

// The documents whose value in a slot the source accepts, scored by the source's weight. A
// column scan: it steps over the slot's present ordinals, so it costs the slot's count.
// A source that does not give max_weight (0) may weigh anything, so nothing is pruned on it.
template <slot_values Slot, value_source Source>
class source_scorer final : public scorer {
public:
	source_scorer(Slot slot, const Source& source) : slot_(std::move(slot)), source_(&source) {
		const double m = source.max_weight();
		bound_ = m > 0.0 ? m : std::numeric_limits<double>::infinity();
	}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = (started_ && doc_ == no_more_docs ? no_more_docs : scan(started_ ? doc_ + 1 : 0)); }
	ordinal_t advance(ordinal_t t) override {
		if (started_ && doc_ >= t) { return doc_; }
		return doc_ = scan(t);
	}
	std::uint64_t cost() const noexcept override { return slot_.count(); }
	double score() override { return source_->weight(std::string_view(slot_.value(doc_))); }
	double max_score(ordinal_t, ordinal_t) override { return bound_; }
	ordinal_t block_end(ordinal_t) override { return no_more_docs; }
	double max_score_total() const noexcept override { return bound_; }
	std::size_t score_terms() const noexcept override { return 1; }
	std::unique_ptr<scorer> fresh() const override { return std::make_unique<source_scorer>(slot_, *source_); }

private:
	ordinal_t scan(ordinal_t from) {
		started_ = true;
		for (ordinal_t d = slot_.next_present(from); d != no_more_docs; d = slot_.next_present(d + 1)) {
			if (source_->accept(std::string_view(slot_.value(d)))) { return d; }
		}
		return no_more_docs;
	}

	Slot slot_;
	const Source* source_;
	double bound_;
	ordinal_t doc_ = 0;
	bool started_ = false;
};

}  // namespace prospect
