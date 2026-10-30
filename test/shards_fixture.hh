// The documents, indexes and query builders of shards_test, shared with sliced_test: documents split over
// shards and segments, Xapian-style weights over the snapshot, and a model that scores every live match.
#pragma once

#include "prospect/shards.hh"
#include "text_field.hh"
#include "prospect/index.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace shards_fixture {

using namespace prospect;

// --- documents and indexes ------------------------------------------------------------------------

struct doc {
	std::size_t shard = 0;
	std::uint64_t did = 0;
	std::string key;
	std::vector<std::string> terms;  // in order: positions are 1, 2, ...
};

// A document's postings, sorted by term.
struct postings {
	std::vector<std::string> names;
	std::vector<std::vector<std::uint32_t>> positions;
	std::uint32_t length = 0;
	std::vector<posting_input> inputs() const {
		std::vector<posting_input> out;
		for (std::size_t i = 0; i < names.size(); ++i) { out.push_back({names[i], std::uint32_t(positions[i].size()), positions[i]}); }
		return out;
	}
};

postings postings_of(const doc& d) {
	std::map<std::string, std::vector<std::uint32_t>> by;
	std::uint32_t pos = 0;
	for (const auto& t : d.terms) { by[t].push_back(++pos); }
	postings p;
	p.length = pos;
	for (auto& [name, ps] : by) {
		p.names.push_back(name);
		p.positions.push_back(std::move(ps));
	}
	return p;
}

// Segments built from documents (ascending docids within a segment); they own their bytes.
struct built {
	std::vector<std::string> bytes;
	std::vector<std::unique_ptr<segment>> segs;
	std::vector<strata::deleted_bits> deleted;
	std::vector<std::uint64_t> dead;
	std::vector<std::vector<std::uint64_t>> docids;  // by segment, by ordinal
	std::vector<segment_snapshot> snapshots() const {
		std::vector<segment_snapshot> out;
		for (std::size_t i = 0; i < segs.size(); ++i) { out.push_back({segs[i].get(), deleted[i].empty() ? nullptr : &deleted[i], deleted[i].empty() ? 0 : dead[i], &text_table()}); }
		return out;
	}
};

// `groups[i]` are (docid, document) pairs of one segment, ascending; `dead` docids are marked deleted.
void add_segment(built& b, const std::vector<std::pair<std::uint64_t, const doc*>>& group, const std::set<std::uint64_t>& dead) {
	segment_writer w;
	std::vector<std::uint64_t> ids;
	strata::deleted_bits bits((group.size() + 63) / 64);
	std::uint64_t* words = bits.mutable_words();
	std::uint64_t n = 0;
	for (std::size_t o = 0; o < group.size(); ++o) {
		const auto p = postings_of(*group[o].second);
		w.add(group[o].first, p.inputs(), p.length);
		ids.push_back(group[o].first);
		if (dead.count(group[o].first)) {
			words[o >> 6] |= std::uint64_t(1) << (o & 63);
			++n;
		}
	}
	b.bytes.push_back(w.flush({}));
	b.segs.push_back(std::make_unique<segment>(b.bytes.back()));
	b.deleted.push_back(n ? std::move(bits) : strata::deleted_bits());
	b.dead.push_back(n);
	b.docids.push_back(std::move(ids));
}

// --- queries ---------------------------------------------------------------------------------------

struct query {
	std::string kind;  // TERM OR AND AND_MAYBE AND_NOT PHRASE
	std::vector<std::string> terms;
};

query parse_query(const std::string& line) {
	query q;
	std::size_t at = 0;
	for (bool first = true; at < line.size();) {
		std::size_t sp = line.find(' ', at);
		if (sp == std::string::npos) { sp = line.size(); }
		(first ? q.kind : q.terms.emplace_back()) = line.substr(at, sp - at);
		first = false;
		at = sp + 1;
	}
	return q;
}

