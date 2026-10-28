// Reordering a merge (reorder.hh; reordering.md D4): the forward index equals the graph
// computed directly from the sources' live postings, with permuted sources and deletes; its
// bytes do not depend on how many passes the memory allows; the order is one merge_segments
// accepts, and deterministic; the fallback and cancellation hold.
#include "text_field.hh"
#include "prospect/index.hh"
#include "prospect/reorder.hh"
#include "view_sources.hh"
#include "strata/core/faultfs.hh"

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

using namespace prospect;

namespace {

const std::array<char, 16> uuid = {'r', 'e', 'o', 'r', 'd', 'e', 'r', '-', 't', 'e', 's', 't', 'u', 'u', 'i', 'd'};

// The graph straight from the sources: per rank, the ids of its graph terms.
std::vector<std::vector<std::uint32_t>> direct(std::span<const merge_source> sources, const reorder::settings& set, std::uint32_t& terms) {
	const auto refs = reorder::docid_order(sources);
	std::map<std::pair<std::uint32_t, ordinal_t>, std::uint32_t> rank;
	for (std::uint32_t r = 0; r < refs.size(); ++r) { rank[{refs[r].source, refs[r].ord}] = r; }
	std::set<std::pair<std::uint32_t, std::string>> all;
	for (const auto& s : sources) {
		s.seg->get<prospect::inverted_plugin>().scan_terms([&](std::uint32_t f, std::string_view t, const prospect::format::term_info&) {
			all.insert({f, std::string(t)});
			return true;
		});
	}
	std::vector<std::vector<std::uint32_t>> rows(refs.size());
	terms = 0;
	for (const auto& t : all) {
		std::vector<std::uint32_t> holders;
		for (std::uint32_t s = 0; s < sources.size(); ++s) {
			auto info = sources[s].seg->get<prospect::inverted_plugin>().term(t.first, t.second);
			if (!info) { continue; }
			postings_iterator it(sources[s].seg->get<prospect::inverted_plugin>(), *info);
			for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
				if (sources[s].live(o)) { holders.push_back(rank.at({s, o})); }
			}
		}
		if (holders.size() < set.min_df || double(holders.size()) > set.max_df * double(refs.size())) { continue; }
		for (auto r : holders) { rows[r].push_back(terms); }
		++terms;
	}
	return rows;
}

bool has_file(kronuz::journal::IO& io, std::string_view suffix) {
	auto cursor = io.scan_directory();
	while (auto n = cursor->next()) {
		if (n->ends_with(suffix)) { return true; }
	}
	return false;
}

std::vector<std::vector<std::uint32_t>> rows_of(const bisection::graph& g) {
	std::vector<std::vector<std::uint32_t>> rows(g.docs);
	for (std::uint32_t d = 0; d < g.docs; ++d) {
		g.for_row(d, [&](std::uint32_t t) { rows[d].push_back(t); });
	}
	return rows;
}

}  // namespace

