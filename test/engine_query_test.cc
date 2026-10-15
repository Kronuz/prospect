// The benchmark engine's query layer (tools/engine_query.hh) over an index's segments, as
// serve and churn answer queries:
//   - Without deletes, many segments answer every query exactly as one segment holding
//     the same documents: weights come from the snapshot's statistics, not each segment's
//     (review R5: churn used per-segment statistics).
//   - With deletes in some segments, the top k agrees with an exhaustive scan of the live
//     documents under the same weights, ties by docid, and every count agrees.
#include "engine_query.hh"
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace strata;

static const std::array<char, 16> uuid = {'e', 'n', 'g', 'i', 'n', 'e', 'q', 'u', 'e', 'r', 'y', 't', 'e', 's', 't', '1'};
static const char* vocab[] = {"a", "b", "c", "d", "e", "f", "rare"};

static document random_doc(std::mt19937_64& r) {
	document d;
	std::uint32_t pos = 0;
	for (const char* w : vocab) {
		const bool rare = std::string_view(w) == "rare";
		if (r() % 100 >= (rare ? 3u : 35u)) { continue; }
		term_entry t{w, 1 + std::uint32_t(r() % 3), {}};
		for (std::uint32_t k = 0; k < t.wdf; ++k) { t.positions.push_back(pos += 1 + std::uint32_t(r() % 2)); }
		d.length += t.wdf;
		d.terms.push_back(std::move(t));
	}
	if (d.terms.empty()) { d.terms.push_back({"a", 1, {1}}), d.length = 1; }
	return d;
}

// Words, +required, -excluded, and "phrases", in the harness's syntax.
static std::string random_query(std::mt19937_64& r) {
	std::string q;
	const std::size_t clauses = 1 + r() % 3;
	for (std::size_t i = 0; i < clauses; ++i) {
		if (!q.empty()) { q += ' '; }
		switch (r() % 6) {
		case 0: q += '+'; break;
		case 1: q += '-'; break;
		case 2: q += '"' + std::string(vocab[r() % 7]) + ' ' + vocab[r() % 7] + '"'; continue;
		default: break;
		}
		q += vocab[r() % 7];
	}
	return q;
}

static std::vector<segment_snapshot> snapshot(const strata::index& ix) {
	std::vector<segment_snapshot> out;
	for (const auto& s : ix.segments()) { out.push_back({s.seg, s.deleted, s.dead}); }
	return out;
}

// Every live match, scored with the snapshot's statistics, best first (ties by docid).
static std::vector<doc_hit> scan(std::span<const segment_snapshot> segs, const engine::parsed& q, scheme sch) {
	engine::snapshot_stats snap(segs);
	std::vector<doc_hit> all;
	for (const auto& s : segs) {
		auto sc = engine::build(*s.seg, q, sch, &snap);
		if (!sc) { continue; }
		for (ordinal_t o = sc->next(); o != no_more_docs; o = sc->next()) {
			if (s.deleted && ((*s.deleted)[o >> 6] >> (o & 63)) & 1) { continue; }
			all.push_back({s.seg->docid(o), sc->score()});
		}
	}
	std::sort(all.begin(), all.end(), [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
	return all;
}

static void check_against_scan(const std::vector<doc_hit>& hits, const std::vector<doc_hit>& all, std::size_t k) {
	auto close = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(a, b)); };
	const std::size_t n = std::min(k, all.size());
	CHECK(hits.size() == n);
	for (std::size_t a = 0; a < n && hits.size() == n;) {
		std::size_t b = a + 1;
		while (b < n && close(all[b].score, all[a].score)) { ++b; }
		std::vector<std::uint64_t> want_ids, got_ids;
		for (std::size_t i = a; i < b; ++i) {
			CHECK(close(hits[i].score, all[i].score));
			want_ids.push_back(all[i].docid);
			got_ids.push_back(hits[i].docid);
		}
		std::sort(got_ids.begin(), got_ids.end());
		bool exact = true;
		for (std::size_t i = a; i < b; ++i) { exact = exact && all[i].score == all[a].score; }
		if (exact) { CHECK(got_ids == want_ids); }
		a = b;
	}
}

int main() {
	std::mt19937_64 r(17);
	for (int round = 0; round < 6; ++round) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options opts;
		opts.auto_merge = false;
		auto ix = strata::index::create(io, opts, uuid);
		const std::size_t docs = 2000 + r() % 3000;
		const bool deletes = round % 2 == 1;
		std::vector<document> all;
		for (std::size_t d = 0; d < docs; ++d) {
			all.push_back(random_doc(r));
			transaction t;
			operation o;
			o.doc = all.back();
			t.ops.push_back(o);
			ix.apply(t, {});
			if (d % 400 == 399) { ix.refresh(); }
		}
		ix.refresh();
		if (deletes) {
			// Deletes in the older half of the docids only, so some segments keep none.
			for (std::size_t i = 0; i < docs / 10; ++i) {
				transaction t;
				operation o;
				o.type = operation::delete_docid;
				o.docid = 1 + r() % (docs / 2);
				t.ops.push_back(o);
				ix.apply(t, {});
			}
			ix.refresh();
		}
		const auto segs = snapshot(ix);
		CHECK(segs.size() > 3);
		std::size_t with_deletes = 0;
		for (const auto& s : segs) { with_deletes += s.any_deleted(); }
		CHECK(deletes ? with_deletes > 0 && with_deletes < segs.size() : with_deletes == 0);
		// The same documents in one segment, docids as the index assigned them.
		segment_writer one;
		for (std::size_t d = 0; d < docs; ++d) {
			std::vector<posting_input> in;
			for (const auto& t : all[d].terms) { in.push_back({t.term, t.wdf, t.positions}); }
			one.add(d + 1, in, all[d].length);
		}
		const std::string one_bytes = one.flush({});
		const segment single(one_bytes);
		const segment_snapshot single_snap[] = {{&single, nullptr, 0}};
		for (int qn = 0; qn < 150; ++qn) {
			const std::string text = random_query(r);
			const engine::parsed q = engine::parse(text);
			const scheme sch = qn % 2 ? scheme::bm25_xapian : scheme::bm25;
			const auto want = scan(segs, q, sch);
			const std::uint64_t counted = engine::count_snapshot(segs, q, sch);
			CHECK(counted == want.size());
			if (sch == scheme::bm25) {
				CHECK(engine::run(std::span<const segment_snapshot>(segs), "COUNT", text) == counted);
				CHECK(engine::run(std::span<const segment_snapshot>(segs), "TOP_10_COUNT", text) == counted);
			}
			for (std::size_t k : {1u, 10u, 100u, 1000u}) {
				std::uint64_t n = 0;
				const auto got = engine::search_snapshot(segs, q, k, sch, &n);
				CHECK(n == counted);
				check_against_scan(got, want, k);
				check_against_scan(engine::search_snapshot(segs, q, k, sch), want, k);
				if (!deletes) {
					std::uint64_t single_n = 0;
					CHECK(engine::search_snapshot(single_snap, q, k, sch, &single_n) == got);
					CHECK(single_n == n);
				}
			}
		}
	}
	std::printf("engine_query: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