// Xapian's weights over the snapshot's statistics (global across the shards).
scorer_ptr build(const segment& seg, const query& q, const multi_snapshot& snap, bool xapian) {
	const statistics st = snap.stats();
	auto weight_of = [&](const std::string& t) {
		const std::uint64_t df = snap.df("text", t);
		return xapian ? weight::bm25_xapian(st, df) : weight::bm25(st, df);
	};
	auto term = [&](const std::string& t) -> scorer_ptr {
		auto info = seg.get<prospect::inverted_plugin>().term(0, t);
		return info ? std::make_unique<term_scorer>(seg.get<prospect::inverted_plugin>(), *info, weight_of(t)) : nullptr;
	};
	std::vector<scorer_ptr> kids;
	if (q.kind == "TERM") { return term(q.terms[0]); }
	if (q.kind == "OR") {
		for (auto& t : q.terms) {
			if (auto s = term(t)) { kids.push_back(std::move(s)); }
		}
		if (kids.empty()) { return nullptr; }
		return kids.size() == 1 ? std::move(kids[0]) : std::make_unique<disjunction_scorer>(std::move(kids));
	}
	if (q.kind == "AND") {
		for (auto& t : q.terms) {
			auto s = term(t);
			if (!s) { return nullptr; }
			kids.push_back(std::move(s));
		}
		return kids.size() == 1 ? std::move(kids[0]) : std::make_unique<conjunction_scorer>(std::move(kids));
	}
	if (q.kind == "AND_MAYBE") {
		auto req = term(q.terms[0]), opt = term(q.terms[1]);
		if (!req) { return nullptr; }
		return opt ? std::make_unique<required_optional_scorer>(std::move(req), std::move(opt)) : std::move(req);
	}
	if (q.kind == "AND_NOT") {
		auto in = term(q.terms[0]);
		if (!in) { return nullptr; }
		if (auto ex = term(q.terms[1])) { return std::make_unique<exclusion_scorer>(std::move(in), std::move(ex)); }
		return in;
	}
	std::vector<prospect::format::term_info> infos;
	std::vector<weight> ws;
	for (auto& t : q.terms) {
		auto info = seg.get<prospect::inverted_plugin>().term(0, t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
		ws.push_back(weight_of(t));
	}
	return std::make_unique<phrase_scorer>(seg.get<prospect::inverted_plugin>(), infos, std::move(ws));
}

scorer_builder builder_for(const query& q, const multi_snapshot& snap, bool xapian) {
	return [&q, &snap, xapian](const segment& seg) { return build(seg, q, snap, xapian); };
}

bool near_eq(double a, double b, double rel = 1e-9) { return std::abs(a - b) <= rel * std::max({1.0, std::abs(a), std::abs(b)}); }

// --- the model: every live match scored without pruning, sorted by the rules ---------------------

struct row {
	std::string key;
	double score;
	std::uint64_t docid;
};

std::vector<row> model(const multi_snapshot& snap, const scorer_builder& b, const std::function<std::string(std::uint64_t)>& key_of) {
	std::vector<row> all;
	for (std::size_t s = 0; s < snap.shard_count(); ++s) {
		for (const auto& sg : snap.segments(s)) {
			auto q = b(*sg.seg);
			if (!q) { continue; }
			for (ordinal_t o = q->next(); o != no_more_docs; o = q->next()) {
				if (sg.deleted && ((*sg.deleted)[o >> 6] >> (o & 63)) & 1) { continue; }
				const std::uint64_t id = snap.docid(s, sg.seg->core().docid(o));
				all.push_back({key_of ? key_of(id) : std::string(), q->score(), id});
			}
		}
	}
	return all;
}

void sort_model(std::vector<row>& all, bool keyed, bool reverse, docid_order order) {
	std::sort(all.begin(), all.end(), [&](const row& a, const row& c) {
		if (keyed && a.key != c.key) { return reverse ? a.key > c.key : a.key < c.key; }
		if (a.score != c.score) { return a.score > c.score; }
		return order == docid_order::descending ? a.docid > c.docid : a.docid < c.docid;
	});
}


// Splits each shard's documents over `parts` segments at random, ascending within a segment.
struct shard_set {
	std::vector<built> shards;
	std::vector<shard_input> inputs() const {
		std::vector<shard_input> out;
		for (const auto& b : shards) { out.push_back({b.snapshots(), 0}); }
		return out;
	}
};

shard_set build_shards(const std::vector<doc>& docs, std::size_t n, std::mt19937_64& r, unsigned max_parts, const std::set<std::pair<std::size_t, std::uint64_t>>& dead = {}) {
	shard_set set;
	set.shards.resize(n);
	for (std::size_t s = 0; s < n; ++s) {
		std::vector<const doc*> mine;
		for (const auto& d : docs) {
			if (d.shard == s) { mine.push_back(&d); }
		}
		std::sort(mine.begin(), mine.end(), [](const doc* a, const doc* b) { return a->did < b->did; });
		const unsigned parts = 1 + unsigned(r() % max_parts);
		std::vector<std::vector<std::pair<std::uint64_t, const doc*>>> groups(parts);
		for (const doc* d : mine) { groups[r() % parts].push_back({d->did, d}); }
		std::set<std::uint64_t> gone;
		for (const auto& x : dead) {
			if (x.first == s) { gone.insert(x.second); }
		}
		for (const auto& g : groups) {
			if (!g.empty()) { add_segment(set.shards[s], g, gone); }
		}
	}
	return set;
}

static const char* const vocab[] = {"a", "b", "c", "d", "e", "f", "g", "h", "rare"};
static const std::vector<std::string> keys = {"", "a", std::string("a\0b", 3), "b", "ab", "\xff", "m"};

// Documents over `n` shards; `holes` leaves the first and last shard empty, `skew` favors shard 0.
std::vector<doc> random_docs(std::mt19937_64& r, std::size_t n, std::size_t count, bool skew, bool holes = false) {
	std::vector<doc> docs;
	std::vector<std::uint64_t> next(n, 0);
	for (std::size_t i = 0; i < count; ++i) {
		doc d;
		d.shard = skew && r() % 2 ? 0 : r() % n;
		if (holes && n > 2) { d.shard = 1 + d.shard % (n - 2); }
		next[d.shard] += 1 + (r() % 6 == 0 ? r() % 4 : 0);  // gaps in the shard's docids
		d.did = next[d.shard];
		d.key = keys[r() % keys.size()];
		const std::size_t len = 1 + r() % 9;
		for (std::size_t t = 0; t < len; ++t) {
			const char* w = vocab[r() % 9];
			if (std::string_view(w) == "rare" && r() % 6) { w = vocab[r() % 8]; }
			d.terms.push_back(w);
		}
		docs.push_back(std::move(d));
	}
	return docs;
}

bool same_hits(const std::vector<doc_hit>& a, const std::vector<doc_hit>& b) {
	if (a.size() != b.size()) { return false; }
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (!near_eq(a[i].score, b[i].score)) { return false; }
		const bool tied = (i > 0 && near_eq(b[i - 1].score, b[i].score)) || (i + 1 < b.size() && near_eq(b[i + 1].score, b[i].score));
		if (!tied && a[i].docid != b[i].docid) { return false; }
	}
	return true;
}


}  // namespace shards_fixture