int main() {
	for (std::uint64_t seed = 0; seed < 6; ++seed) {
		std::mt19937_64 r(seed);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options opts;
		opts.key_prefix = "Q";
		opts.reorder_settings.min_df = 40;
		auto ix = prospect::index::create(io, opts, uuid);
		// Two topics with their own words, interleaved, so bisection has clusters to find.
		for (int i = 0; i < 2500; ++i) {
			transaction t;
			t.fields = text_fields();
			t.fields.push_back({"other", strata::field_role::indexed, strata::field_postings::positions, false});
			operation o;
			const std::uint64_t kind = r() % 10;
			const std::string key = "Q" + std::to_string(r() % 900);
			const int topic = int(r() % 2);
			document d;
			for (int k = 0, n = 3 + int(r() % 6); k < n; ++k) {
				const std::string w = std::string(1, char('a' + topic * 10 + r() % 10)) + (r() % 4 ? "" : "x");
				d.field().terms.push_back({w, 1, {std::uint32_t(k + 1)}});
				d.field().length += 1;
			}
			std::sort(d.field().terms.begin(), d.field().terms.end(), [](const auto& a, const auto& b) { return a.term < b.term; });
			d.field().terms.erase(std::unique(d.field().terms.begin(), d.field().terms.end(), [](const auto& a, const auto& b) { return a.term == b.term; }), d.field().terms.end());
			if (r() % 2) {  // the same words in a second field: they are other terms, never merged in the graph
				auto& other = d.field(strata::field_ref::declared(1));
				other.terms = d.field().terms;
				other.length = std::uint32_t(other.terms.size());
			}
			if (r() % 3) { d.stored = std::to_string(i) + std::string(r() % 300, char('a' + topic)); }
			if (kind < 6) {
				o.type = operation::replace_key;
				o.key = key;
				d.key = key;
				o.doc = d;
			} else if (kind < 7) {
				o.type = operation::delete_key;
				o.key = key;
			} else {
				o.type = operation::add;
				o.doc = d;
			}
			t.ops.push_back(o);
			(void)ix.apply(t, {});
			if (r() % 400 == 0) { ix.refresh(); }
		}
		ix.refresh();
		view_sources held(ix.segments());
		auto& sources = held.sources;
		CHECK(sources.size() > 2);
		reorder::settings set;
		set.min_df = 40;
		set.max_df = seed % 2 ? 1.0 : 0.3;

		// The forward index, against the direct graph, at memory sizes from many passes to one.
		std::uint32_t terms = 0;
		const auto expect = direct(sources, set, terms);
		CHECK(terms > 4);
		std::string first_bytes;
		for (std::uint64_t mem : {std::uint64_t(64), std::uint64_t(4096), std::uint64_t(256) << 20}) {
			set.memory = mem;
			string_scratch file;
			const auto st = reorder::write_forward(sources, file.out(), set);
			CHECK(st.terms == terms);
			CHECK(st.docs == expect.size());
			CHECK((mem == 64) == (st.passes > 100));
			const auto g = reorder::forward_graph(file.bytes(), st);
			g.check();
			CHECK(rows_of(g) == expect);
			if (first_bytes.empty()) {
				first_bytes = std::string(file.bytes());
			} else {
				CHECK(file.bytes() == first_bytes);
			}
		}

		// The order: a permutation of the live documents that merge accepts, and deterministic.
		set.memory = 256ull << 20;
		string_scratch f1, f2;
		const auto order = reorder::order(sources, f1, set);
		CHECK(!order.empty());
		CHECK(reorder::order(sources, f2, set).size() == order.size());
		{
			string_scratch f3;
			const auto again = reorder::order(sources, f3, set);
			bool same = again.size() == order.size();
			for (std::size_t i = 0; same && i < order.size(); ++i) { same = again[i].source == order[i].source && again[i].ord == order[i].ord; }
			CHECK(same);
		}
		std::string merged;
		{
			string_output out(merged);
			string_spill spill;
			const std::array<std::uint8_t, 16> id{9};
			const auto plan = merge_segments(sources, id, out, spill, {.order = order, .key_prefix = "Q"});
			CHECK(plan.doc_count == order.size());
		}
		const segment seg(merged);
		seg.verify_all();
		CHECK(!seg.core().docid_ordered());
		// Stored records through a record spill (each source frame decoded once) write the
		// same bytes as through the frame caches.
		{
			std::string again;
			string_output out(again);
			string_spill spill;
			string_scratch records;
			const std::array<std::uint8_t, 16> id{9};
			(void)merge_segments(sources, id, out, spill, {.order = order, .records = &records, .key_prefix = "Q"});
			CHECK(again == merged);
			CHECK(!records.bytes().empty());
		}

		// A permuted source among docid-ordered ones (a forced merge of a reordered segment with
		// newer ones): its lists are read whole, through the rank map.
		std::vector<merge_source> mixed{{&seg, nullptr}};
		std::vector<std::uint64_t> mixed_deleted((seg.core().doc_count() + 63) / 64, 0);
		for (ordinal_t o = 0; o < seg.core().doc_count(); o += 7) { mixed_deleted[o >> 6] |= std::uint64_t(1) << (o & 63); }
		mixed[0].deleted = &mixed_deleted;
		// Newer documents, docids past the merged segment's.
		segment_writer w;
		for (std::uint64_t i = 0; i < 300; ++i) {
			const std::uint64_t docid = seg.core().max_docid() + 1 + i;
			std::vector<posting_input> in;
			static const std::vector<std::uint32_t> pos = {1};
			for (const char* t : {"a", "b", "k", "l"}) {
				if (r() % 2) { in.push_back({t, 1, pos}); }
			}
			w.add(docid, in, std::uint32_t(in.size()));
		}
		const std::string newer = w.flush({});
		const segment seg2(newer);
		mixed.push_back({&seg2, nullptr});
		const auto expect_mixed = direct(mixed, set, terms);
		for (std::uint64_t mem : {std::uint64_t(64), std::uint64_t(256) << 20}) {
			set.memory = mem;
			string_scratch file;
			const auto st = reorder::write_forward(mixed, file.out(), set);
			CHECK(rows_of(reorder::forward_graph(file.bytes(), st)) == expect_mixed);
		}
		set.memory = 256ull << 20;

		// The fallback: a working set above memory keeps docid order.
		{
			reorder::settings tight = set;
			tight.memory = reorder::working_set(expect.size(), 1) - 1;
			string_scratch file;
			reorder::forward_stats st;
			CHECK(reorder::order(sources, file, tight, nullptr, nullptr, &st).empty());
			CHECK(reorder::working_set(st.docs, st.terms) > tight.memory);
		}
		// Cancellation.
		{
			task::cancel_source stop;
			stop.request();
			const task::cancel_token token = stop.token();
			string_scratch file;
			bool threw = false;
			try {
				(void)reorder::order(sources, file, set, &token);
			} catch (const task::cancelled&) {
				threw = true;
			}
			CHECK(threw);
		}

		// The index's forced merge reorders: one permuted segment, the same content, every key
		// and docid answering, and no forward index left behind; the policy's merges do not.
		{
			const std::uint64_t before = ix.digest();
			std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> keys;
			for (int k = 0; k < 900; ++k) {
				if (auto e = ix.find_key("Q" + std::to_string(k))) { keys["Q" + std::to_string(k)] = *e; }
			}
			ix.force_merge();
			ix.commit();
			CHECK(ix.segments().size() == 1);
			CHECK(!ix.segments()[0].seg()->core().docid_ordered());
			CHECK(ix.digest() == before);
			(void)ix.check();
			for (int k = 0; k < 900; ++k) {
				const auto e = ix.find_key("Q" + std::to_string(k));
				const auto it = keys.find("Q" + std::to_string(k));
				CHECK(e.has_value() == (it != keys.end()));
				if (e && it != keys.end()) { CHECK(*e == it->second && ix.has_docid(e->first)); }
			}
			CHECK(!has_file(io, ".fwd") && !has_file(io, ".rec"));
		}
		// A forward index left by a crash is an orphan open removes.
		{
			auto f = io.create_exclusive("seg-999999.fwd");
			f->write_at(0, std::string_view("partial"));
		}
		{ const prospect::index closed = std::move(ix); }
		CHECK(has_file(io, "seg-999999.fwd"));
		auto reopened = prospect::index::open(io, opts);
		CHECK(!has_file(io, "seg-999999.fwd"));
		CHECK(reopened.segments().size() == 1);
	}
	// T7: one reordering merge gives the same bytes on every platform and at 1 and 8 threads,
	// pinned by hash (zstd is the pinned release, so stored frames match too).
	{
		std::vector<std::string> files;
		std::mt19937_64 r(77);
		for (int part = 0; part < 4; ++part) {
			segment_writer w;
			for (int i = 0; i < 400; ++i) {
				const std::uint64_t docid = std::uint64_t(part) * 400 + std::uint64_t(i) + 1;
				std::vector<posting_input> in;
				static const std::vector<std::uint32_t> pos = {1, 3};
				const int topic = int(r() % 3);
				for (int k = 0; k < 6; ++k) {
					static const char* words[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
					if (r() % 2) { in.push_back({words[topic * 4 + k % 4], 2, pos}); }
				}
				std::sort(in.begin(), in.end(), [](const auto& x, const auto& y) { return x.term < y.term; });
				in.erase(std::unique(in.begin(), in.end(), [](const auto& x, const auto& y) { return x.term == y.term; }), in.end());
				w.add(docid, in, 6, "stored " + std::to_string(docid));
			}
			files.push_back(w.flush({}));
		}
		std::vector<segment> segs;
		for (const auto& f : files) { segs.emplace_back(f); }
		std::vector<merge_source> src;
		for (const auto& sg : segs) { src.push_back({&sg, nullptr}); }
		std::string out_bytes[2];
		for (int t = 0; t < 2; ++t) {
			reorder::settings set;
			set.min_df = 20;
			set.bisection.threads = t ? 8 : 1;
			string_scratch forward, records;
			const auto order = reorder::order(src, forward, set);
			CHECK(!order.empty());
			string_output out(out_bytes[t]);
			string_spill spill;
			const std::array<std::uint8_t, 16> id{7};
			(void)merge_segments(src, id, out, spill, {.order = order, .records = &records});
		}
		CHECK(out_bytes[0] == out_bytes[1]);
		const std::uint64_t h = xxh64::hash(out_bytes[0]);
		constexpr std::uint64_t pinned = 0xc770db3d97fb6957ull;  // strata format 0x8000000C's bytes
		if (h != pinned) { std::fprintf(stderr, "T7: reordering merge hash %016llx\n", (unsigned long long)h); }
		CHECK(h == pinned);
	}

	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("reorder: ok");
	return 0;
}
