// Supplied statistics (include/prospect/searcher.hh, docs/mechanisms/supplied-statistics.md):
//   1. A fan-out weighted with supplied statistics answers as one index holding every shard's
//      documents: shards of the oracle corpora (3 and 5 shards) and random corpora, split over
//      segments, with deletions, for both weights and every query shape; each shard searched alone
//      with the sum of the shards' gathered statistics, its hits merged, equal the merged index's
//      bit for bit, and the supplied sum equals the merged index's own gather.
//   2. Without them the shards disagree with the merged index (the test has power), and a
//      shard's own statistics supplied change nothing.
//   3. The input's edges: terms by field, a term the set lacks, and a table left usable by a refusal.
#include "shards_fixture.hh"
#include "text_field.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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

using namespace shards_fixture;
using shards_fixture::built;

// --- the corpora and queries ------------------------------------------------------------------------

std::vector<std::string> read_lines(const std::string& path) {
	std::ifstream in(path);
	std::vector<std::string> out;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty()) { out.push_back(line); }
	}
	return out;
}

std::string field_of(const std::string& line, const std::string& key) {
	const std::size_t at = line.find("\"" + key + "\":");
	if (at == std::string::npos) { return {}; }
	std::size_t a = at + key.size() + 3, b = a;
	if (line[a] == '"') { return line.substr(a + 1, line.find('"', a + 1) - a - 1); }
	while (b < line.size() && line[b] != ',' && line[b] != '}' && line[b] != ']') { ++b; }
	return line.substr(a, b - a);
}

std::string unhex(const std::string& h) {
	std::string out;
	for (std::size_t i = 0; i + 1 < h.size(); i += 2) { out.push_back(char(std::stoi(h.substr(i, 2), nullptr, 16))); }
	return out;
}

// The oracle's corpus of `shards` shards (test/fixtures/xapian-shards), and its queries.
struct corpus {
	std::size_t shards = 0;
	std::vector<doc> docs;
	std::vector<query> queries;
};

corpus read_corpus(const std::string& dir, const std::string& name) {
	corpus c;
	const auto lines = read_lines(dir + "/shards-" + name + ".jsonl");
	c.shards = std::stoul(field_of(lines.at(0), "shards"));
	for (std::size_t i = 1; i < lines.size(); ++i) {
		doc d;
		d.shard = std::stoul(field_of(lines[i], "shard"));
		d.did = std::stoull(field_of(lines[i], "did"));
		d.key = unhex(field_of(lines[i], "key"));
		for (std::size_t at = lines[i].find("\"terms\":[") + 9; lines[i][at] != ']';) {
			const std::size_t a = lines[i].find('"', at), b = lines[i].find('"', a + 1);
			d.terms.push_back(lines[i].substr(a + 1, b - a - 1));
			at = lines[i][b + 1] == ',' ? b + 2 : b + 1;
		}
		c.docs.push_back(std::move(d));
	}
	std::set<std::pair<std::string, std::vector<std::string>>> seen;
	for (const auto& line : read_lines(dir + "/shards-" + name + ".out.jsonl")) {
		if (!line.starts_with("{\"query\"")) { continue; }
		query q = parse_query(field_of(line, "query"));
		if (seen.insert({q.kind, q.terms}).second) { c.queries.push_back(std::move(q)); }
	}
	return c;
}

// --- builders over a term table ----------------------------------------------------------------------

