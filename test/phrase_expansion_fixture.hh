// A keyed index of positional documents and the brute-force evaluation of a phrase over expanding
// words, for the phrase-expansion tests and the fuzz target (docs/mechanisms/phrase-expansion.md).
// The world has several segments, replaced and deleted documents (terms held only by dead
// documents), a large positional field with its own dictionaries ("text") and a small one in the
// pooled dictionary ("title"), and the key field. The oracle works from the model's tokens alone,
// with automata's reference predicates; it reads the snapshot only for the document frequencies
// weights take (deleted documents counted, as everywhere).
#pragma once

#include "automata_reference.hh"
#include "prospect/index.hh"
#include "prospect/phrase_expand.hh"
#include "prospect/searcher.hh"
#include "strata/core/faultfs.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace phrase_fixture {

using namespace prospect;
namespace faultfs = strata::faultfs;

inline const char* const text_name = "text";
inline const char* const title_name = "title";
inline const char* const key_name = "_id";

inline std::vector<strata::field_declaration> fields() {
	return {{text_name, strata::field_role::indexed, strata::field_postings::positions, false},
		{title_name, strata::field_role::indexed, strata::field_postings::positions, false},
		{key_name, strata::field_role::key, strata::field_postings::docs, false}};
}

// Pieces of terms: ASCII, 2-, 3- and 4-byte characters, lone and aliasing bytes.
inline const std::vector<std::string>& pieces() {
	static const std::vector<std::string> p = {"a", "b", "c", "o", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9D\x84\x9E", "\xE2", "\xC3\xA2", "\xC3"};
	return p;
}

inline std::string random_term(std::mt19937_64& r, std::size_t max_pieces) {
	std::string s;
	for (std::size_t i = 0, n = 1 + r() % max_pieces; i < n; ++i) { s += pieces()[r() % pieces().size()]; }
	return s;
}

struct doc {
	std::uint64_t docid = 0;
	std::vector<std::string> text, title;  // token i is at position i + 1
	const std::vector<std::string>& of(std::string_view field) const { return field == text_name ? text : title; }
};
using model = std::map<std::string, doc>;  // live documents by key

class world {
public:
	// `text_pool` is the hot vocabulary phrases are built from; `tail_pool` is a long tail of terms that
	// gives a segment's text field the terms for its own dictionaries (fielddict::own_threshold).
	world(std::uint64_t seed, std::size_t docs, std::size_t text_terms = 36, std::size_t title_terms = 10, std::size_t tail_terms = 800, std::size_t refresh_every = 400) : io_(fs_), r_(seed) {
		prospect::index::options opts;
		opts.keyed = true;
		ix_.emplace(prospect::index::create(io_, opts, {}));
		while (text_pool.size() < text_terms) { text_pool.push_back(random_term(r_, 4)); }
		while (tail_pool.size() < tail_terms) { tail_pool.push_back(random_term(r_, 5)); }
		while (title_pool.size() < title_terms) { title_pool.push_back(random_term(r_, 2)); }
		for (std::size_t i = 0; i < docs / 5 + 5; ++i) { keys_.push_back("k" + std::to_string(i)); }
		auto skewed = [&](const std::vector<std::string>& pool) { return pool[std::min(r_() % pool.size(), r_() % pool.size())]; };
		for (std::size_t d = 0; d < docs; ++d) {
			const std::string& key = keys_[r_() % keys_.size()];
			doc m;
			for (std::size_t i = 0, n = 3 + r_() % 10; i < n; ++i) { m.text.push_back(r_() % 2 == 0 ? tail_pool[r_() % tail_pool.size()] : skewed(text_pool)); }
			for (std::size_t i = 0, n = r_() % 4; i < n; ++i) { m.title.push_back(skewed(title_pool)); }
			m.docid = put(key, m);
			live[key] = std::move(m);
			if (r_() % refresh_every == 0) { ix_->refresh(); }
			if (r_() % 9 == 0 && !live.empty()) {  // deletes, some of documents holding terms nobody else holds
				auto it = live.begin();
				std::advance(it, std::ptrdiff_t(r_() % live.size()));
				operation del;
				del.type = operation::delete_key;
				del.key = it->first;
				apply(std::move(del));
				live.erase(it);
			}
			if (r_() % 120 == 0) {
				const std::string key2 = "dead" + std::to_string(d);
				doc gone;
				gone.text = {"dead" + random_term(r_, 3), "dead" + random_term(r_, 3)};
				put(key2, gone);
				operation del;
				del.type = operation::delete_key;
				del.key = key2;
				apply(std::move(del));
			}
		}
		ix_->refresh();
		for (const auto& v : ix_->segments()) { segs.push_back({v.seg(), v.deleted, v.dead, &ix_->fields()}); }
		if (segs.empty()) { throw std::runtime_error("phrase_fixture: no segments"); }
	}
	world(const world&) = delete;
	world& operator=(const world&) = delete;

	model live;
	std::vector<std::string> text_pool, title_pool, tail_pool;
	std::vector<segment_snapshot> segs;

	// Segments with a deleted document, and fields in each dictionary shape.
	std::size_t with_deletes() const {
		std::size_t n = 0;
		for (const auto& s : segs) { n += s.any_deleted(); }
		return n;
	}
	std::size_t own_dictionaries() const { return count(true); }
	std::size_t pooled_dictionaries() const { return count(false); }

private:
	std::size_t count(bool own) const {
		std::size_t n = 0;
		for (const auto& s : segs) {
			for (const char* f : {text_name, title_name}) {
				const auto id = s.field_id(f);
				if (!id || !s.seg->get<inverted_plugin>().has_field(*id)) { continue; }
				n += (s.seg->get<inverted_plugin>().dictionary().entry_of(*id).has_value()) == own;
			}
		}
		return n;
	}

	std::uint64_t apply(operation op) {
		transaction t;
		t.fields = fields();
		t.ops.push_back(std::move(op));
		const auto v = ix_->apply(t, {});
		if (v.result != strata::verdict::accepted) { throw std::runtime_error("phrase_fixture: a transaction was refused"); }
		return v.docids.empty() ? 0 : v.docids[0];
	}
	std::uint64_t put(const std::string& key, const doc& m) {
		operation op;
		op.type = operation::replace_key;
		op.key = key;
		op.doc.key = key;
		std::uint32_t length = 0;
		for (int f = 0; f < 2; ++f) {
			const auto& tokens = f == 0 ? m.text : m.title;
			std::map<std::string, std::vector<std::uint32_t>> at;
			for (std::size_t i = 0; i < tokens.size(); ++i) { at[tokens[i]].push_back(std::uint32_t(i + 1)); }
			for (const auto& [t, pos] : at) { op.doc.field(strata::field_ref::declared(std::uint32_t(f))).terms.push_back({t, std::uint32_t(pos.size()), pos}); }
			length += std::uint32_t(tokens.size());
		}
		op.doc.length = length;
		return apply(std::move(op));
	}

	faultfs::filesystem fs_;
	faultfs::io io_;
	std::optional<prospect::index> ix_;
	std::mt19937_64 r_;
	std::vector<std::string> keys_;
};

// --- The evaluation ---------------------------------------------------------------------------

struct query {
	std::string field = text_name;
	std::vector<phrase_word> words;
	std::uint32_t window = 0;
	std::vector<std::uint32_t> offsets;
	slot_scoring scoring = slot_scoring::synonym;
	std::uint64_t max_expansions = 0;
};

inline bool accepts(const phrase_word& w, std::string_view term) {
	switch (w.how) {
	case phrase_word::literal: return term == w.text;
	case phrase_word::prefix: return reference::prefix(w.text, term);
	case phrase_word::glob: return reference::glob(w.text, w.glob_flags, term);
	default: return reference::levenshtein(w.text, w.distance, w.fixed, w.swaps, term);
	}
}

inline weight weigh(std::span<const segment_snapshot> segs, std::uint64_t df) { return weight::bm25_xapian(snapshot_statistics(segs), df, 1); }

// What a query must give: its slots' terms, or the error, and the documents with their scores.
struct expectation {
	bool error = false;
	std::size_t word = 0;
	std::uint64_t max = 0;
	bool per_query = false;
	std::vector<std::vector<std::string>> slots;
	std::vector<bool> exact_slot;  // a literal word whose term has no live document may or may not be kept
	std::map<std::uint64_t, double> hits;  // by docid
};

// The live terms of a field the word accepts, in term order.
inline std::vector<std::string> live_matches(const world& w, std::string_view field, const phrase_word& word) {
	std::set<std::string> out;
	for (const auto& [key, d] : w.live) {
		for (const auto& t : d.of(field)) {
			if (accepts(word, t)) { out.insert(t); }
		}
	}
	return {out.begin(), out.end()};
}

// A word's terms after its own limit (Xapian's limit types), by the snapshot's dfs for
// `most_frequent`; none when `error` is past its bound.
inline std::optional<std::vector<std::string>> limited(const world& w, std::string_view field, std::vector<std::string> terms, expansion_limit l) {
	if (!l.max || terms.size() <= l.max) { return terms; }
	if (l.type == expansion_limit::error) { return std::nullopt; }
	if (l.type == expansion_limit::first) {
		terms.resize(std::size_t(l.max));
		return terms;
	}
	std::vector<std::uint64_t> dfs;
	for (const auto& t : terms) { dfs.push_back(snapshot_df(w.segs, field, t)); }
	std::vector<std::size_t> order(terms.size());
	for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
	std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return dfs[a] > dfs[b]; });
	std::vector<std::string> top;
	for (std::size_t i = 0; i < std::size_t(l.max); ++i) { top.push_back(terms[order[i]]); }
	std::sort(top.begin(), top.end());
	return top;
}

