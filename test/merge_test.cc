// Merges: the merge of an index's segments, after deletes and replacements, is the same
// bytes as one flush of its live documents in docid order; and the result answers as the
// sources did.
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/merge.hh"
#include "strata/merge_policy.hh"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace strata;

static const std::array<char, 16> uuid = {'m', 'e', 'r', 'g', 'e', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i', 'd', '!'};

int main() {
	for (std::uint64_t seed = 0; seed < 20; ++seed) {
		std::mt19937_64 r(seed);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options opts;
		opts.key_prefix = "Q";
		auto ix = strata::index::create(io, opts, uuid);
		// The model: each live docid's document, key, and version.
		struct live {
			document doc;
			std::string key;
			std::uint64_t version;
		};
		std::map<std::uint64_t, live> model;
		static const char* words[] = {"a", "b", "c", "d", "rare", "Bool"};
		auto make = [&](const std::string& key) {
			document d;
			if (!key.empty()) { d.terms.push_back({key, 0, {}}); }
			std::uint32_t pos = 0;
			for (const char* w : words) {
				if (r() % 3) { continue; }
				term_entry t{w, std::string_view(w) == "Bool" ? 0u : 1 + std::uint32_t(r() % 3), {}};
				const std::uint32_t n = std::string_view(w) == "Bool" ? 1 : t.wdf;
				for (std::uint32_t k = 0; k < n; ++k) { t.positions.push_back(pos += 1 + std::uint32_t(r() % 2)); }
				if (r() % 7 == 0) { t.positions.clear(); }  // indexed without positions here
				d.length += t.wdf;
				d.terms.push_back(std::move(t));
			}
			if (r() % 5 == 0) { d.stored = std::string(r() % 5000, char('a' + r() % 26)); }
			return d;
		};
		for (int i = 0; i < 3000; ++i) {
			transaction t;
			operation o;
			const std::uint64_t kind = r() % 10;
			const std::string key = "Q" + std::to_string(r() % 400);
			if (kind < 6) {
				o.type = operation::replace_key;
				o.key = key;
				o.doc = make(key);
			} else if (kind < 7) {
				o.type = operation::delete_key;
				o.key = key;
			} else if (kind < 9) {
				o.type = operation::add;
				o.doc = make("");
			} else {
				o.type = operation::delete_docid;
				o.docid = 1 + r() % (ix.last_docid() + 1);
			}
			t.ops.push_back(o);
			const auto v = ix.apply(t, {});
			if (v.result == verdict::accepted) {
				if (o.type == operation::delete_key || o.type == operation::delete_docid) {
					if (v.docids[0]) { model.erase(v.docids[0]); }
				} else {
					model[v.docids[0]] = {o.doc, o.type == operation::replace_key ? key : std::string(), v.versions[0]};
				}
			}
			if (r() % 300 == 0) { ix.refresh(); }
		}
		ix.refresh();
		std::vector<merge_source> sources;
		for (const auto& s : ix.segments()) { sources.push_back({s.seg, s.deleted}); }
		CHECK(sources.size() > 3);
		const std::array<std::uint8_t, 16> id{7};
		std::string merged;
		{
			string_output out(merged);
			string_spill spill;
			const auto plan = merge_segments(sources, id, out, spill, "Q");
			CHECK(plan.doc_count == model.size());
		}
		// One flush of the live documents in docid order.
		segment_writer w;
		std::vector<posting_input> in;
		for (const auto& [docid, l] : model) {
			in.clear();
			for (const auto& t : l.doc.terms) { in.push_back({t.term, t.wdf, t.positions}); }
			w.add(docid, in, l.doc.length, l.doc.stored, l.version, l.key);
		}
		const std::string direct = w.flush(id);
		CHECK(merged == direct);
		if (merged != direct) { std::fprintf(stderr, "  seed %llu: merged %zu bytes, direct %zu\n", (unsigned long long)seed, merged.size(), direct.size()); }
	}
	// The policy: merges are 2-10 existing segments under the size cap, and applying them
	// converges.
	{
		std::mt19937_64 r(3);
		merge_policy p;
		for (int round = 0; round < 200; ++round) {
			std::vector<merge_policy::candidate> segs;
			std::uint64_t next_id = 1;
			for (int i = 0, n = 2 + int(r() % 60); i < n; ++i) {
				const std::uint64_t docs = 1 + r() % 100000;
				segs.push_back({next_id++, (1 + r() % 4000) << 20, docs, r() % 4 ? 0 : r() % docs});
			}
			for (int steps = 0;; ++steps) {
				CHECK(steps < 100);
				if (steps >= 100) { break; }
				const auto ms = p.find_merges(segs);
				if (ms.empty()) { break; }
				const auto& ids = ms.front();
				CHECK(!ids.empty() && ids.size() <= p.max_merge_at_once);
				std::set<std::uint64_t> unique(ids.begin(), ids.end());
				CHECK(unique.size() == ids.size());
				merge_policy::candidate merged{next_id++, 0, 0, 0};
				double live = 0;
				for (auto id : ids) {
					auto it = std::find_if(segs.begin(), segs.end(), [&](const auto& c) { return c.id == id; });
					CHECK(it != segs.end());
					if (it == segs.end()) { continue; }
					live += it->live_bytes();
					merged.docs += it->docs - it->deleted;
					segs.erase(it);
				}
				CHECK(live <= double(p.max_merged_bytes));
				merged.bytes = std::uint64_t(live);
				segs.push_back(merged);
			}
		}
	}
	// An index merging as it goes keeps few segments and the same digest as one that never
	// merges.
	{
		faultfs::filesystem fa, fb;
		faultfs::io ia(fa), ib(fb);
		index::options merging;
		merging.key_prefix = "Q";
		merging.auto_merge = true;
		index::options plain;
		plain.key_prefix = "Q";
		auto a = strata::index::create(ia, merging, uuid), b = strata::index::create(ib, plain, uuid);
		std::mt19937_64 r(8);
		for (int i = 0; i < 2000; ++i) {
			transaction t;
			operation o;
			o.type = operation::replace_key;
			o.key = "Q" + std::to_string(r() % 500);
			o.doc.terms = {{o.key, 0, {}}, {"w" + std::to_string(r() % 30), 1, {1}}};
			o.doc.length = 1;
			t.ops.push_back(o);
			a.apply(t, {});
			b.apply(t, {});
			if (i % 20 == 19) {
				a.refresh();
				b.refresh();
			}
		}
		CHECK(a.segments().size() <= 9 && b.segments().size() == 100);
		CHECK(a.digest() == b.digest());
		a.commit();
		a.close();
		a = strata::index::open(ia, merging);
		CHECK(a.digest() == b.digest());
	}
	// Forced merges on an index: force_merge_deletes rewrites only segments past its
	// threshold, force_merge(1) leaves one segment without deletes; the digest holds.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options o;
		o.key_prefix = "Q";
		auto ix = strata::index::create(io, o, uuid);
		auto deleted_in = [&](const index::segment_view& v) {
			std::uint64_t n = 0;
			for (auto w : *v.deleted) { n += std::uint64_t(std::popcount(w)); }
			return n;
		};
		auto put = [&](const std::string& key, operation::kind type) {
			transaction t;
			operation op;
			op.type = type;
			op.key = key;
			if (type == operation::replace_key) {
				op.doc.terms = {{key, 0, {}}, {"w", 1, {1}}};
				op.doc.length = 1;
			}
			t.ops.push_back(op);
			ix.apply(t, {});
		};
		for (int seg = 0; seg < 10; ++seg) {
			for (int i = 0; i < 20; ++i) { put("Q" + std::to_string(seg * 20 + i), operation::replace_key); }
			ix.refresh();
		}
		// One deletion in segments 0-4 (5%), four in segments 5-9 (20%).
		for (int seg = 0; seg < 10; ++seg) {
			for (int i = 0; i < (seg < 5 ? 1 : 4); ++i) { put("Q" + std::to_string(seg * 20 + i), operation::delete_key); }
		}
		const auto before = ix.digest();
		std::set<std::uint64_t> light;
		for (const auto& v : ix.segments()) {
			if (deleted_in(v) == 1) { light.insert(v.id); }
		}
		ix.force_merge_deletes();
		CHECK(ix.digest() == before);
		std::size_t kept = 0;
		for (const auto& v : ix.segments()) {
			kept += light.count(v.id);
			if (!light.count(v.id)) { CHECK(deleted_in(v) == 0); }
		}
		CHECK(kept == 5);
		ix.force_merge(1);
		CHECK(ix.segments().size() == 1 && deleted_in(ix.segments()[0]) == 0);
		CHECK(ix.segments()[0].seg->doc_count() == 175);
		CHECK(ix.digest() == before);
	}
	std::printf("merge: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