// `lucene` scores a phrase as Lucene does: the phrase frequency with the summed idf, from the
// table's statistics and document frequencies; otherwise each term is weighed on its own.
scorer_ptr build_table(const segment& seg, const query& q, term_table& table, bool xapian, bool lucene = false) {
	const statistics st = table.stats();
	auto weight_of = [&](const std::string& t) {
		const std::uint64_t df = table.df(text_name, t);
		return xapian ? weight::bm25_xapian(st, df) : weight::bm25(st, df);
	};
	auto term = [&](const std::string& t) -> scorer_ptr {
		auto info = seg.get<inverted_plugin>().term(0, t);
		return info ? std::make_unique<term_scorer>(seg.get<inverted_plugin>(), *info, weight_of(t)) : nullptr;
	};
	std::vector<scorer_ptr> kids;
	if (q.kind == "TERM") { return term(q.terms[0]); }
	if (q.kind == "OR" || q.kind == "AND") {
		for (const auto& t : q.terms) {
			auto s = term(t);
			if (!s && q.kind == "AND") { return nullptr; }
			if (s) { kids.push_back(std::move(s)); }
		}
		if (kids.empty()) { return nullptr; }
		if (kids.size() == 1) { return std::move(kids[0]); }
		if (q.kind == "AND") { return std::make_unique<conjunction_scorer>(std::move(kids)); }
		return std::make_unique<disjunction_scorer>(std::move(kids));
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
	std::vector<std::uint64_t> dfs;
	std::vector<weight> ws;
	for (const auto& t : q.terms) {
		auto info = seg.get<inverted_plugin>().term(0, t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
		dfs.push_back(table.df(text_name, t));
		ws.push_back(weight_of(t));
	}
	if (lucene) { return std::make_unique<phrase_scorer>(seg.get<inverted_plugin>(), infos, std::vector<weight>{}, 0, &st, dfs); }
	return std::make_unique<phrase_scorer>(seg.get<inverted_plugin>(), infos, std::move(ws));
}

std::vector<term_name> names_of(const query& q) {
	std::vector<term_name> out;
	for (const auto& t : q.terms) { out.push_back({text_name, t}); }
	return out;
}

// A shard-by-shard fan-out: each shard searched alone through `stats_for` (null: its own
// statistics), its hits numbered across the shards and merged best first, ties by docid.
struct fanout_result {
	std::vector<doc_hit> hits;
	std::uint64_t count = 0;
};

fanout_result fan_out(const shard_set& set, const query& q, bool xapian, bool lucene, std::size_t k, const supplied_statistics* supplied) {
	fanout_result out;
	const std::size_t n = set.shards.size();
	for (std::size_t s = 0; s < n; ++s) {
		const auto segs = set.shards[s].snapshots();
		term_table table(segs, supplied);
		std::uint64_t c = 0;
		for (const auto& h : search(segs, [&](const segment& seg) { return build_table(seg, q, table, xapian, lucene); }, k, &c)) { out.hits.push_back({interleave_docid(h.docid, s, n), h.score}); }
		out.count += c;
	}
	std::sort(out.hits.begin(), out.hits.end(), [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
	if (out.hits.size() > k) { out.hits.resize(k); }
	return out;
}

// --- 1 and 2. identity with the merged index -----------------------------------------------------------

struct tally {
	std::size_t compared = 0, differ_without = 0, nonempty = 0, supplied_runs = 0;
};

void check_split(const std::vector<doc>& docs, std::size_t n, const std::vector<query>& queries, std::mt19937_64& r, unsigned parts, bool deletes, tally& t) {
	std::set<std::pair<std::size_t, std::uint64_t>> dead;
	std::set<std::uint64_t> dead_merged;
	if (deletes) {
		for (const auto& d : docs) {
			if (r() % 8 == 0) {
				dead.insert({d.shard, d.did});
				dead_merged.insert(interleave_docid(d.did, d.shard, n));
			}
		}
	}
	const auto set = build_shards(docs, n, r, parts, dead);
	built merged;
	{
		std::vector<std::pair<std::uint64_t, const doc*>> group;
		for (const auto& d : docs) { group.push_back({interleave_docid(d.did, d.shard, n), &d}); }
		std::sort(group.begin(), group.end());
		add_segment(merged, group, dead_merged);
	}
	const auto one = merged.snapshots();
	for (const auto& q : queries) {
		const auto names = names_of(q);
		// The statistics round trip: each shard gathers its own, the coordinator adds them.
		supplied_statistics sum;
		for (const auto& b : set.shards) { sum += gather_statistics(b.snapshots(), names); }
		const auto want_stats = gather_statistics(one, names);
		CHECK(sum == want_stats);
		term_table whole(one);
		for (const auto& nm : names) {
			CHECK(whole.cf(nm.field, nm.term) == want_stats.find(nm.field, nm.term)->cf);
			// A shard's table with the sum weighs as the merged index's.
			for (const auto& b : set.shards) {
				const auto segs = b.snapshots();
				term_table mine(segs, &sum);
				CHECK(mine.df(nm.field, nm.term) == whole.df(nm.field, nm.term) && mine.cf(nm.field, nm.term) == whole.cf(nm.field, nm.term));
				CHECK(mine.stats().doc_count == whole.stats().doc_count && mine.stats().total_length == whole.stats().total_length);
				for (const bool xapian : {false, true}) {
					const auto a = xapian ? weight::bm25_xapian(mine.stats(), mine.df(nm.field, nm.term)) : weight::bm25(mine.stats(), mine.df(nm.field, nm.term));
					const auto c = xapian ? weight::bm25_xapian(whole.stats(), whole.df(nm.field, nm.term)) : weight::bm25(whole.stats(), whole.df(nm.field, nm.term));
					CHECK(a.idf == c.idf && a.avg == c.avg && a.inv_avg == c.inv_avg);
					for (const std::uint32_t tf : {1u, 2u, 5u}) {
						for (const std::uint32_t len : {1u, 7u, 40u}) { CHECK(a(tf, len) == c(tf, len)); }
					}
				}
			}
		}
		for (const bool xapian : {false, true}) {
			for (const bool lucene : {false, true}) {
				if (lucene && (xapian || q.kind != "PHRASE")) { continue; }
				for (const std::size_t k : {std::size_t(1), std::size_t(10), std::size_t(5000)}) {
					term_table whole_table(one);
					std::uint64_t ref_count = 0;
					const auto ref = search(one, [&](const segment& seg) { return build_table(seg, q, whole_table, xapian, lucene); }, k, &ref_count);
					const auto got = fan_out(set, q, xapian, lucene, k, &sum);
					CHECK(got.hits == ref);  // ids, order and score bits
					CHECK(got.count == ref_count);
					// A shard's own statistics supplied are the statistics it has.
					if (k == 10) {
						std::vector<doc_hit> own;
						const std::size_t shards = set.shards.size();
						for (std::size_t s = 0; s < shards; ++s) {
							const auto segs = set.shards[s].snapshots();
							const auto mine = gather_statistics(segs, names);
							term_table a(segs, &mine), b(segs);
							const auto x = search(segs, [&](const segment& seg) { return build_table(seg, q, a, xapian, lucene); }, k);
							const auto y = search(segs, [&](const segment& seg) { return build_table(seg, q, b, xapian, lucene); }, k);
							CHECK(x == y);
							++t.supplied_runs;
						}
						// Without them the shards weigh by their own: the test sees the difference.
						const auto local = fan_out(set, q, xapian, lucene, k, nullptr);
						t.differ_without += local.hits != ref;
					}
					++t.compared;
					t.nonempty += !ref.empty();
				}
			}
		}
	}
}

void identity(const std::string& dir) {
	tally t;
	std::size_t rows = 0;
	for (const char* name : {"s3", "s5"}) {
		const corpus c = read_corpus(dir, name);
		CHECK(!c.docs.empty() && !c.queries.empty());
		std::vector<query> qs = c.queries;
		if (qs.size() > 400) { qs.resize(400); }
		rows += qs.size();
		for (const unsigned parts : {1u, 3u}) {
			for (const bool deletes : {false, true}) {
				std::mt19937_64 r(900 + parts + deletes);
				check_split(c.docs, c.shards, qs, r, parts, deletes, t);
			}
		}
	}
	// Random corpora: skewed shards, empty shards, 3 and 5 of them.
	std::mt19937_64 r(2026);
	for (int round = 0; round < 8; ++round) {
		const std::size_t n = round % 2 ? 5 : 3;
		const auto docs = random_docs(r, n, 150 + r() % 300, round % 3 == 0, round % 4 == 2);
		std::vector<query> qs;
		const char* kinds[] = {"TERM", "OR", "AND", "AND_MAYBE", "AND_NOT", "PHRASE"};
		for (int i = 0; i < 40; ++i) {
			query q;
			q.kind = kinds[r() % 6];
			const std::size_t want = q.kind == "TERM" ? 1 : q.kind == "PHRASE" ? 2 + r() % 2 : q.kind == "OR" || q.kind == "AND" ? 2 + r() % 3 : 2;
			while (q.terms.size() < want) { q.terms.push_back(vocab[r() % 9]); }
			qs.push_back(std::move(q));
		}
		check_split(docs, n, qs, r, 1 + round % 4, round % 2 == 1, t);
	}
	std::printf("  identity: %zu fan-outs equal the merged index (%zu with hits), %zu corpus rows, %zu queries differ without supplied statistics\n", t.compared, t.nonempty, rows, t.differ_without);
	CHECK(t.compared > 3000 && t.nonempty > 2500);
	CHECK(t.differ_without > 200);  // power: unsupplied shards rank differently
	CHECK(t.supplied_runs > 2000);
}

// --- 3. the input's edges -----------------------------------------------------------------------------

void edges() {
	const std::vector<doc> docs = {{0, 1, "", {"a", "b", "a"}}, {0, 2, "", {"b", "c"}}, {0, 3, "", {"a"}}};
	std::mt19937_64 r(1);
	const auto set = build_shards(docs, 1, r, 1);
	const auto segs = set.shards[0].snapshots();
	// Statistics by field and term; the local figures are a, df 2 cf 3.
	const auto local = gather_statistics(segs, std::vector<term_name>{{text_name, "a"}, {text_name, "zz"}});
	CHECK(local.collection.doc_count == 3 && local.collection.total_length == 6);
	CHECK((*local.find(text_name, "a") == term_statistics{2, 3}) && (*local.find(text_name, "zz") == term_statistics{}) && !local.find(text_name, "b") && !local.find("other", "a"));
	supplied_statistics s;
	s.collection = {1000, 25000};
	s.terms[text_name]["a"] = {400, 900};
	term_table t(segs, &s);
	CHECK(t.stats().doc_count == 1000 && t.stats().total_length == 25000);
	CHECK(t.df(text_name, "a") == 400 && t.cf(text_name, "a") == 900);
	// The segment's own frequencies stay the segment's.
	CHECK(t.df(*segs[0].seg, text_name, "a") == 2);
	// A term absent from the snapshot and from the set is absent: df 0.
	CHECK(t.df(text_name, "absent") == 0 && t.cf(text_name, "absent") == 0);
	// A term the snapshot holds and the set lacks is refused, and the table stays usable.
	auto refused = [&](auto f) {
		try { f(); } catch (const std::invalid_argument&) { return true; }
		return false;
	};
	CHECK(refused([&] { (void)t.df(text_name, "b"); }));
	CHECK(refused([&] { (void)t.df(text_name, "b"); }));  // still refused, not remembered as 0
	CHECK(t.df(text_name, "a") == 400);
	// The set is keyed by field: a term under another field name is not this field's.
	supplied_statistics other;
	other.terms["other"]["a"] = {9, 9};
	term_table u(segs, &other);
	CHECK(refused([&] { (void)u.df(text_name, "a"); }));
	CHECK(u.df("other", "a") == 9);  // a supplied term is the caller's even where the snapshot lacks the field
	// Sums add collection and terms, and a missing side is zero.
	supplied_statistics x = local, y;
	y.collection = {2, 4};
	y.terms[text_name]["b"] = {1, 1};
	x += y;
	CHECK(x.collection.doc_count == 5 && x.collection.total_length == 10);
	CHECK((*x.find(text_name, "a") == term_statistics{2, 3}) && (*x.find(text_name, "b") == term_statistics{1, 1}));
	// A weight from supplied statistics is the weight of a collection that size.
	const auto w = weight::bm25(t.stats(), t.df(text_name, "a"));
	CHECK(w.idf == weight::bm25(statistics(1000, 25000), 400).idf && w.avg == 25.0);
}

}  // namespace

int main(int argc, char** argv) {
	const std::string dir = argc > 1 ? argv[1] : PROSPECT_FIXTURES "/xapian-shards";
	identity(dir);
	edges();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("stats_test ok\n");
	return 0;
}
