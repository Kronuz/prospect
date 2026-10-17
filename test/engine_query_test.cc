// The benchmark engine's query layer (tools/engine_query.hh) over an index's segments, as
// serve and churn answer queries:
//   - Without deletes, many segments answer every query exactly as one segment holding
//     the same documents: weights come from the snapshot's statistics, not each segment's
//     (review R5: churn used per-segment statistics).
//   - With deletes in some segments, the top k agrees with an exhaustive scan of the live
//     documents under the same weights, ties by docid, and every count agrees.
//   - The term table (P6-3) answers exactly as the path before it, which looked each term
//     up twice per segment (once for its snapshot df, once for the scorer): the same hits,
//     scores, and counts, bit for bit; and find_term plus complete_impacts is term().
#include "engine_query.hh"
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
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

// The query path before the term table: each term's snapshot df from term() in every
// segment, then term() again in the segment being built.
struct reference_stats {
	std::span<const segment_snapshot> segs;
	statistics st;
	std::map<std::string, std::uint64_t> df;
	explicit reference_stats(std::span<const segment_snapshot> s) : segs(s), st(snapshot_statistics(s)) {}
	std::uint64_t of(const std::string& term) {
		auto [it, fresh] = df.try_emplace(term, 0);
		if (fresh) {
			for (const auto& s : segs) {
				if (auto info = s.seg->get<strata::inverted_plugin>().term(term)) { it->second += info->df; }
			}
		}
		return it->second;
	}
};

static scorer_ptr reference_clause(const segment& seg, const std::vector<std::string>& clause, double boost, scheme sch, reference_stats& snap) {
	std::vector<format::term_info> infos;
	for (const auto& t : clause) {
		auto info = seg.get<strata::inverted_plugin>().term(t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
	}
	const bool xapian = sch == scheme::bm25_xapian;
	if (infos.size() == 1) {
		const auto df = snap.of(clause[0]);
		return std::make_unique<term_scorer>(seg.get<strata::inverted_plugin>(), infos[0], xapian ? weight::bm25_xapian(snap.st, df, std::uint32_t(boost)) : weight::bm25(snap.st, df, boost));
	}
	if (!xapian) {
		std::vector<std::uint64_t> dfs;
		for (const auto& t : clause) { dfs.push_back(snap.of(t)); }
		return std::make_unique<phrase_scorer>(seg.get<strata::inverted_plugin>(), infos, std::vector<weight>{}, 0, &snap.st, dfs);
	}
	std::vector<weight> weights;
	for (std::size_t k = 0; k < infos.size(); ++k) { weights.push_back(weight::bm25_xapian(snap.st, snap.of(clause[k]))); }
	return std::make_unique<phrase_scorer>(seg.get<strata::inverted_plugin>(), infos, std::move(weights));
}

static scorer_ptr reference_build(const segment& seg, const engine::parsed& q, scheme sch, reference_stats& snap) {
	std::vector<scorer_ptr> must, should, must_not;
	for (const auto& [c, boost] : engine::merge_repeats(q.must)) {
		auto s = reference_clause(seg, c, boost, sch, snap);
		if (!s) { return nullptr; }
		must.push_back(std::move(s));
	}
	for (const auto& [c, boost] : engine::merge_repeats(q.should)) {
		if (auto s = reference_clause(seg, c, boost, sch, snap)) { should.push_back(std::move(s)); }
	}
	for (const auto& c : q.must_not) {
		if (auto s = reference_clause(seg, c, 1.0, sch, snap)) { must_not.push_back(std::move(s)); }
	}
	auto combine = [](std::vector<scorer_ptr> v, bool all) -> scorer_ptr {
		if (v.empty()) { return nullptr; }
		if (v.size() == 1) { return std::move(v[0]); }
		if (all) { return std::make_unique<conjunction_scorer>(std::move(v)); }
		return std::make_unique<disjunction_scorer>(std::move(v));
	};
	scorer_ptr req = combine(std::move(must), true), opt = combine(std::move(should), false);
	scorer_ptr result;
	if (req && opt) {
		result = std::make_unique<required_optional_scorer>(std::move(req), std::move(opt));
	} else {
		result = req ? std::move(req) : std::move(opt);
	}
	for (auto& ex : must_not) {
		if (result) { result = std::make_unique<exclusion_scorer>(std::move(result), std::move(ex)); }
	}
	return result;
}

// Every live match, scored with the snapshot's statistics, best first (ties by docid).
static std::vector<doc_hit> scan(std::span<const segment_snapshot> segs, const engine::parsed& q, scheme sch) {
	term_table table(segs);
	std::vector<doc_hit> all;
	for (const auto& s : segs) {
		auto sc = engine::build(*s.seg, q, sch, &table);
		if (!sc) { continue; }
		for (ordinal_t o = sc->next(); o != no_more_docs; o = sc->next()) {
			if (s.deleted && ((*s.deleted)[o >> 6] >> (o & 63)) & 1) { continue; }
			all.push_back({s.seg->core().docid(o), sc->score()});
		}
	}
	std::sort(all.begin(), all.end(), [](const doc_hit& a, const doc_hit& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
	return all;
}

static void check_against_scan(const std::vector<doc_hit>& hits, const std::vector<doc_hit>& all, std::size_t k) {
	auto close = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(a, b)); };
	const std::size_t n = std::min(k, all.size());
	CHECK(hits.size() == n);
	// R1 on the engine's own scores: equal scores come in docid order.
	for (std::size_t i = 1; i < hits.size(); ++i) { CHECK(hits[i - 1].score != hits[i].score || hits[i - 1].docid < hits[i].docid); }
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
		// The scan's tie order binds only where the engine scored the group bit for bit
		// as the scan did; a pruned path may sum in another order, an ulp apart.
		bool exact = true;
		for (std::size_t i = a; i < b; ++i) { exact = exact && all[i].score == all[a].score && hits[i].score == all[i].score; }
		if (exact) { CHECK(got_ids == want_ids); }
		a = b;
	}
}

