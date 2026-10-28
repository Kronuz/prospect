// Search across segments, under both weighting presets:
//   - Without deletes, an index of many flushes answers every query exactly as one segment
//     holding the same documents: the same docids, scores, ties, and counts.
//   - With deletes, pruned search across segments equals an exhaustive scan of every
//     segment's live matches, ties by docid, and counts equal.
#include "prospect/index.hh"
#include "prospect/searcher.hh"
#include "strata/core/budget.hh"
#include "strata/core/faultfs.hh"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace prospect;

static const std::array<char, 16> uuid = {'m', 'u', 'l', 't', 'i', 's', 'e', 'g', 'm', 'e', 'n', 't', 't', 'e', 's', 't'};
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
		d.field().terms.push_back(std::move(t));
	}
	if (d.field().terms.empty()) { d.field().terms.push_back({"a", 1, {1}}), d.length = 1; }
	return d;
}

// A query: kind 0 OR, 1 AND, 2 AND_MAYBE, 3 AND_NOT, 4 phrase, 5 exclusion chain.
struct query {
	int kind;
	std::vector<std::string> terms;
};

static scorer_ptr build(const segment& seg, const query& q, const statistics& st, std::span<const segment_snapshot> segs, bool xapian) {
	auto weight_of = [&](const std::string& t) {
		const std::uint64_t df = snapshot_df(segs, t);
		return xapian ? weight::bm25_xapian(st, df) : weight::bm25(st, df);
	};
	auto term = [&](const std::string& t) -> scorer_ptr {
		auto info = seg.get<prospect::inverted_plugin>().term(0, t);
		return info ? std::make_unique<term_scorer>(seg.get<prospect::inverted_plugin>(), *info, weight_of(t)) : nullptr;
	};
	std::vector<scorer_ptr> kids;
	switch (q.kind) {
	case 0:
		for (auto& t : q.terms) {
			if (auto s = term(t)) { kids.push_back(std::move(s)); }
		}
		if (kids.empty()) { return nullptr; }
		if (kids.size() == 1) { return std::move(kids[0]); }
		return std::make_unique<disjunction_scorer>(std::move(kids));
	case 1:
		for (auto& t : q.terms) {
			auto s = term(t);
			if (!s) { return nullptr; }
			kids.push_back(std::move(s));
		}
		return kids.size() == 1 ? std::move(kids[0]) : std::make_unique<conjunction_scorer>(std::move(kids));
	case 2: {
		auto req = term(q.terms[0]), opt = term(q.terms[1]);
		if (!req) { return nullptr; }
		if (!opt) { return req; }
		return std::make_unique<required_optional_scorer>(std::move(req), std::move(opt));
	}
	case 3:
	case 5: {
		auto in = term(q.terms[0]);
		if (!in) { return nullptr; }
		for (std::size_t i = 1; i < (q.kind == 3 ? 2u : q.terms.size()); ++i) {
			if (auto ex = term(q.terms[i])) { in = std::make_unique<exclusion_scorer>(std::move(in), std::move(ex)); }
		}
		return in;
	}
	default: {
		std::vector<prospect::format::term_info> infos;
		std::vector<std::uint64_t> dfs;
		std::vector<weight> ws;
		for (auto& t : q.terms) {
			auto info = seg.get<prospect::inverted_plugin>().term(0, t);
			if (!info) { return nullptr; }
			infos.push_back(*info);
			dfs.push_back(snapshot_df(segs, t));
			ws.push_back(weight_of(t));
		}
		if (xapian) { return std::make_unique<phrase_scorer>(seg.get<prospect::inverted_plugin>(), infos, std::move(ws)); }
		return std::make_unique<phrase_scorer>(seg.get<prospect::inverted_plugin>(), infos, std::vector<weight>{}, 0, &st, dfs);
	}
	}
}

// Every live match of every segment, scored without pruning, best first (ties by docid).
static std::vector<doc_hit> scan(std::span<const segment_snapshot> segs, const scorer_builder& builder) {
	std::vector<doc_hit> all;
	for (const auto& s : segs) {
		auto sc = builder(*s.seg);
		if (!sc) { continue; }
		for (ordinal_t o = sc->next(); o != no_more_docs; o = sc->next()) {
			if (s.deleted && ((*s.deleted)[o >> 6] >> (o & 63)) & 1) { continue; }
			all.push_back({s.seg->core().docid(o), sc->score()});
		}
	}
	std::sort(all.begin(), all.end(), [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
	return all;
}

// Ranks agree with the scan on score; each run of equal scores is drawn from the scan's
// documents with that score, the smallest docids first (ties by docid), including a run
// cut at the k-th position.
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
		// Exactly equal scores must give exactly the scan's docids.
		bool exact = true;
		for (std::size_t i = a; i < b; ++i) { exact = exact && all[i].score == all[a].score; }
		if (exact) { CHECK(got_ids == want_ids); }
		a = b;
	}
}

