// Prefix, wildcard and fuzzy term expansion (docs/design/automata.md, docs/mechanisms/automata.md):
// an automaton from Kronuz/automata walked over one field's terms in each segment, seeking past
// dead keys (D4), each match's info decoded in the walk (D5), the segments' matches merged under
// Xapian's limit types with terms that have no live document skipped (D6), and the expansion's
// columns filled into the query's term_table for the scorers (D7).
#pragma once

#include "automata.hh"
#include "prospect/searcher.hh"
#include "task.hh"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prospect {

// Xapian's limit types and max_expansion (0: no limit). `error` throws past the limit, `first`
// keeps the first terms in term order, `most_frequent` the terms of highest snapshot df, ties by
// term order.
struct expansion_limit {
	enum kind { error, first, most_frequent } type = error;
	std::uint64_t max = 0;
};

// An `error`-limited expansion matched more distinct live terms than its limit.
struct expansion_error : std::runtime_error {
	explicit expansion_error(std::uint64_t limit) : std::runtime_error("prospect: expansion past its limit of " + std::to_string(limit) + " terms"), max(limit) {}
	std::uint64_t max;
};

// Every check_every entries a walk throws task::cancelled when `cancel` is requested, then asks
// pause(), which returns true to stop the walk where it is (D8). charge() is offered the bytes
// of the automaton's recorded states and of the expansion's terms before they are allocated, and
// throws to refuse them.
struct walk_options {
	std::size_t check_every = 1024;  // a preset: it changes no answer
	const task::cancel_token* cancel = nullptr;
	std::function<bool()> pause;
	automata::charge_fn charge;
};

namespace detail {

inline automata::walk_options walk_of(const walk_options& o) {
	automata::walk_options w;
	w.check_every = o.check_every;
	w.charge = o.charge;
	if (o.cancel || o.pause) {
		w.pause = [&o] {
			if (o.cancel && o.cancel->requested()) { throw task::cancelled{}; }
			return o.pause && o.pause();
		};
	}
	return w;
}

// The key field's terms (D4, D9): keys_plugin's scan restarted at each seek target, since the
// keys section has no seekable cursor. A key is a leaf of one ordinal: df 1, wdf 0, no positions.
class key_cursor {
public:
	explicit key_cursor(const keys_plugin::reader& keys) : keys_(&keys) {}
	bool seek(std::string_view from) {
		range_.emplace(keys_->range(from));
		return range_->valid();
	}
	bool next() {
		range_->next();
		return range_->valid();
	}
	std::string_view key() const noexcept { return range_->key(); }
	format::term_info info() const {
		format::term_info t;
		t.flags = format::single_posting | format::no_wdf | format::no_positions;
		t.df = 1;
		t.postings = range_->ordinal();
		return t;
	}

private:
	const keys_plugin::reader* keys_;
	std::optional<keys::reader::range> range_;
};

// Whether a term has a live document in a segment with deletes: its postings walked to the first
// live ordinal (D6; a single posting's ordinal is inline).
inline bool has_live(const segment_snapshot& s, const format::term_info& info) {
	const auto& del = *s.deleted;
	auto dead = [&](ordinal_t o) { return std::size_t(o >> 6) < del.size() && (del[o >> 6] >> (o & 63)) & 1; };
	if (info.flags & format::single_posting) { return !dead(ordinal_t(info.postings)); }
	postings_iterator it(s.seg->get<inverted_plugin>(), info);
	for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
		if (!dead(o)) { return true; }
	}
	return false;
}

}  // namespace detail

