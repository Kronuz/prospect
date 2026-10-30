// Search over several indexes as one (include/prospect/shards.hh):
//   1. The docid numbering is Xapian's, and statistics, bounds and last docid agree with
//      Xapian's recorded for the same shards (test/fixtures/xapian-shards, from strata-oracle).
//   2. Hits, weights and counts equal Xapian's for relevance and key-then-relevance, both docid
//      orders, on 1, 2, 3, 5 (one with empty shards) and 7 shards.
//   3. Identity: any split of a document set into shards, segments and deletions answers exactly
//      as one index holding the same documents at the interleaved docids.
//   4. The visitor sees every live match exactly once.
#include "shards_fixture.hh"
#include "text_field.hh"
#include "prospect/index.hh"
#include "strata/core/faultfs.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace prospect;

namespace {

using namespace shards_fixture;
using shards_fixture::built;

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
	CHECK(interleave_docid(big / 2, 0, 2) == big - 2);
	CHECK(interleave_docid(big / 2 + 1, 0, 2) == big);
	CHECK(interleave_docid(big / 2, 1, 2) == big - 1);
	auto throws = [](auto f) {
		try { f(); } catch (const std::exception&) { return true; }
		return false;
	};
	CHECK(throws([&] { (void)interleave_docid(0, 0, 3); }));
	CHECK(throws([&] { (void)interleave_docid(1, 3, 3); }));
	CHECK(throws([&] { (void)interleave_docid(1, 0, 0); }));
	CHECK(throws([&] { (void)interleave_docid(big, 1, 2); }));
	CHECK(throws([&] { (void)interleave_docid(big / 2 + 1, 1, 2); }));
	CHECK(throws([&] { (void)interleave_docid(big / 2 + 2, 0, 2); }));
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
					CHECK(snap.df("text", field(line, "term")) == std::stoull(field(line, "termfreq")));
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
					// Xapian stops once `check_at_least` documents are checked, so its bounds meet only when the
					// matches are fewer than that (tier 1) or when it has seen the end.
					if (got.count && (*got.count < check || std::stoull(field(line, "lower")) == std::stoull(field(line, "upper")))) {
						CHECK(*got.count == std::stoull(field(line, "lower")) && *got.count == std::stoull(field(line, "upper")) && *got.count == std::stoull(field(line, "estimated")));
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

void identity(int rounds) {
	std::mt19937_64 r(2026);
	std::size_t compared = 0, empty_shards = 0, dead_rounds = 0;
	for (int round = 0; round < rounds; ++round) {
		const std::size_t n = 1 + r() % 7;
		const auto docs = random_docs(r, n, 120 + r() % 500, round % 3 == 0, round % 4 == 2);
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
				for (int variant = 0; variant < 6; ++variant) {
					search_options e;
					e.k = k;
					const bool keyed = variant >= 2 && variant != 5;
					e.reverse_key = variant == 3;
					e.order = variant == 1 || variant == 4 ? docid_order::descending : variant == 5 ? docid_order::dont_care : docid_order::ascending;
					if (keyed) { e.key = [&](const match& m) { return key_of.at(m.docid); }; }
					if (variant == 0 || variant == 5) { e.visit = [](const match&) {}; }
					const auto x = search(multi, b_multi, e), y = search(one, b_one, e);
					CHECK(x.count && *x.count == all.size());
					CHECK(x.hits == y.hits);
					sorted = all;
					sort_model(sorted, keyed, e.reverse_key, e.order == docid_order::dont_care ? docid_order::ascending : e.order);
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

// --- 5. one snapshot, several threads --------------------------------------------------------------

// Searches are read-only over the snapshot: concurrent ones answer as they do alone (TSan's check).
void concurrent() {
	std::mt19937_64 r(5);
	const auto docs = random_docs(r, 4, 600, false);
	const auto set = build_shards(docs, 4, r, 3);
	const multi_snapshot snap(set.inputs());
	std::map<std::uint64_t, std::string> key_of;
	for (const auto& d : docs) { key_of[interleave_docid(d.did, d.shard, 4)] = d.key; }
	std::vector<query> queries = {{"OR", {"a", "b"}}, {"AND", {"a", "c"}}, {"TERM", {"rare"}}, {"PHRASE", {"a", "b"}}, {"AND_NOT", {"c", "d"}}};
	auto run = [&](const query& q, bool keyed) {
		const auto b = builder_for(q, snap, true);
		search_options o;
		o.k = 20;
		o.count = true;
		if (keyed) { o.key = [&](const match& m) { return key_of.at(m.docid); }; }
		std::uint64_t seen = 0;
		o.visit = [&](const match&) { ++seen; };
		auto got = search(snap, b, o);
		CHECK(got.count && *got.count == seen);
		return got;
	};
	std::vector<search_result> alone;
	for (const auto& q : queries) {
		alone.push_back(run(q, false));
		alone.push_back(run(q, true));
	}
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&, t] {
			for (int round = 0; round < 30; ++round) {
				for (std::size_t i = 0; i < queries.size(); ++i) {
					const auto got = run(queries[(i + std::size_t(t)) % queries.size()], round % 2);
					const auto& want = alone[((i + std::size_t(t)) % queries.size()) * 2 + std::size_t(round % 2)];
					CHECK(got.hits == want.hits && got.count == want.count);
				}
			}
		});
	}
	for (auto& th : threads) { th.join(); }
}

// --- 6. what the exhaustive path skips, and bounds over empty segments ------------------------------

// Forwards a scorer and counts its score() calls.
class counting_scorer final : public scorer {
public:
	counting_scorer(scorer_ptr inner, std::uint64_t* calls) : inner_(std::move(inner)), calls_(calls) {}
	ordinal_t doc() const noexcept override { return inner_->doc(); }
	ordinal_t next() override { return inner_->next(); }
	ordinal_t advance(ordinal_t t) override { return inner_->advance(t); }
	std::uint64_t cost() const noexcept override { return inner_->cost(); }
	double score() override {
		++*calls_;
		return inner_->score();
	}
	double max_score(ordinal_t from, ordinal_t upto) override { return inner_->max_score(from, upto); }
	ordinal_t block_end(ordinal_t t) override { return inner_->block_end(t); }
	double max_score_total() const noexcept override { return inner_->max_score_total(); }
	std::size_t score_terms() const noexcept override { return inner_->score_terms(); }

private:
	scorer_ptr inner_;
	std::uint64_t* calls_;
};

void lazy_scoring() {
	std::mt19937_64 r(9);
	const auto docs = random_docs(r, 3, 900, false);
	const auto set = build_shards(docs, 3, r, 2);
	const multi_snapshot snap(set.inputs());
	std::map<std::uint64_t, std::string> key_of;
	for (const auto& d : docs) { key_of[interleave_docid(d.did, d.shard, 3)] = d.key; }
	const query q{"TERM", {"a"}};
	std::uint64_t calls = 0;
	const auto b = [&](const segment& seg) -> scorer_ptr {
		auto inner = build(seg, q, snap, true);
		return inner ? std::make_unique<counting_scorer>(std::move(inner), &calls) : nullptr;
	};
	search_options o;
	o.k = 5;
	o.key = [&](const match& m) { return key_of.at(m.docid); };
	const auto got = search(snap, b, o);
	CHECK(got.count && *got.count > 300);
	// Once the heap holds five documents with the smallest keys, a document with a larger key is
	// rejected on its key alone: most matches are never scored.
	CHECK(calls < *got.count / 2);
	// Without a key every kept candidate is scored, and a visitor that asks for no score costs none.
	calls = 0;
	o.key = nullptr;
	o.k = 0;
	o.visit = [](const match&) {};
	CHECK(search(snap, b, o).count == got.count && calls == 0);
}

// A segment with no documents, and a shard holding only deleted ones, leave the bounds to the others.
void empty_segments() {
	doc d;
	d.shard = 0;
	d.did = 1;
	d.terms = {"a", "b", "c", "d", "e"};
	doc e = d;
	e.did = 2;
	e.terms = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
	built b0, b1;
	segment_writer w;
	b0.bytes.push_back(w.flush({}));
	b0.segs.push_back(std::make_unique<segment>(b0.bytes.back()));
	b0.deleted.emplace_back();
	b0.dead.push_back(0);
	b0.docids.emplace_back();
	add_segment(b0, {{1, &d}}, {});
	add_segment(b1, {{1, &e}}, {1});  // its only document is deleted: still a bound
	const multi_snapshot snap(std::vector<shard_input>{{b0.snapshots(), 0}, {b1.snapshots(), 0}});
	CHECK((snap.doc_length_bounds() == length_bounds{5, 9}));
	CHECK(snap.stats().doc_count == 2 && snap.last_docid() == 2);
	const multi_snapshot none(std::vector<shard_input>{{}, {}});
	CHECK((none.doc_length_bounds() == length_bounds{0, 0}) && none.last_docid() == 0 && none.stats().doc_count == 0);
	// Searching nothing finds nothing.
	const auto b = [&](const segment&) -> scorer_ptr { return nullptr; };
	search_options o;
	o.count = true;
	CHECK(search(none, b, o).hits.empty() && search(none, b, o).count == 0);
	o.visit = [](const match&) {};
	CHECK(search(none, b, o).hits.empty() && search(none, b, o).count == 0);
}

// --- 5. fields by name (fields-and-keys T5b, D14) ---------------------------------------------------

// Two shards that declare their fields in different orders (and one field only one shard has)
// answer as one index holding every document: each shard resolves a query's field names to its
// own ids, and the statistics are summed by name.
struct named_index {
	strata::faultfs::filesystem fs;
	strata::faultfs::io io{fs};
	std::optional<prospect::index> ix;
	std::vector<std::string> order;  // the field names, in declaration order
	named_index(std::vector<std::string> names, const std::array<char, 16>& uuid) : order(std::move(names)) {
		ix.emplace(prospect::index::create(io, prospect::index::options{}, uuid));
	}
	strata::field_ref ref(const std::string& name) const {
		return strata::field_ref::declared(std::uint32_t(std::find(order.begin(), order.end(), name) - order.begin()));
	}
	void add_all(const std::vector<std::map<std::string, std::vector<std::pair<std::string, std::uint32_t>>>>& docs) {
		prospect::transaction t;
		for (const auto& n : order) { t.fields.push_back({n, strata::field_role::indexed, strata::field_postings::freqs, false}); }
		for (const auto& d : docs) {
			prospect::operation op;
			op.type = prospect::operation::add;
			for (const auto& [name, terms] : d) {
				auto& part = op.doc.field(ref(name));
				for (const auto& [term, wdf] : terms) {
					part.terms.push_back({term, wdf, {}});
					part.length += wdf;
				}
			}
			t.ops.push_back(std::move(op));
		}
		CHECK(ix->apply(t, {}).result == strata::verdict::accepted);
		ix->commit();
	}
	shard_input input() const {
		shard_input in;
		for (const auto& s : ix->segments()) { in.segments.push_back({s.seg(), s.deleted, s.dead, &ix->fields()}); }
		return in;
	}
};

using named_doc = std::map<std::string, std::vector<std::pair<std::string, std::uint32_t>>>;

struct field_query {
	bool all;  // AND, else OR
	std::vector<std::pair<std::string, std::string>> clauses;  // (field, term)
};

scorer_ptr build_fields(const segment& seg, const field_query& q, term_table& table, bool xapian) {
	std::vector<scorer_ptr> kids;
	for (const auto& [field, term] : q.clauses) {
		const auto* info = table.info(seg, field, term);
		if (!info) {
			if (q.all) { return nullptr; }
			continue;
		}
		const auto w = xapian ? weight::bm25_xapian(table.stats(), table.df(field, term)) : weight::bm25(table.stats(), table.df(field, term));
		kids.push_back(std::make_unique<term_scorer>(seg.get<prospect::inverted_plugin>(), *info, w));
	}
	if (kids.empty()) { return nullptr; }
	if (kids.size() == 1) { return std::move(kids[0]); }
	if (q.all) { return std::make_unique<conjunction_scorer>(std::move(kids)); }
	return std::make_unique<disjunction_scorer>(std::move(kids));
}

void fields_by_name() {
	static const std::array<char, 16> uuid_a{'s', 'h', 'a', 'r', 'd', 'a'}, uuid_b{'s', 'h', 'a', 'r', 'd', 'b'}, uuid_one{'o', 'n', 'e'};
	static const std::vector<std::string> words = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
	static const std::vector<std::string> names = {"title", "body", "extra"};
	std::mt19937_64 r(5150);
	std::vector<named_doc> docs;
	for (std::size_t k = 0; k < 300; ++k) {
		named_doc d;
		for (const auto& n : names) {
			if (n == "extra" && k % 2 == 0) { continue; }  // only the second shard has the field
			if (r() % 4 == 0) { continue; }
			std::set<std::string> chosen;
			for (std::size_t m = 1 + r() % 6; m > 0; --m) { chosen.insert(words[r() % words.size()]); }
			for (const auto& w : chosen) { d[n].push_back({w, 1 + std::uint32_t(r() % 3)}); }
		}
		docs.push_back(std::move(d));
	}
	named_index a({"title", "body"}, uuid_a), b({"extra", "title", "body"}, uuid_b), one({"title", "body", "extra"}, uuid_one);
	std::vector<named_doc> for_a, for_b;
	for (std::size_t k = 0; k < docs.size(); ++k) { (k % 2 ? for_b : for_a).push_back(docs[k]); }
	a.add_all(for_a);
	b.add_all(for_b);
	one.add_all(docs);  // document k has docid k + 1, which is its interleaved docid across the two shards
	CHECK(a.ix->fields().find("title") == 0u && b.ix->fields().find("title") == 1u && one.ix->fields().find("title") == 0u);
	CHECK(a.ix->fields().find("body") == 1u && b.ix->fields().find("body") == 2u && !a.ix->fields().find("extra"));
	const multi_snapshot sharded(std::vector<shard_input>{a.input(), b.input()});
	const multi_snapshot single(std::vector<shard_input>{one.input()});
	std::size_t compared = 0, nonempty = 0;
	for (std::size_t round = 0; round < 120; ++round) {
		field_query q;
		q.all = r() % 3 == 0;
		for (std::size_t c = 1 + r() % 3; c > 0; --c) { q.clauses.push_back({names[r() % names.size()], words[r() % words.size()]}); }
		for (const bool xapian : {false, true}) {
			for (int path = 0; path < 3; ++path) {
				search_options so;
				so.k = 10;
				so.count = true;
				if (path == 1) { so.key = [](const match& m) { return std::to_string(m.docid % 5); }; }  // the exhaustive path
				if (path == 2) { so.order = docid_order::descending; }
				term_table ts(sharded.segments()), t1(single.segments());
				const auto got = search(sharded, [&](const segment& seg) { return build_fields(seg, q, ts, xapian); }, so);
				const auto want = search(single, [&](const segment& seg) { return build_fields(seg, q, t1, xapian); }, so);
				CHECK(got.count == want.count);
				CHECK(got.hits.size() == want.hits.size());
				for (std::size_t i = 0; i < std::min(got.hits.size(), want.hits.size()); ++i) {
					CHECK(got.hits[i].docid == want.hits[i].docid);
					CHECK(std::fabs(got.hits[i].score - want.hits[i].score) <= 1e-12 * std::max(1.0, std::fabs(want.hits[i].score)));
				}
				++compared;
				nonempty += !want.hits.empty();
			}
		}
	}
	CHECK(compared == 720 && nonempty > 600);
	// A field one shard lacks has the other's documents only, and a name nobody declared has none.
	CHECK(sharded.df("extra", "a") == single.df("extra", "a") && sharded.df("extra", "a") > 0 && sharded.df("nope", "a") == 0);
}

}  // namespace

int main(int argc, char** argv) {
	numbering();
	const std::string dir = argc > 1 ? argv[1] : PROSPECT_FIXTURES "/xapian-shards";
	oracle(dir);
	identity(std::getenv("SHARDS_ROUNDS") ? std::atoi(std::getenv("SHARDS_ROUNDS")) : 60);
	visitor(40);
	concurrent();
	lazy_scoring();
	empty_segments();
	fields_by_name();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("shards_test ok\n");
	return 0;
}