static std::vector<segment_snapshot> snapshot(const prospect::index& ix) {
	std::vector<segment_snapshot> out;
	for (const auto& s : ix.segments()) { out.push_back({s.seg(), s.deleted, s.dead}); }
	return out;
}

int main() {
	std::mt19937_64 r(5);
	for (int round = 0; round < 12; ++round) {
		faultfs::filesystem fs, serial_fs;
		faultfs::io io(fs), serial_io(serial_fs);
		// Every third round inverts each flush on four threads (D18); a serial twin of the
		// same transactions must reach the same digest.
		index::options opts;
		if (round % 3 == 2) { opts.flush_threads = 4; opts.flush_min_docs = 64; }
		auto ix = prospect::index::create(io, opts, uuid);
		auto serial = prospect::index::create(serial_io, {}, uuid);
		const std::size_t docs = 2000 + r() % 6000;
		const bool deletes = round % 2 == 1;
		segment_writer one;
		std::vector<document> all;
		for (std::size_t d = 0; d < docs; ++d) {
			all.push_back(random_doc(r));
			transaction t;
			operation o;
			o.doc = all.back();
			t.ops.push_back(o);
			ix.apply(t, {});
			serial.apply(t, {});
			if (d % 500 == 499) { ix.refresh(); serial.refresh(); }
		}
		if (deletes) {
			// Deletes, and replacements that put small docids into newer segments, so ties
			// across segments are not settled by segment order.
			for (std::size_t i = 0; i < docs / 5; ++i) {
				transaction t;
				operation o;
				o.docid = 1 + r() % docs;
				if (r() % 2) {
					o.type = operation::delete_docid;
				} else {
					o.type = operation::replace_docid;
					o.doc = random_doc(r);
				}
				t.ops.push_back(o);
				ix.apply(t, {});
				serial.apply(t, {});
				if (r() % 100 == 0) { ix.refresh(); serial.refresh(); }
			}
		}
		ix.refresh();
		serial.refresh();
		CHECK(ix.digest() == serial.digest());
		if (round % 3 == 2) { CHECK(ix.segments().size() > serial.segments().size()); }
		const auto segs = snapshot(ix);
		// Each segment's dead count is its bitmap's population (review R5).
		for (const auto& v : ix.segments()) {
			std::uint64_t bits = 0;
			if (v.deleted) {
				for (auto w : *v.deleted) { bits += std::uint64_t(std::popcount(w)); }
			}
			CHECK(v.dead == bits);
		}
		const statistics st = snapshot_statistics(segs);
		CHECK(segs.size() > 3);
		// The same documents in one segment.
		for (std::size_t d = 0; d < docs; ++d) {
			std::vector<posting_input> in;
			for (const auto& t : all[d].field().terms) { in.push_back({t.term, t.wdf, t.positions}); }
			one.add(d + 1, in, all[d].length);
		}
		const std::string one_bytes = one.flush({});
		const segment single(one_bytes);
		const strata::deleted_bits none((single.core().doc_count() + 63) / 64);
		const segment_snapshot single_snap[] = {{&single, &none}};
		for (int qn = 0; qn < 120; ++qn) {
			query q;
			q.kind = int(r() % 6);
			const std::size_t n = q.kind == 4 ? 2 + r() % 2 : q.kind == 5 ? 3 : 2;
			std::set<std::string> used;
			while (q.terms.size() < n) {
				std::string t = vocab[r() % 7];
				if (q.kind == 4 || used.insert(t).second) { q.terms.push_back(t); }
			}
			const bool xapian = qn % 2 == 1;
			auto builder = [&](const segment& seg) { return build(seg, q, st, segs, xapian); };
			const auto all = scan(segs, builder);
			for (std::size_t k : {1u, 10u, 100u, 1000u}) {
				std::uint64_t counted = 0;
				const auto got = search(segs, builder, k, &counted);
				const auto pruned = search(segs, builder, k);
				CHECK(search_count(segs, builder) == counted);
				CHECK(counted == all.size());
				check_against_scan(got, all, k);
				check_against_scan(pruned, all, k);
				if (!deletes) {
					// One segment with equal statistics answers identically.
					auto single_builder = [&](const segment& seg) { return build(seg, q, st, single_snap, xapian); };
					std::uint64_t single_count = 0;
					CHECK(search(single_snap, single_builder, k, &single_count) == got);
					CHECK(single_count == counted);
				}
			}
		}
	}
	// Ties straddling the k-th position across segments, and floors that exclude whole
	// segments (P6-1, review R1): segments of identical documents, short ones scoring high
	// and long ones (a "pad" term) scoring low, with the smallest docids in a newer segment.
	{
		struct group {
			std::uint64_t first, n;
			bool low;
		};
		const group groups[] = {{1001, 1200, false}, {3001, 600, true}, {4001, 1200, false}, {1, 300, false}, {6001, 100, true}};
		static const char* shapes[3][3] = {{"a", "b", "c"}, {"a", "c", "d"}, {"a", "b", "d"}};
		static const std::uint32_t p1[] = {1}, p2[] = {2}, p3[] = {3};
		std::uint32_t pad[30];
		for (std::uint32_t i = 0; i < 30; ++i) { pad[i] = 4 + i; }
		std::vector<std::string> files;
		files.reserve(std::size(groups));
		std::vector<std::unique_ptr<segment>> owned;
		std::vector<segment_snapshot> segs;
		for (const auto& g : groups) {
			segment_writer w;
			for (std::uint64_t d = g.first; d < g.first + g.n; ++d) {
				const auto& sh = shapes[d % 3];
				std::vector<posting_input> in = {{sh[0], 1, p1}, {sh[1], 1, p2}, {sh[2], 1, p3}};
				if (g.low) { in.push_back({"pad", 30, pad}); }
				w.add(d, in, g.low ? 33u : 3u);
			}
			files.push_back(w.flush({}));
			owned.push_back(std::make_unique<segment>(files.back()));
			segs.push_back({owned.back().get(), nullptr});
		}
		const std::vector<segment_snapshot> reversed(segs.rbegin(), segs.rend());
		const std::vector<segment_snapshot>* const views[] = {&segs, &reversed};
		const statistics st = snapshot_statistics(segs);
		const std::vector<query> queries = {{0, {"a", "b"}}, {0, {"c", "d"}}, {1, {"a"}}, {1, {"a", "c"}}, {1, {"b", "d"}},
			{2, {"a", "b"}}, {2, {"c", "pad"}}, {3, {"a", "b"}}, {3, {"a", "pad"}}, {4, {"a", "b"}}, {4, {"c", "d"}},
			{5, {"a", "b", "pad"}}, {5, {"c", "b", "pad"}}};
		for (const bool xapian : {false, true}) {
			for (const auto& q : queries) {
				auto builder = [&](const segment& seg) { return build(seg, q, st, segs, xapian); };
				for (const auto* view : views) {
					const std::span<const segment_snapshot> v(*view);
					const auto all = scan(v, builder);
					for (std::size_t k : {1u, 10u, 100u, 1000u}) {
						std::uint64_t counted = 0;
						const auto got = search(v, builder, k, &counted);
						CHECK(counted == all.size() && search_count(v, builder) == counted);
						check_against_scan(got, all, k);
						check_against_scan(search(v, builder, k), all, k);
					}
				}
				// A segment given a floor returns no hit at or below it; the low segments
				// hold nothing above the high segment's k-th score for a single term.
				for (std::size_t k : {1u, 10u, 100u, 1000u}) {
					auto first = builder(*segs[0].seg);
					if (!first) { continue; }
					const auto top = top_k(*segs[0].seg, *first, k);
					if (top.size() < k) { continue; }
					const double floor = std::nextafter(top.back().score, -std::numeric_limits<double>::infinity());
					for (std::size_t g : {1u, 2u, 4u}) {
						auto sc = builder(*segs[g].seg);
						if (!sc) { continue; }
						const auto hits = top_k(*segs[g].seg, *sc, k, nullptr, floor);
						for (const auto& h : hits) { CHECK(h.score > floor); }
						if (q.kind == 1 && q.terms.size() == 1 && g != 2) { CHECK(hits.empty()); }
					}
				}
			}
		}
		// The single term "a": every short document ties, so the k-th position straddles
		// three segments and the smallest docids win, wherever their segment sits.
		auto builder = [&](const segment& seg) { return build(seg, query{1, {"a"}}, st, segs, false); };
		std::vector<std::uint64_t> want;
		for (std::uint64_t d = 1; d <= 300; ++d) { want.push_back(d); }
		for (std::uint64_t d = 1001; d <= 1700; ++d) { want.push_back(d); }
		for (const auto* view : views) {
			std::vector<std::uint64_t> got;
			for (const auto& h : search(std::span<const segment_snapshot>(*view), builder, 1000)) { got.push_back(h.docid); }
			CHECK(got == want);
		}
	}
	// any_deleted reads the dead count when it has one (review R5) and scans the bitmap
	// when the count is unknown.
	{
		strata::deleted_bits none(4), some(4);
		some.mutable_words()[3] = 1;
		segment_snapshot s{nullptr, nullptr};
		CHECK(s.dead == segment_snapshot::unknown_dead && !s.any_deleted());
		s.deleted = &none;
		CHECK(!s.any_deleted());
		s.deleted = &some;
		CHECK(s.any_deleted());
		s.dead = 0;
		CHECK(!s.any_deleted());
		s.deleted = &none;
		s.dead = 1;
		CHECK(s.any_deleted());
	}
	// live_scorer nested where parents use two-phase iteration, its counting copy, and
	// windows that do not start on a word.
	{
		std::mt19937_64 r(21);
		segment_writer w;
		std::vector<std::vector<std::string>> docs;
		for (std::uint32_t d = 0; d < 3000; ++d) {
			std::vector<std::string> tokens;
			for (std::size_t i = 0, n = 2 + r() % 8; i < n; ++i) { tokens.push_back(vocab[r() % 4]); }
			std::map<std::string, std::vector<std::uint32_t>> pos;
			for (std::uint32_t i = 0; i < tokens.size(); ++i) { pos[tokens[i]].push_back(i + 1); }
			std::vector<posting_input> in;
			for (auto& [t, p] : pos) { in.push_back({t, std::uint32_t(p.size()), p}); }
			w.add(d + 1, in, std::uint32_t(tokens.size()));
			docs.push_back(tokens);
		}
		const std::string bytes = w.flush({});
		const segment seg(bytes);
		strata::deleted_bits deleted((seg.core().doc_count() + 63) / 64);
		for (std::uint32_t d = 0; d < 3000; d += 1 + std::uint32_t(r() % 5)) { deleted.set(d); }
		auto dead = [&](std::uint32_t d) { return (deleted[d >> 6] >> (d & 63)) & 1; };
		auto phrase = [&] { return std::make_unique<phrase_scorer>(seg.get<prospect::inverted_plugin>(), std::vector<prospect::format::term_info>{*seg.get<prospect::inverted_plugin>().term(0, "a"), *seg.get<prospect::inverted_plugin>().term(0, "b")}); };
		auto live_phrase = [&] { return std::make_unique<live_scorer>(phrase(), deleted); };
		// "a b" and "c" in live documents, by brute force.
		std::set<ordinal_t> want;
		for (std::uint32_t d = 0; d < 3000; ++d) {
			bool ab = false, c = false;
			for (std::size_t i = 0; i < docs[d].size(); ++i) {
				ab = ab || (i + 1 < docs[d].size() && docs[d][i] == "a" && docs[d][i + 1] == "b");
				c = c || docs[d][i] == "c";
			}
			if (ab && c && !dead(d)) { want.insert(d); }
		}
		std::vector<scorer_ptr> kids;
		kids.push_back(live_phrase());
		kids.push_back(std::make_unique<term_scorer>(seg.get<prospect::inverted_plugin>(), *seg.get<prospect::inverted_plugin>().term(0, "c")));
		conjunction_scorer both(std::move(kids));
		CHECK(both.two_phase());
		std::set<ordinal_t> got;
		for (ordinal_t d = both.next(); d != no_more_docs; d = both.next()) { got.insert(d); }
		CHECK(got == want);
		// The counting copy, and count() through the wrapper.
		// (A phrase has no counting copy; a term does, and the wrapper keeps it.)
		CHECK(!live_phrase()->fresh());
		auto lt = std::make_unique<live_scorer>(std::make_unique<term_scorer>(seg.get<prospect::inverted_plugin>(), *seg.get<prospect::inverted_plugin>().term(0, "c")), deleted);
		auto copy = lt->fresh();
		CHECK(copy && count(*copy) == count(*lt));
		// A window starting off a word boundary clears exactly the deleted ordinals in it.
		auto all_c = std::make_unique<live_scorer>(std::make_unique<term_scorer>(seg.get<prospect::inverted_plugin>(), *seg.get<prospect::inverted_plugin>().term(0, "c")), deleted);
		std::uint64_t bits[64] = {};
		all_c->advance(5);
		all_c->fill(5, 205, bits);
		for (std::uint32_t d = 5; d < 205; ++d) {
			const bool has_c = std::find(docs[d].begin(), docs[d].end(), "c") != docs[d].end();
			CHECK(bool((bits[(d - 5) >> 6] >> ((d - 5) & 63)) & 1) == (has_c && !dead(d)));
		}
	}
	// Budget permits: an empty permit draws nothing; move assignment settles the target.
	{
		byte_budget b(100);
		byte_budget::permit none;
		CHECK(!none.try_grow(1) && !none);
		none.grow(5);
		CHECK(b.used() == 0);
		auto p = b.open(), q = b.open();
		p.grow(30);
		q.grow(40);
		p = std::move(q);  // p's 30 return; q's 40 move
		CHECK(b.used() == 40 && p.bytes() == 40);
		CHECK(!p.try_grow(70) && p.try_grow(60) && b.used() == 100);
	}
	std::printf("multisegment: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
