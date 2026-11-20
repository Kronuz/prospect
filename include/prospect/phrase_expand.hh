// PHRASE and NEAR over words that expand (docs/mechanisms/phrase-expansion.md): each prefix,
// wildcard or fuzzy word of a phrase expands once over the snapshot through expand.hh, and the
// phrase matches any expansion at the word's position through multi_phrase_scorer. The expansion
// is bounded per word and per query (max_expansions), and a phrase past a bound throws
// phrase_expansion_error naming the word.
#pragma once

#include "automata.hh"
#include "prospect/expand.hh"
#include "prospect/operators.hh"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prospect {

// One word of a phrase: a term as it is, or a pattern that expands over the field's terms.
struct phrase_word {
	enum shape { literal, prefix, glob, fuzzy } how = literal;
	std::string text;                                      // the term, prefix, glob pattern or fuzzy target
	unsigned glob_flags = automata::glob::multi | automata::glob::single;
	std::uint64_t distance = 1;                            // fuzzy: edits allowed
	std::size_t fixed = 0;                                 // fuzzy: leading characters that must match
	bool swaps = true;                                     // fuzzy: a transposition is one edit
	expansion_limit limit;                                 // this word's own bound (max 0: none)

	static phrase_word term(std::string t) { return {literal, std::move(t)}; }
	static phrase_word starts_with(std::string p) { return {prefix, std::move(p)}; }
	static phrase_word wildcard(std::string pattern, unsigned flags = automata::glob::multi | automata::glob::single) {
		phrase_word w{glob, std::move(pattern)};
		w.glob_flags = flags;
		return w;
	}
	static phrase_word similar(std::string target, std::uint64_t distance, std::size_t fixed = 0, bool swaps = true) {
		phrase_word w{fuzzy, std::move(target)};
		w.distance = distance;
		w.fixed = fixed;
		w.swaps = swaps;
		return w;
	}
};

// How a slot's terms score: as an OR (the sum of each term's own weight) or as a SYNONYM (one
// weight at the union's estimated df).
enum class slot_scoring { any, synonym };

struct phrase_options {
	std::uint64_t max_expansions = 0;  // the terms all words' expansions select together (0: none); literal words are not counted
	walk_options walk;                 // cancellation, slicing and the byte budget of the walks
	slot_scoring scoring = slot_scoring::synonym;
	std::function<weight(std::uint64_t df)> weigh;  // a term's weight at a df; empty: bm25_xapian over the table's statistics
};

// A word's expansion selected more terms than its own limit or than the query's. `word` indexes
// the phrase's words; `max` is the bound that was passed, `per_query` says which one.
struct phrase_expansion_error : std::runtime_error {
	phrase_expansion_error(std::size_t w, bool query, std::uint64_t limit)
		: std::runtime_error("prospect: phrase word " + std::to_string(w) + " expands past " + (query ? "the query's max_expansions of " : "its limit of ") + std::to_string(limit) + " terms"),
		  word(w),
		  max(limit),
		  per_query(query) {}
	std::size_t word;
	std::uint64_t max;
	bool per_query;
};

// A phrase's words expanded over a snapshot's field, ready to build its scorer in each segment.
// The weights are fixed here, once per slot, so a segment scores as the snapshot does. The table
// and the options' callbacks must outlive it, and scorer() is not thread-safe (it completes the
// table's columns).
class expanded_phrase {
public:
	// Throws phrase_expansion_error past a bound, std::invalid_argument for the key field (it has
	// no positions) or an empty phrase, and what the walks throw (task::cancelled, a charge).
	expanded_phrase(std::span<const segment_snapshot> segs, std::string_view field, std::span<const phrase_word> words, term_table& table, const phrase_options& o = {})
		: field_(field), table_(&table), how_(o.scoring), charge_(o.walk.charge) {
		if (words.empty()) { throw std::invalid_argument("prospect: a phrase needs a word"); }
		for (const auto& s : segs) {
			if (s.key_field() && s.key_field() == s.field_id(field)) { throw std::invalid_argument("prospect: a phrase takes a field with positions, not the key field"); }
		}
		const auto weigh = o.weigh ? o.weigh : [&table](std::uint64_t df) { return weight::bm25_xapian(table.stats(), df, 1); };
		std::optional<std::uint64_t> left;  // the query's terms still allowed
		if (o.max_expansions) { left = o.max_expansions; }
		for (std::size_t i = 0; i < words.size(); ++i) {
			std::vector<std::string> terms;
			if (words[i].how == phrase_word::literal) {
				if (table.df(field, words[i].text)) { terms.push_back(words[i].text); }
			} else {
				terms = expand_word(segs, i, words[i], left, o.max_expansions, o.walk);
				if (left) { *left -= terms.size(); }
			}
			std::vector<std::uint64_t> dfs;
			for (const auto& t : terms) { dfs.push_back(table.df(field, t)); }
			std::vector<weight> w;
			if (how_ == slot_scoring::synonym) {
				w.push_back(weigh(synonym_df(dfs, table.stats().doc_count)));
			} else {
				for (const auto df : dfs) { w.push_back(weigh(df)); }
			}
			slots_.push_back(std::move(terms));
			weights_.push_back(std::move(w));
		}
	}

