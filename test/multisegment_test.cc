// Search across segments, under both weighting presets:
//   - Without deletes, an index of many flushes answers every query exactly as one segment
//     holding the same documents: the same docids, scores, ties, and counts.
//   - With deletes, pruned search across segments equals an exhaustive scan of every
//     segment's live matches, ties by docid, and counts equal.
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/searcher.hh"
#include "strata/budget.hh"
#include <map>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace strata;

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
		d.terms.push_back(std::move(t));
	}
	if (d.terms.empty()) { d.terms.push_back({"a", 1, {1}}), d.length = 1; }
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
		auto info = seg.term(t);
		return info ? std::make_unique<term_scorer>(seg, *info, weight_of(t)) : nullptr;
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
		std::vector<format::term_info> infos;
		std::vector<std::uint64_t> dfs;
		std::vector<weight> ws;
		for (auto& t : q.terms) {
			auto info = seg.term(t);
			if (!info) { return nullptr; }
			infos.push_back(*info);
			dfs.push_back(snapshot_df(segs, t));
			ws.push_back(weight_of(t));
		}
		if (xapian) { return std::make_unique<phrase_scorer>(seg, infos, std::move(ws)); }
		return std::make_unique<phrase_scorer>(seg, infos, std::vector<weight>{}, 0, &st, dfs);
	}
	}
}

static std::vector<segment_snapshot> snapshot(const strata::index& ix) {
	std::vector<segment_snapshot> out;
	for (const auto& s : ix.segments()) { out.push_back({s.seg, s.deleted}); }
	return out;
}

int main() {
	std::mt19937_64 r(5);
	for (int round = 0; round < 12; ++round) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, {}, uuid);
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
			if (d % 500 == 499) { ix.refresh(); }
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
				if (r() % 100 == 0) { ix.refresh(); }
			}
		}
		ix.refresh();
		const auto segs = snapshot(ix);
		const statistics st = snapshot_statistics(segs);
		CHECK(segs.size() > 3);
		// The same documents in one segment.
		for (std::size_t d = 0; d < docs; ++d) {
			std::vector<posting_input> in;
			for (const auto& t : all[d].terms) { in.push_back({t.term, t.wdf, t.positions}); }
			one.add(d + 1, in, all[d].length);
		}
		const std::string one_bytes = one.flush({});
		const segment single(one_bytes);
		const std::vector<std::uint64_t> none((single.doc_count() + 63) / 64, 0);
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
			for (std::size_t k : {1u, 10u, 100u}) {
				std::uint64_t counted = 0;
				const auto got = search(segs, builder, k, &counted);
				const auto pruned = search(segs, builder, k);
				CHECK(search_count(segs, builder) == counted);
				// Every live match of every segment, scored without pruning, best first.
				std::vector<doc_hit> all;
				for (const auto& s : segs) {
					auto sc = builder(*s.seg);
					if (!sc) { continue; }
					for (ordinal_t o = sc->next(); o != no_more_docs; o = sc->next()) {
						if (s.deleted && ((*s.deleted)[o >> 6] >> (o & 63)) & 1) { continue; }
						all.push_back({s.seg->docid(o), sc->score()});
					}
				}
				std::sort(all.begin(), all.end(), [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
				CHECK(counted == all.size());
				auto close = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(a, b)); };
				const std::size_t n = std::min(k, all.size());
				for (const auto* hits : {&got, &pruned}) {
					CHECK(hits->size() == n);
					// Ranks agree on score; each run of equal scores is drawn from the scan's
					// documents with that score, the smallest docids first (ties by docid).
					for (std::size_t a = 0; a < n && hits->size() == n;) {
						std::size_t b = a + 1;
						while (b < n && close(all[b].score, all[a].score)) { ++b; }
						std::vector<std::uint64_t> want_ids, got_ids;
						for (std::size_t i = a; i < b; ++i) {
							CHECK(close((*hits)[i].score, all[i].score));
							want_ids.push_back(all[i].docid);
							got_ids.push_back((*hits)[i].docid);
						}
						std::sort(got_ids.begin(), got_ids.end());
						// Exactly equal scores must give exactly the scan's docids.
						bool exact = true;
						for (std::size_t i = a; i < b; ++i) { exact = exact && all[i].score == all[a].score; }
						if (exact) { CHECK(got_ids == want_ids); }
						a = b;
					}
				}
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
		std::vector<std::uint64_t> deleted((seg.doc_count() + 63) / 64, 0);
		for (std::uint32_t d = 0; d < 3000; d += 1 + std::uint32_t(r() % 5)) { deleted[d >> 6] |= std::uint64_t(1) << (d & 63); }
		auto dead = [&](std::uint32_t d) { return (deleted[d >> 6] >> (d & 63)) & 1; };
		auto phrase = [&] { return std::make_unique<phrase_scorer>(seg, std::vector<format::term_info>{*seg.term("a"), *seg.term("b")}); };
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
		kids.push_back(std::make_unique<term_scorer>(seg, *seg.term("c")));
		conjunction_scorer both(std::move(kids));
		CHECK(both.two_phase());
		std::set<ordinal_t> got;
		for (ordinal_t d = both.next(); d != no_more_docs; d = both.next()) { got.insert(d); }
		CHECK(got == want);
		// The counting copy, and count() through the wrapper.
		// (A phrase has no counting copy; a term does, and the wrapper keeps it.)
		CHECK(!live_phrase()->fresh());
		auto lt = std::make_unique<live_scorer>(std::make_unique<term_scorer>(seg, *seg.term("c")), deleted);
		auto copy = lt->fresh();
		CHECK(copy && count(*copy) == count(*lt));
		// A window starting off a word boundary clears exactly the deleted ordinals in it.
		auto all_c = std::make_unique<live_scorer>(std::make_unique<term_scorer>(seg, *seg.term("c")), deleted);
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
