// Churn (TESTING, "Churn" and "Steady state"; Phase 3 design T10), on faultfs:
//   - Cycles of small repeated changes to a few keys, bursts of many adds, mass deletes,
//     and documents of megabytes and tens of thousands of terms among tiny ones, with
//     merges, forced deletes merges, commits, and reopens between. After every cycle the
//     index equals a model: the same live keys, versions, and stored records, the same
//     document count for sampled terms, and index::check passes.
//   - At a constant live set, half replaced every cycle: segment bytes, segment count,
//     and the deleted fraction stay bounded against a fresh index of the same documents,
//     and do not trend upward.
// STRATA_CHURN_CYCLES sets the number of churn cycles (default 8).
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/searcher.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::document;
using strata::operation;
using strata::transaction;
using strata::verdict;

namespace faultfs = strata::faultfs;

static const std::array<char, 16> uuid{'c', 'h', 'u', 'r', 'n'};

struct model_doc {
	std::uint64_t version = 0;
	std::string stored;
	std::vector<std::string> words;  // distinct
};
using model = std::map<std::string, model_doc>;

static document make_doc(const std::string& key, const model_doc& m) {
	document d;
	d.terms.push_back({key, 0, {}});
	std::uint32_t pos = 0;
	for (const auto& w : m.words) {
		d.terms.push_back({w, 1, {++pos}});
		++d.length;
	}
	d.stored = m.stored;
	return d;
}

static model_doc random_doc(std::mt19937_64& r, bool huge = false) {
	model_doc m;
	std::set<std::string> words;
	const std::size_t n = huge ? 20000 : 1 + r() % 12;
	if (huge) {
		for (std::size_t i = 0; i < n; ++i) { words.insert("h" + std::to_string(i)); }
		m.stored.resize(1 + r() % (3 << 20));
		for (auto& c : m.stored) { c = char(r()); }
	} else {
		while (words.size() < n) { words.insert("w" + std::to_string(r() % 200)); }
		m.stored = "s" + std::to_string(r()) + std::string(r() % 64, 'x');
	}
	m.words.assign(words.begin(), words.end());
	return m;
}

// Applies one change to the index and the model: add or replace (with the new version
// the index assigns) or delete.
static void put(strata::index& ix, model& m, const std::string& key, const model_doc* doc) {
	transaction t;
	operation o;
	o.key = key;
	if (doc) {
		o.type = operation::replace_key;
		o.doc = make_doc(key, *doc);
	} else {
		o.type = operation::delete_key;
	}
	t.ops.push_back(o);
	const auto v = ix.apply(t, {});
	CHECK(v.result == verdict::accepted);
	if (doc) {
		auto stored = *doc;
		stored.version = v.versions.empty() ? 0 : v.versions[0];
		m[key] = std::move(stored);
	} else {
		m.erase(key);
	}
}

static std::vector<strata::segment_snapshot> snapshot(const strata::index& ix) {
	std::vector<strata::segment_snapshot> out;
	for (const auto& s : ix.segments()) { out.push_back({s.seg, s.deleted}); }
	return out;
}

// The index against the model: live keys, versions, stored records, sampled term counts,
// and the offline check.
static void verify(const strata::index& ix, const model& m, std::mt19937_64& r, const char* when) {
	std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> live;  // docid -> (version, stored)
	for (const auto& s : ix.segments()) {
		for (strata::ordinal_t o = 0; o < s.seg->doc_count(); ++o) {
			if (s.live(o)) { live[s.seg->docid(o)] = {s.seg->version(o), s.seg->stored(o)}; }
		}
	}
	CHECK(live.size() == m.size());
	std::size_t mismatched = 0;
	for (const auto& [key, doc] : m) {
		const auto found = ix.find_key(key);
		if (!found || found->second != doc.version || !live.count(found->first) || live[found->first].second != doc.stored) { ++mismatched; }
	}
	CHECK(mismatched == 0);
	const auto segs = snapshot(ix);
	for (int q = 0; q < 12; ++q) {
		const std::string word = q < 10 ? "w" + std::to_string(r() % 200) : "h" + std::to_string(r() % 20000);
		std::uint64_t want = 0;
		for (const auto& [key, doc] : m) { want += std::binary_search(doc.words.begin(), doc.words.end(), word); }
		const auto got = strata::search_count(segs, [&](const strata::segment& seg) -> strata::scorer_ptr {
			auto info = seg.term(word);
			if (!info) { return nullptr; }
			return std::make_unique<strata::term_scorer>(seg, *info);
		});
		CHECK(got == want);
	}
	const auto report = ix.check();
	CHECK(report.documents - report.deleted == m.size());
	if (failures) { std::fprintf(stderr, "  after %s\n", when); }
}

// The committed segments' bytes (call after a commit).
static std::uint64_t segment_bytes(const strata::index& ix) {
	std::uint64_t n = 0;
	for (const auto& s : ix.manifest().segments) { n += s.size; }
	return n;
}