// A document scores the same in every segment: each probe goes into two segments whose
// fillers reverse which terms bound highest, and a disjunction of every word must score
// both copies bit for bit (scores sum in clause order, not in each segment's bound order).
static void check_segment_independent(std::mt19937_64& r) {
	const char* words[] = {"a", "b", "c", "d", "e", "f"};
	faultfs::filesystem fs;
	faultfs::io io(fs);
	index::options opts;
	opts.auto_merge = false;
	auto ix = strata::index::create(io, opts, uuid);
	std::vector<document> probes;
	for (int i = 0; i < 200; ++i) {
		document d;
		std::uint32_t pos = 0;
		for (const char* w : words) {
			term_entry t{w, 1 + std::uint32_t(r() % 9), {}};
			for (std::uint32_t k = 0; k < t.wdf; ++k) { t.positions.push_back(++pos); }
			d.length += t.wdf;
			d.terms.push_back(std::move(t));
		}
		probes.push_back(std::move(d));
	}
	auto add = [&](const document& d) {
		transaction t;
		operation o;
		o.doc = d;
		t.ops.push_back(o);
		ix.apply(t, {});
	};
	for (int side = 0; side < 2; ++side) {
		for (const auto& d : probes) { add(d); }
		for (int i = 0; i < 300; ++i) {  // short fillers dense in one end of the vocabulary
			document f;
			const char* w = words[side ? 5 - i % 3 : i % 3];
			f.terms.push_back({w, 20, {}});
			for (std::uint32_t k = 0; k < 20; ++k) { f.terms.back().positions.push_back(k + 1); }
			f.length = 20;
			add(f);
		}
		ix.refresh();
	}
	const auto segs = snapshot(ix);
	CHECK(segs.size() == 2);
	const engine::parsed q = engine::parse("a b c d e f");
	const auto all = scan(segs, q, scheme::bm25);
	std::map<std::uint64_t, double> score;
	for (const auto& h : all) { score[h.docid] = h.score; }
	const std::uint64_t second = probes.size() + 300;  // docids: probes, fillers, probes, fillers
	std::size_t differ = 0;
	for (std::uint64_t i = 1; i <= probes.size(); ++i) { differ += score.at(i) != score.at(second + i); }
	CHECK(differ == 0);
	for (const auto& h : engine::search_snapshot(segs, q, 1000, scheme::bm25)) { CHECK(h.score == score.at(h.docid)); }
}