// Calls f(term, info) for every term of `field` in one segment the automaton accepts, at or
// above `from`, in order, until f returns false; info as find_term decodes it (impacts left
// empty below 128 postings). Deleted documents are not looked at. Returns the key to resume
// from when pause() stopped the walk, none when it ended.
template <class A, class F>
std::optional<std::string> intersect(const segment_snapshot& s, std::string_view field, const A& a, F&& f, const walk_options& o = {}, std::string_view from = {}) {
	const auto id = s.field_id(field);
	if (!id) { return std::nullopt; }
	const auto w = detail::walk_of(o);
	if (s.key_field() == id) {
		detail::key_cursor c(s.seg->get<keys_plugin>());
		return automata::walk(c, a, [&](std::string_view key) { return f(key, c.info()); }, w, from);
	}
	inverted_plugin::reader::field_cursor c(s.seg->get<inverted_plugin>(), *id);
	return automata::walk(c, a, [&](std::string_view term) { return f(term, c.info()); }, w, from);
}

// The snapshot's distinct terms of `field` the automaton accepts, in term order, after the limit;
// each fills its column of `terms` (its info in every segment holding it, deleted documents
// counted in its df as everywhere). A term with no live document in any segment is not selected
// and counts toward no limit (D6). Throws expansion_error past an `error` limit. A walk pause()
// stops is resumed at once: a caller that yields between slices drives intersect itself.
template <class A>
std::vector<std::string> expand(std::span<const segment_snapshot> segs, std::string_view field, const A& a, expansion_limit limit, term_table& terms, const walk_options& o = {}) {
	struct entry {
		std::vector<std::optional<format::term_info>> infos;  // by segment
		bool live = false;
	};
	std::map<std::string, entry, std::less<>> merged;
	std::uint64_t live = 0;
	std::optional<std::string> cutoff;  // `first`: the last of the first max live terms
	const bool capped = limit.max != 0;
	for (std::size_t i = 0; i < segs.size(); ++i) {
		const segment_snapshot& s = segs[i];
		const bool deletes = s.any_deleted();
		auto on = [&](std::string_view term, const format::term_info& info) {
			if (cutoff && term > *cutoff) { return false; }
			auto it = merged.find(term);
			if (it == merged.end()) {
				if (o.charge) { o.charge(term.size() + segs.size() * sizeof(std::optional<format::term_info>)); }
				it = merged.emplace(std::string(term), entry{std::vector<std::optional<format::term_info>>(segs.size()), false}).first;
			}
			it->second.infos[i] = info;
			if (it->second.live || (deletes && !detail::has_live(s, info))) { return true; }
			it->second.live = true;
			++live;
			if (!capped || live <= limit.max) { return true; }
			if (limit.type == expansion_limit::error) { throw expansion_error(limit.max); }
			if (limit.type == expansion_limit::first) {  // keep the first max: drop the largest and what follows it
				while (live > limit.max) {
					const auto last = std::prev(merged.end());
					live -= last->second.live;
					merged.erase(last);
				}
				while (!std::prev(merged.end())->second.live) { merged.erase(std::prev(merged.end())); }
				cutoff = std::prev(merged.end())->first;
				return term < *cutoff;
			}
			return true;
		};
		for (std::optional<std::string> from = std::string(); from;) { from = intersect(s, field, a, on, o, *from); }
	}
	std::vector<std::string> out;
	if (limit.type == expansion_limit::most_frequent && capped && live > limit.max) {
		std::vector<std::pair<std::uint64_t, const std::string*>> ranked;  // (snapshot df, term)
		for (const auto& [term, e] : merged) {
			if (!e.live) { continue; }
			std::uint64_t df = 0;
			for (const auto& info : e.infos) { df += info ? info->df : 0; }
			ranked.emplace_back(df, &term);
		}
		std::stable_sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
		ranked.resize(std::size_t(limit.max));
		for (const auto& r : ranked) { out.push_back(*r.second); }
		std::sort(out.begin(), out.end());
	} else {
		for (const auto& [term, e] : merged) {
			if (e.live) { out.push_back(term); }
		}
	}
	for (const auto& t : out) { terms.put(field, t, std::move(merged.find(t)->second.infos)); }
	return out;
}

}  // namespace prospect