// Distinct positions, one per slot, all within [lo, lo + window).
inline bool pick(const std::vector<std::set<std::uint32_t>>& at, std::size_t k, std::uint32_t lo, std::uint32_t window, std::set<std::uint32_t>& used) {
	if (k == at.size()) { return true; }
	for (const auto pos : at[k]) {
		if (pos < lo || pos - lo >= window || used.count(pos)) { continue; }
		used.insert(pos);
		if (pick(at, k + 1, lo, window, used)) { return true; }
		used.erase(pos);
	}
	return false;
}

// Whether the phrase occurs in a token sequence, position by position.
inline bool occurs(const std::vector<std::string>& tokens, const std::vector<std::vector<std::string>>& slots, const query& q) {
	std::vector<std::set<std::uint32_t>> at;
	for (const auto& s : slots) {
		at.emplace_back();
		for (std::size_t i = 0; i < tokens.size(); ++i) {
			if (std::find(s.begin(), s.end(), tokens[i]) != s.end()) { at.back().insert(std::uint32_t(i + 1)); }
		}
		if (at.back().empty()) { return false; }
	}
	if (q.window) {
		std::set<std::uint32_t> starts;
		for (const auto& a : at) { starts.insert(a.begin(), a.end()); }
		for (const auto lo : starts) {
			std::set<std::uint32_t> used;
			if (pick(at, 0, lo, q.window, used)) { return true; }
		}
		return false;
	}
	for (const auto first : at[0]) {
		bool all = true;
		for (std::size_t k = 1; k < at.size() && all; ++k) {
			const std::uint32_t offset = q.offsets.empty() ? std::uint32_t(k) : q.offsets[k] - q.offsets[0];
			all = at[k].count(first + offset) > 0;
		}
		if (all) { return true; }
	}
	return false;
}