// Review R2: explicit docids that interleave across flushes, duplicated documents so
// equal scores straddle the k-th position, segments visited in index order and largest
// first. Single words score bit for bit as the scan does, so they must equal its first k.
static void check_interleaved(std::mt19937_64& r) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	index::options opts;
	opts.auto_merge = false;
	auto ix = strata::index::create(io, opts, uuid);
	std::vector<document> pool;
	for (int i = 0; i < 40; ++i) { pool.push_back(random_doc(r)); }
	for (std::size_t d = 0; d < 3000; ++d) {
		transaction t;
		operation o;
		o.type = operation::replace_docid;
		o.docid = 1 + r() % 6000;
		o.doc = pool[r() % pool.size()];
		t.ops.push_back(o);
		ix.apply(t, {});
		if (d % 300 == 299) { ix.refresh(); }
	}
	ix.refresh();
	auto segs = snapshot(ix);
	CHECK(segs.size() > 3);
	auto largest_first = segs;
	std::stable_sort(largest_first.begin(), largest_first.end(), [](const segment_snapshot& a, const segment_snapshot& b) { return a.seg->core().doc_count() > b.seg->core().doc_count(); });
	for (const auto* order : {&segs, &largest_first}) {
		const std::span<const segment_snapshot> view(*order);
		for (const char* w : vocab) {
			const engine::parsed q = engine::parse(w);
			const auto want = scan(view, q, scheme::bm25);
			for (std::size_t k : {1u, 10u, 100u}) {
				std::uint64_t n = 0;
				const auto got = engine::search_snapshot(view, q, k, scheme::bm25, &n);
				CHECK(n == want.size());
				CHECK(got == std::vector<doc_hit>(want.begin(), want.begin() + std::ptrdiff_t(std::min(k, want.size()))));
			}
		}
		for (int qn = 0; qn < 60; ++qn) {
			const engine::parsed q = engine::parse(random_query(r));
			const auto want = scan(view, q, scheme::bm25);
			for (std::size_t k : {1u, 10u, 100u}) { check_against_scan(engine::search_snapshot(view, q, k, scheme::bm25), want, k); }
		}
	}
}

int main() {
	std::mt19937_64 r(17);
	check_interleaved(r);
	check_segment_independent(r);
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
		// find_term plus complete_impacts is term(), field by field.
		for (const auto& s : segs) {
			for (const char* t : vocab) {
				const auto full = s.seg->get<strata::inverted_plugin>().term(t);
				auto lazy = s.seg->get<strata::inverted_plugin>().find_term(t);
				CHECK(bool(full) == bool(lazy));
				if (!full || !lazy) { continue; }
				CHECK(lazy->df == full->df && lazy->cf == full->cf && lazy->flags == full->flags);
				s.seg->get<strata::inverted_plugin>().complete_impacts(*lazy);
				CHECK(lazy->postings == full->postings && lazy->positions == full->positions && lazy->single_wdf == full->single_wdf);
				CHECK(lazy->postings_length == full->postings_length && lazy->positions_length == full->positions_length);
				CHECK(lazy->impacts.size() == full->impacts.size());
				for (std::size_t i = 0; i < lazy->impacts.size() && i < full->impacts.size(); ++i) {
					CHECK(lazy->impacts[i].wdf == full->impacts[i].wdf && lazy->impacts[i].length == full->impacts[i].length);
				}
			}
		}
		// Single-word counts (P6-7): document frequencies in segments without deletes,
		// counted in the others, equal to the scan and to today's path.
		for (const char* w : vocab) {
			const std::string word(w);
			for (const std::string& text : {word, "+" + word, word + " " + word, "+" + word + " " + word, "\"" + word + "\""}) {
				const engine::parsed q = engine::parse(text);
				CHECK(engine::single_term(q) && *engine::single_term(q) == word);
				reference_stats ref(segs);
				auto reference = [&](const segment& seg) { return reference_build(seg, q, scheme::bm25, ref); };
				const std::uint64_t n = engine::count_snapshot(segs, q);
				CHECK(n == scan(segs, q, scheme::bm25).size());
				CHECK(n == search_count(segs, reference));
				CHECK(engine::run(std::span<const segment_snapshot>(segs), "COUNT", text) == n);
			}
			CHECK(!engine::single_term(engine::parse(word + " -" + word)));
			CHECK(!engine::single_term(engine::parse(word + " b")) || word == "b");
		}
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
			reference_stats ref(segs);
			auto reference = [&](const segment& seg) { return reference_build(seg, q, sch, ref); };
			CHECK(search_count(segs, reference) == counted);
			for (std::size_t k : {1u, 10u, 100u, 1000u}) {
				std::uint64_t n = 0;
				const auto got = engine::search_snapshot(segs, q, k, sch, &n);
				CHECK(n == counted);
				std::uint64_t ref_n = 0;
				CHECK(search(segs, reference, k, &ref_n) == got);
				CHECK(ref_n == n);
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
