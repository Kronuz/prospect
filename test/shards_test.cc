// Search over several indexes as one (include/prospect/shards.hh):
//   1. The docid numbering is Xapian's, and statistics, bounds and last docid agree with
//      Xapian's recorded for the same shards (test/fixtures/xapian-shards, from strata-oracle).
//   2. Hits, weights and counts equal Xapian's for relevance and key-then-relevance, both docid
//      orders, on 1, 2, 3, 5 (one with empty shards) and 7 shards.
//   3. Identity: any split of a document set into shards, segments and deletions answers exactly
//      as one index holding the same documents at the interleaved docids.
//   4. The visitor sees every live match exactly once.
#include "prospect/shards.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace prospect;

namespace {

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
	std::vector<std::vector<std::uint64_t>> deleted;
	std::vector<std::uint64_t> dead;
	std::vector<std::vector<std::uint64_t>> docids;  // by segment, by ordinal
	std::vector<segment_snapshot> snapshots() const {
		std::vector<segment_snapshot> out;
		for (std::size_t i = 0; i < segs.size(); ++i) { out.push_back({segs[i].get(), deleted[i].empty() ? nullptr : &deleted[i], deleted[i].empty() ? 0 : dead[i]}); }
		return out;
	}
};

// `groups[i]` are (docid, document) pairs of one segment, ascending; `dead` docids are marked deleted.
void add_segment(built& b, const std::vector<std::pair<std::uint64_t, const doc*>>& group, const std::set<std::uint64_t>& dead) {
	segment_writer w;
	std::vector<std::uint64_t> ids;
	std::vector<std::uint64_t> bits((group.size() + 63) / 64, 0);
	std::uint64_t n = 0;
	for (std::size_t o = 0; o < group.size(); ++o) {
		const auto p = postings_of(*group[o].second);
		w.add(group[o].first, p.inputs(), p.length);
		ids.push_back(group[o].first);
		if (dead.count(group[o].first)) {
			bits[o >> 6] |= std::uint64_t(1) << (o & 63);
			++n;
		}
	}
	b.bytes.push_back(w.flush({}));
	b.segs.push_back(std::make_unique<segment>(b.bytes.back()));
	b.deleted.push_back(n ? bits : std::vector<std::uint64_t>());
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
scorer_ptr build(const segment& seg, const query& q, std::span<const segment_snapshot> all, bool xapian) {
	const statistics st = snapshot_statistics(all);
	auto weight_of = [&](const std::string& t) {
		const std::uint64_t df = snapshot_df(all, t);
		return xapian ? weight::bm25_xapian(st, df) : weight::bm25(st, df);
	};
	auto term = [&](const std::string& t) -> scorer_ptr {
		auto info = seg.get<inverted_plugin>().term(t);
		return info ? std::make_unique<term_scorer>(seg.get<inverted_plugin>(), *info, weight_of(t)) : nullptr;
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
	std::vector<format::term_info> infos;
	std::vector<weight> ws;
	for (auto& t : q.terms) {
		auto info = seg.get<inverted_plugin>().term(t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
		ws.push_back(weight_of(t));
	}
	return std::make_unique<phrase_scorer>(seg.get<inverted_plugin>(), infos, std::move(ws));
}

scorer_builder builder_for(const query& q, const multi_snapshot& snap, bool xapian) {
	return [&q, &snap, xapian](const segment& seg) { return build(seg, q, snap.segments(), xapian); };
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

// --- 1. the numbering ------------------------------------------------------------------------------

void numbering() {
	// Hand values: three shards, shard 1's second document is document 5.
	CHECK(interleave_docid(1, 0, 3) == 1);
	CHECK(interleave_docid(1, 2, 3) == 3);
	CHECK(interleave_docid(2, 1, 3) == 5);
	CHECK(interleave_docid(7, 4, 5) == 35);
	CHECK(interleave_docid(5, 0, 1) == 5);
	CHECK((split_docid(5, 3) == shard_docid{1, 2}));
	std::mt19937_64 r(11);
	for (int i = 0; i < 20000; ++i) {
		const std::size_t n = 1 + r() % 64, s = r() % n;
		const std::uint64_t did = 1 + r() % 1000000;
		const auto id = interleave_docid(did, s, n);
		CHECK(split_docid(id, n) == (shard_docid{s, did}));
		CHECK(interleave_docid(did + 1, s, n) == id + n);  // increasing in the shard's docid
	}
	// Distinct shards never collide, and every docid up to n * m is some shard's.
	std::set<std::uint64_t> seen;
	for (std::size_t s = 0; s < 5; ++s) {
		for (std::uint64_t d = 1; d <= 40; ++d) { CHECK(seen.insert(interleave_docid(d, s, 5)).second); }
	}
	CHECK(seen.size() == 200 && *seen.begin() == 1 && *seen.rbegin() == 200);
	// Edges.
	const auto big = std::numeric_limits<std::uint64_t>::max();
	CHECK(interleave_docid(big / 2, 0, 2) == big - 1);
	auto throws = [](auto f) {
		try { f(); } catch (const std::exception&) { return true; }
		return false;
	};
	CHECK(throws([&] { (void)interleave_docid(0, 0, 3); }));
	CHECK(throws([&] { (void)interleave_docid(1, 3, 3); }));
	CHECK(throws([&] { (void)interleave_docid(1, 0, 0); }));
	CHECK(throws([&] { (void)interleave_docid(big, 1, 2); }));
	CHECK(throws([&] { (void)interleave_docid(big / 2 + 1, 1, 2); }));
	CHECK(throws([&] { (void)split_docid(0, 3); }));
}

// --- 2. Xapian's vectors ---------------------------------------------------------------------------

std::vector<std::string> read_lines(const std::string& path) {
	std::ifstream in(path);
	std::vector<std::string> out;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty()) { out.push_back(line); }
	}
	return out;
}

std::string field(const std::string& line, const std::string& key) {
	const std::size_t at = line.find("\"" + key + "\":");
	if (at == std::string::npos) { return {}; }
	std::size_t a = at + key.size() + 3, b = a;
	if (line[a] == '"') { return line.substr(a + 1, line.find('"', a + 1) - a - 1); }
	while (b < line.size() && line[b] != ',' && line[b] != '}' && line[b] != ']') { ++b; }
	return line.substr(a, b - a);
}

std::vector<std::uint64_t> number_list(const std::string& line, const std::string& key) {
	std::vector<std::uint64_t> out;
	std::size_t at = line.find("\"" + key + "\":[");
	for (at += key.size() + 4; line[at] != ']';) {
		std::size_t end = at;
		while (line[end] != ',' && line[end] != ']') { ++end; }
		out.push_back(std::stoull(line.substr(at, end - at)));
		at = line[end] == ',' ? end + 1 : end;
	}
	return out;
}

std::string unhex(const std::string& h) {
	std::string out;
	for (std::size_t i = 0; i + 1 < h.size(); i += 2) { out.push_back(char(std::stoi(h.substr(i, 2), nullptr, 16))); }
	return out;
}

std::vector<doc> read_corpus(const std::string& path, std::size_t& shards) {
	const auto lines = read_lines(path);
	shards = std::stoul(field(lines[0], "shards"));
	std::vector<doc> out;
	for (std::size_t i = 1; i < lines.size(); ++i) {
		doc d;
		d.shard = std::stoul(field(lines[i], "shard"));
		d.did = std::stoull(field(lines[i], "did"));
		d.key = unhex(field(lines[i], "key"));
		for (std::size_t at = lines[i].find("\"terms\":[") + 9; lines[i][at] != ']';) {
			const std::size_t a = lines[i].find('"', at), b = lines[i].find('"', a + 1);
			d.terms.push_back(lines[i].substr(a + 1, b - a - 1));
			at = lines[i][b + 1] == ',' ? b + 2 : b + 1;
		}
		out.push_back(std::move(d));
	}
	return out;
}

std::vector<std::pair<std::uint64_t, double>> hits_of(const std::string& line) {
	std::vector<std::pair<std::uint64_t, double>> out;
	std::size_t at = line.find("\"hits\":[") + 8;
	while (line[at] == '[') {
		const std::size_t c = line.find(',', at), close = line.find(']', c);
		out.push_back({std::stoull(line.substr(at + 1, c - at - 1)), std::stod(line.substr(c + 1, close - c - 1))});
		at = line[close + 1] == ',' ? close + 2 : close + 1;
	}
	return out;
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

void oracle(const std::string& dir) {
	int rows = 0, files = 0;
	for (const char* name : {"s1", "s2", "s3", "s5", "s5e", "s7"}) {
		std::size_t n = 0;
		const auto docs = read_corpus(dir + "/shards-" + name + ".jsonl", n);
		const auto vectors = read_lines(dir + "/shards-" + name + ".out.jsonl");
		CHECK(!vectors.empty());
		if (vectors.empty()) { continue; }
		++files;
		std::map<std::uint64_t, std::string> key_of;
		for (const auto& d : docs) { key_of[interleave_docid(d.did, d.shard, n)] = d.key; }
		for (const unsigned parts : {1u, 3u}) {
			std::mt19937_64 r(100 + parts);
			const auto set = build_shards(docs, n, r, parts);
			const multi_snapshot snap(set.inputs());
			// Statistics and numbering.
			const auto& head = vectors[0];
			CHECK(snap.shard_count() == std::stoul(field(head, "shards")));
			CHECK(snap.stats().doc_count == std::stoull(field(head, "docs")));
			CHECK(near_eq(snap.stats().average_length(), std::stod(field(head, "avlength")), 1e-15));
			CHECK(snap.last_docid() == std::stoull(field(head, "lastdocid")));
			CHECK(snap.doc_length_bounds().lower == std::stoul(field(head, "lower_length")));
			CHECK(snap.doc_length_bounds().upper == std::stoul(field(head, "upper_length")));
			const auto shard_docs = number_list(head, "shard_docs");
			for (std::size_t s = 0; s < n; ++s) {
				CHECK(snapshot_statistics(snap.segments(s)).doc_count == shard_docs[s]);
			}
			for (std::size_t i = 1; i < vectors.size(); ++i) {
				const auto& line = vectors[i];
				if (line.starts_with("{\"term\"")) {
					CHECK(snap.df(field(line, "term")) == std::stoull(field(line, "termfreq")));
					continue;
				}
				const query q = parse_query(field(line, "query"));
				const std::string mode = field(line, "mode");
				const bool keyed = mode.starts_with("key"), reverse = mode == "key-rev";
				const docid_order order = mode.ends_with("desc") ? docid_order::descending : docid_order::ascending;
				const auto want = hits_of(line);
				const std::uint64_t check = std::stoull(field(line, "check_at_least"));
				const auto build_fn = builder_for(q, snap, true);
				for (const bool force : {false, true}) {
					search_options o;
					o.k = 25;
					o.count = true;
					o.order = order;
					o.reverse_key = reverse;
					if (keyed) { o.key = [&](const match& m) { return key_of.at(m.docid); }; }
					if (force) { o.visit = [](const match&) {}; }
					const auto got = search(snap, build_fn, o);
					CHECK(got.hits.size() == want.size());
					CHECK(got.count && *got.count >= std::stoull(field(line, "lower")) && *got.count <= std::stoull(field(line, "upper")));
					if (check == snap.stats().doc_count) {  // every match counted: Xapian's bounds meet
						CHECK(got.count && *got.count == std::stoull(field(line, "lower")) && *got.count == std::stoull(field(line, "upper")));
					}
					for (std::size_t h = 0; h < got.hits.size() && h < want.size(); ++h) {
						CHECK(near_eq(got.hits[h].score, want[h].second));
						const bool tied = (h > 0 && near_eq(want[h - 1].second, want[h].second)) || (h + 1 < want.size() && near_eq(want[h + 1].second, want[h].second));
						if (!tied) { CHECK(got.hits[h].docid == want[h].first); }
						if (keyed && !tied) { CHECK(key_of.at(got.hits[h].docid) == key_of.at(want[h].first)); }
					}
					if (!keyed || true) {
						// The hits' keys are non-decreasing (non-increasing when reversed).
						for (std::size_t h = 1; keyed && h < got.hits.size(); ++h) {
							const auto &a = key_of.at(got.hits[h - 1].docid), &b = key_of.at(got.hits[h].docid);
							CHECK(reverse ? a >= b : a <= b);
						}
					}
				}
				++rows;
			}
		}
	}
	std::printf("  oracle: %d files, %d rows compared\n", files, rows);
	CHECK(files == 6 && rows > 3000);
}


// --- 3. identity over random splits -----------------------------------------------------------------

static const char* const vocab[] = {"a", "b", "c", "d", "e", "f", "g", "h", "rare"};
static const std::vector<std::string> keys = {"", "a", std::string("a\0b", 3), "b", "ab", "\xff", "m"};

std::vector<doc> random_docs(std::mt19937_64& r, std::size_t n, std::size_t count, bool skew) {
	std::vector<doc> docs;
	std::vector<std::uint64_t> next(n, 0);
	for (std::size_t i = 0; i < count; ++i) {
		doc d;
		d.shard = skew && r() % 2 ? 0 : r() % n;
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

void identity(int rounds) {
	std::mt19937_64 r(2026);
	std::size_t compared = 0, empty_shards = 0, dead_rounds = 0;
	for (int round = 0; round < rounds; ++round) {
		const std::size_t n = 1 + r() % 7;
		const auto docs = random_docs(r, n, 120 + r() % 500, round % 3 == 0);
		std::set<std::pair<std::size_t, std::uint64_t>> dead;
		std::set<std::uint64_t> dead_merged;
		if (round % 2 == 1) {
			++dead_rounds;
			for (const auto& d : docs) {
				if (r() % 8 == 0) {
					dead.insert({d.shard, d.did});
					dead_merged.insert(interleave_docid(d.did, d.shard, n));
				}
			}
		}
		const auto set = build_shards(docs, n, r, 1 + round % 4, dead);
		const multi_snapshot multi(set.inputs());
		for (std::size_t s = 0; s < n; ++s) { empty_shards += set.shards[s].segs.empty(); }
		// One index holding the same documents at the interleaved docids.
		built merged;
		{
			std::vector<std::pair<std::uint64_t, const doc*>> group;
			for (const auto& d : docs) { group.push_back({interleave_docid(d.did, d.shard, n), &d}); }
			std::sort(group.begin(), group.end());
			add_segment(merged, group, dead_merged);
		}
		const multi_snapshot one(std::vector<shard_input>{{merged.snapshots(), 0}});
		CHECK(multi.stats().doc_count == one.stats().doc_count && multi.stats().total_length == one.stats().total_length);
		std::map<std::uint64_t, std::string> key_of;
		for (const auto& d : docs) { key_of[interleave_docid(d.did, d.shard, n)] = d.key; }
		const auto key_fn = [&](std::uint64_t id) { return key_of.at(id); };
		for (int qn = 0; qn < 24; ++qn) {
			query q;
			const char* kinds[] = {"TERM", "OR", "AND", "AND_MAYBE", "AND_NOT", "PHRASE"};
			q.kind = kinds[r() % 6];
			const std::size_t want = q.kind == "TERM" ? 1 : q.kind == "PHRASE" ? 2 + r() % 2 : q.kind == "OR" || q.kind == "AND" ? 2 + r() % 3 : 2;
			std::set<std::string> used;
			while (q.terms.size() < want) {
				std::string t = vocab[r() % 9];
				if (q.kind == "PHRASE" || used.insert(t).second) { q.terms.push_back(t); }
			}
			const bool xapian = qn % 2 == 1;
			const auto b_multi = builder_for(q, multi, xapian), b_one = builder_for(q, one, xapian);
			auto all = model(multi, b_multi, key_fn);
			for (const std::size_t k : {std::size_t(0), std::size_t(1), std::size_t(7), std::size_t(60), std::size_t(5000)}) {
				// Relevance, pruned: shards equal the merged index and the model.
				search_options o;
				o.k = k;
				o.count = true;
				const auto got = search(multi, b_multi, o), ref = search(one, b_one, o);
				CHECK(got.count && ref.count && *got.count == *ref.count && *got.count == all.size());
				CHECK(got.hits == ref.hits);  // exactly: ids, order and score bits
				auto sorted = all;
				sort_model(sorted, false, false, docid_order::ascending);
				std::vector<doc_hit> expect;
				for (std::size_t i = 0; i < std::min(k, sorted.size()); ++i) { expect.push_back({sorted[i].docid, sorted[i].score}); }
				CHECK(same_hits(got.hits, expect));
				++compared;
				// Exhaustive paths: key, descending docids, a visitor, each against the model
				// (exactly: same scorer calls) and the merged index.
				for (int variant = 0; variant < 5; ++variant) {
					search_options e;
					e.k = k;
					const bool keyed = variant >= 2;
					e.reverse_key = variant == 3;
					e.order = variant == 1 || variant == 4 ? docid_order::descending : docid_order::ascending;
					if (keyed) { e.key = [&](const match& m) { return key_of.at(m.docid); }; }
					if (variant == 0) { e.visit = [](const match&) {}; }
					const auto x = search(multi, b_multi, e), y = search(one, b_one, e);
					CHECK(x.count && *x.count == all.size());
					CHECK(x.hits == y.hits);
					sorted = all;
					sort_model(sorted, keyed, e.reverse_key, e.order);
					expect.clear();
					for (std::size_t i = 0; i < std::min(k, sorted.size()); ++i) { expect.push_back({sorted[i].docid, sorted[i].score}); }
					CHECK(x.hits == expect);
					++compared;
				}
			}
		}
	}
	std::printf("  identity: %zu comparisons, %zu empty shards, %zu rounds with deletions\n", compared, empty_shards, dead_rounds);
	CHECK(compared > 1000 && empty_shards > 0 && dead_rounds > 0);
}

// --- 4. the visitor --------------------------------------------------------------------------------

void visitor(int rounds) {
	std::mt19937_64 r(77);
	std::size_t visited_total = 0;
	for (int round = 0; round < rounds; ++round) {
		const std::size_t n = 1 + r() % 6;
		const auto docs = random_docs(r, n, 100 + r() % 300, false);
		std::set<std::pair<std::size_t, std::uint64_t>> dead;
		if (round % 2) {
			for (const auto& d : docs) {
				if (r() % 5 == 0) { dead.insert({d.shard, d.did}); }
			}
		}
		const auto set = build_shards(docs, n, r, 3, dead);
		const multi_snapshot snap(set.inputs());
		for (int qn = 0; qn < 12; ++qn) {
			query q{"OR", {vocab[r() % 9], vocab[r() % 9]}};
			if (qn % 3 == 1) { q.kind = "AND"; }
			if (q.terms[0] == q.terms[1]) { q.terms.pop_back(); }
			const auto b = builder_for(q, snap, qn % 2);
			const auto all = model(snap, b, nullptr);
			std::map<std::uint64_t, double> truth;
			for (const auto& m : all) { CHECK(truth.emplace(m.docid, m.score).second); }
			for (const std::size_t k : {std::size_t(0), std::size_t(3), std::size_t(1000)}) {
				std::map<std::uint64_t, int> seen;
				std::size_t last_shard = 0;
				search_options o;
				o.k = k;
				o.visit = [&](const match& m) {
					++seen[m.docid];
					CHECK(m.docid == interleave_docid(m.did, m.shard, n));
					CHECK(m.seg->core().docid(m.ord) == m.did);
					CHECK(m.shard >= last_shard);
					last_shard = m.shard;
					CHECK(truth.count(m.docid) && m.score() == truth.at(m.docid) && m.score() == truth.at(m.docid));
				};
				const auto got = search(snap, b, o);
				CHECK(seen.size() == truth.size());
				for (const auto& [id, times] : seen) { CHECK(times == 1 && truth.count(id)); }
				CHECK(got.count && *got.count == truth.size());
				CHECK(got.hits.size() == std::min(k, truth.size()));
				visited_total += seen.size();
			}
		}
	}
	std::printf("  visitor: %zu matches seen\n", visited_total);
	CHECK(visited_total > 1000);
}

}  // namespace

int main(int argc, char** argv) {
	numbering();
	const std::string dir = argc > 1 ? argv[1] : PROSPECT_FIXTURES "/xapian-shards";
	oracle(dir);
	identity(std::getenv("SHARDS_ROUNDS") ? std::atoi(std::getenv("SHARDS_ROUNDS")) : 60);
	visitor(40);
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("shards_test ok\n");
	return 0;
}