inline bool close(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }

// The phrase over the world's live documents, by brute force.
inline expectation evaluate(const world& w, const query& q) {
	expectation e;
	std::optional<std::uint64_t> left;
	if (q.max_expansions) { left = q.max_expansions; }
	for (std::size_t i = 0; i < q.words.size(); ++i) {
		const auto& word = q.words[i];
		auto terms = live_matches(w, q.field, word);
		e.exact_slot.push_back(word.how != phrase_word::literal || !terms.empty());
		if (word.how != phrase_word::literal) {
			const std::uint64_t own = word.limit.max ? word.limit.max : ~std::uint64_t(0);
			const std::uint64_t room = left ? *left : ~std::uint64_t(0);
			const auto kept = limited(w, q.field, terms, word.limit);
			// A bound is past when `error` finds more terms than it allows, or any kept terms exceed
			// what the query has left; the query's is the one named when it is the tighter.
			if (!kept || kept->size() > room) {
				e.error = true;
				e.word = i;
				e.per_query = room < own;
				e.max = e.per_query ? q.max_expansions : word.limit.max;
				return e;
			}
			terms = *kept;
			if (left) { *left -= terms.size(); }
		}
		e.slots.push_back(std::move(terms));
	}
	// Scores: the sum of the slots', each as it scores alone.
	std::vector<std::vector<weight>> weights;
	for (const auto& s : e.slots) {
		std::vector<std::uint64_t> dfs;
		for (const auto& t : s) { dfs.push_back(snapshot_df(w.segs, q.field, t)); }
		std::vector<weight> ws;
		if (q.scoring == slot_scoring::synonym) {
			ws.push_back(weigh(w.segs, synonym_df(dfs, snapshot_statistics(w.segs).doc_count)));
		} else {
			for (const auto df : dfs) { ws.push_back(weigh(w.segs, df)); }
		}
		weights.push_back(std::move(ws));
	}
	for (const auto& [key, d] : w.live) {
		const auto& tokens = d.of(q.field);
		if (!occurs(tokens, e.slots, q)) { continue; }
		const std::uint32_t length = std::uint32_t(d.text.size() + d.title.size());
		double sum = 0;
		for (std::size_t k = 0; k < e.slots.size(); ++k) {
			std::uint64_t total = 0;
			for (std::size_t j = 0; j < e.slots[k].size(); ++j) {
				const std::uint32_t wdf = std::uint32_t(std::count(tokens.begin(), tokens.end(), e.slots[k][j]));
				total += wdf;
				if (q.scoring == slot_scoring::any && wdf) { sum += weights[k][j](wdf, length); }
			}
			if (q.scoring == slot_scoring::synonym) { sum += weights[k][0](std::uint32_t(std::min<std::uint64_t>(total, length)), length); }
		}
		e.hits[d.docid] = sum;
	}
	return e;
}

// What the library answers: the expansion and the ranked hits, or the error thrown.
struct answer {
	std::optional<phrase_expansion_error> error;
	std::vector<std::vector<std::string>> slots;
	std::vector<doc_hit> hits;
	std::uint64_t count = 0;
};

inline answer run(const world& w, const query& q, const walk_options& walk = {}) {
	answer a;
	term_table table(w.segs);
	phrase_options o;
	o.max_expansions = q.max_expansions;
	o.walk = walk;
	o.scoring = q.scoring;
	try {
		const expanded_phrase ph(w.segs, q.field, q.words, table, o);
		a.slots = ph.terms();
		a.hits = search(w.segs, ph.builder(q.window, q.offsets), 1000000, &a.count);
	} catch (const phrase_expansion_error& e) {
		a.error = e;
	}
	return a;
}

}  // namespace phrase_fixture