	// Each word's selected terms, in term order: the terms its position accepts.
	const std::vector<std::vector<std::string>>& terms() const noexcept { return slots_; }

	// The phrase in one segment of the snapshot: consecutive words, or with `offsets` (one per
	// word) the gaps where words were removed; with window > 0 it is NEAR. Null when a word has
	// no term in the segment, so the segment matches nothing.
	scorer_ptr scorer(const segment& seg, std::uint32_t window = 0, std::span<const std::uint32_t> offsets = {}) const {
		const auto& inv = seg.get<inverted_plugin>();
		std::vector<phrase_slot> slots(slots_.size());
		std::vector<std::vector<std::size_t>> weight_of(slots_.size());  // each present term's place in weights_
		std::uint64_t bytes = 0;
		for (std::size_t k = 0; k < slots_.size(); ++k) {
			for (std::size_t j = 0; j < slots_[k].size(); ++j) {
				if (const auto* info = table_->info(seg, field_, slots_[k][j])) {
					slots[k].terms.push_back(*info);
					weight_of[k].push_back(j);
					bytes += term_bytes(*info);
				}
			}
			if (slots[k].terms.empty()) { return nullptr; }
		}
		if (charge_) { charge_(bytes); }  // before the iterators are allocated, as the walks charge
		for (std::size_t k = 0; k < slots.size(); ++k) {
			auto& one = slots[k];
			if (how_ == slot_scoring::synonym) {
				one.scorer = std::make_unique<synonym_scorer>(inv, one.terms, weights_[k][0]);
				continue;
			}
			std::vector<scorer_ptr> kids;
			for (std::size_t j = 0; j < one.terms.size(); ++j) { kids.push_back(std::make_unique<term_scorer>(inv, one.terms[j], weights_[k][weight_of[k][j]])); }
			if (kids.size() == 1) {
				one.scorer = std::move(kids[0]);
			} else {
				one.scorer = std::make_unique<disjunction_scorer>(std::move(kids));
			}
		}
		return std::make_unique<multi_phrase_scorer>(inv, std::move(slots), window, offsets);
	}

	// The scorer_builder of a search over the snapshot (search.hh's `search`).
	scorer_builder builder(std::uint32_t window = 0, std::vector<std::uint32_t> offsets = {}) const {
		return [this, window, offsets = std::move(offsets)](const segment& seg) { return scorer(seg, window, offsets); };
	}

private:
	// What one term costs in one segment's scorer: the slot's copy of its info, the phrase's cursor
	// (a postings iterator and a positions reader) and the slot scorer's own iterator and copies.
	std::uint64_t term_bytes(const format::term_info& info) const noexcept {
		const std::uint64_t impacts = info.impacts.size() * sizeof(format::impact), copy = sizeof(format::term_info) + impacts;
		const std::uint64_t cursor = sizeof(postings_iterator) + impacts + sizeof(positions_reader) + 3 * sizeof(void*);
		const std::uint64_t mine = how_ == slot_scoring::synonym ? sizeof(postings_iterator) + impacts + copy + sizeof(void*) : sizeof(term_scorer) + impacts + sizeof(void*);
		return copy + cursor + mine + 256;  // 256: the vectors' growth and the allocator's rounding
	}

	// One word's terms: expand under the word's own limit, tightened to what the query has left
	// (`left`, of `total`). Under the query's bound the word's limit is `error` whatever its type: a
	// result larger than `left` is refused either way, so the walk stops at the first term past it.
	std::vector<std::string> expand_word(std::span<const segment_snapshot> segs, std::size_t index, const phrase_word& w, std::optional<std::uint64_t> left, std::uint64_t total, const walk_options& walk) {
		expansion_limit eff = w.limit;
		const std::uint64_t own = w.limit.max ? w.limit.max : ~std::uint64_t(0);
		const std::uint64_t room = left ? *left : ~std::uint64_t(0);
		const bool query = room < own;
		if (query) { eff = {expansion_limit::error, std::max<std::uint64_t>(room, 1)}; }
		std::vector<std::string> out;
		try {
			switch (w.how) {
			case phrase_word::prefix: out = expand(segs, field_, automata::prefix(w.text), eff, *table_, walk); break;
			case phrase_word::glob: out = expand(segs, field_, automata::glob(w.text, w.glob_flags, walk.charge), eff, *table_, walk); break;
			default: out = expand(segs, field_, automata::levenshtein(w.text, w.distance, w.fixed, w.swaps, walk.charge), eff, *table_, walk); break;
			}
		} catch (const expansion_error& e) {
			throw phrase_expansion_error(index, query, query ? total : e.max);
		}
		if (left && out.size() > *left) { throw phrase_expansion_error(index, true, total); }
		return out;
	}

	std::string field_;
	term_table* table_;
	slot_scoring how_;
	automata::charge_fn charge_;  // the walks' byte budget, offered the scorers' bytes too
	std::vector<std::vector<std::string>> slots_;
	std::vector<std::vector<weight>> weights_;
};

}  // namespace prospect