int main() {
	strata::index::options o;
	o.key_prefix = "Q";
	o.auto_merge = true;
	std::size_t cycles = 8;
	if (const char* env = std::getenv("STRATA_CHURN_CYCLES")) { cycles = std::strtoull(env, nullptr, 10); }
	// Churn against the model.
	{
		std::mt19937_64 r(2026);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		strata::byte_budget memory(4 << 20);
		o.budget = &memory;
		o.buffer_share = 1 << 20;
		auto ix = strata::index::create(io, o, uuid);
		model m;
		std::size_t next_key = 0;
		for (std::size_t c = 0; c < cycles; ++c) {
			// Small repeated changes to the same few keys.
			for (int i = 0; i < 300; ++i) {
				const std::string key = "Qs" + std::to_string(r() % 20);
				if (r() % 3 == 0 && m.count(key)) {
					put(ix, m, key, nullptr);
				} else {
					const auto d = random_doc(r);
					put(ix, m, key, &d);
				}
				if (i % 50 == 49) { ix.refresh(); }
			}
			ix.commit();
			verify(ix, m, r, "small changes");
			// A burst of adds, then a mass delete of most of what is live.
			for (int i = 0; i < 1500; ++i) {
				const auto d = random_doc(r);
				put(ix, m, "Qb" + std::to_string(next_key++), &d);
			}
			ix.commit();
			std::vector<std::string> keys;
			for (const auto& [k, d] : m) { keys.push_back(k); }
			std::shuffle(keys.begin(), keys.end(), r);
			for (std::size_t i = 0; i < keys.size() * 8 / 10; ++i) { put(ix, m, keys[i], nullptr); }
			ix.commit();
			verify(ix, m, r, "a burst and a mass delete");
			// Huge documents among tiny ones, then one huge one deleted.
			std::vector<std::string> huge;
			for (int i = 0; i < 2; ++i) {
				const auto big = random_doc(r, true);
				huge.push_back("Qh" + std::to_string(next_key++));
				put(ix, m, huge.back(), &big);
				for (int j = 0; j < 25; ++j) {
					const auto d = random_doc(r);
					put(ix, m, "Qt" + std::to_string(next_key++), &d);
				}
			}
			ix.commit();
			put(ix, m, huge[r() % 2], nullptr);
			ix.commit();
			verify(ix, m, r, "huge documents");
			if (c % 3 == 2) {
				ix.force_merge_deletes();
				ix.commit();
				verify(ix, m, r, "a forced deletes merge");
			}
			if (c % 4 == 3) {
				ix.close();
				ix = strata::index::open(io, o);
				verify(ix, m, r, "a reopen");
			}
		}
		ix.force_merge(1);
		ix.commit();
		verify(ix, m, r, "a merge to one segment");
		std::printf("churn: %zu cycles, %zu live documents, %zu segments\n", cycles, m.size(), ix.segments().size());
	}
	// The steady state: 1,000 live keys, half replaced every cycle.
	{
		std::mt19937_64 r(7);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		strata::index::options so;
		so.key_prefix = "Q";
		so.auto_merge = true;
		so.policy.floor_bytes = 4096;  // tiers at this scale, as at 16 MiB on real ones
		auto ix = strata::index::create(io, so, uuid);
		model m;
		for (int i = 0; i < 1000; ++i) {
			const auto d = random_doc(r);
			put(ix, m, "Q" + std::to_string(i), &d);
		}
		ix.commit();
		std::vector<std::uint64_t> bytes;
		std::size_t most_segments = 0;
		double most_deleted = 0;
		for (int c = 0; c < 40; ++c) {
			for (int i = 0; i < 500; ++i) {
				const auto d = random_doc(r);
				put(ix, m, "Q" + std::to_string(r() % 1000), &d);
				if (i % 100 == 99) { ix.refresh(); }
			}
			ix.commit();
			bytes.push_back(segment_bytes(ix));
			most_segments = std::max(most_segments, ix.segments().size());
			std::uint64_t docs = 0, deleted = 0;
			for (const auto& s : ix.segments()) {
				docs += s.seg->doc_count();
				if (s.deleted) {
					for (auto w : *s.deleted) { deleted += std::uint64_t(std::popcount(w)); }
				}
			}
			most_deleted = std::max(most_deleted, docs ? double(deleted) / double(docs) : 0.0);
		}
		verify(ix, m, r, "the steady state");
		// A fresh index of the same documents.
		faultfs::filesystem ffs;
		faultfs::io fio(ffs);
		auto fresh = strata::index::create(fio, so, uuid);
		model fm;
		for (const auto& [key, doc] : m) { put(fresh, fm, key, &doc); }
		fresh.commit();
		fresh.force_merge(1);
		fresh.commit();
		const double base = double(segment_bytes(fresh));
		const auto mean = [&](std::size_t from, std::size_t to) {
			double s = 0;
			for (std::size_t i = from; i < to; ++i) { s += double(bytes[i]); }
			return s / double(to - from);
		};
		const double worst = double(*std::max_element(bytes.begin(), bytes.end()));
		std::printf("steady state: segment bytes up to %.2fx a fresh index, %zu segments at most, %.0f%% deleted at most; cycles 10-20 %.2fx, 30-40 %.2fx\n",
			worst / base, most_segments, 100 * most_deleted, mean(10, 20) / base, mean(30, 40) / base);
		CHECK(worst <= 3.0 * base);
		CHECK(mean(30, 40) <= 1.25 * mean(10, 20));  // no upward trend
		CHECK(most_deleted <= 0.5);
		CHECK(most_segments <= 24);
	}
	std::printf("churn: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
