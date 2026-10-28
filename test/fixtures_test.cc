// Fixtures pinning format 13's bytes and the index digest (format-13.md F22): every commit must
// reproduce them exactly. Regenerate only for an intended format change:
// PROSPECT_WRITE_FIXTURES=1 fixtures_test.
#include "prospect/check.hh"
#include "prospect/index.hh"
#include "prospect/merge.hh"
#include "prospect/reader.hh"
#include "prospect/writer.hh"
#include "strata/core/faultfs.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

using namespace prospect;

// Compares bytes with the named fixture, or writes it under PROSPECT_WRITE_FIXTURES.
static void pin(const std::string& name, const std::string& bytes) {
	const std::string path = std::string(PROSPECT_FIXTURES) + "/" + name;
	if (std::getenv("PROSPECT_WRITE_FIXTURES")) {
		std::FILE* f = std::fopen(path.c_str(), "wb");
		std::fwrite(bytes.data(), 1, bytes.size(), f);
		std::fclose(f);
	}
	std::string golden;
	if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
		for (int c; (c = std::fgetc(f)) != EOF;) { golden.push_back(char(c)); }
		std::fclose(f);
	}
	if (golden != bytes) { std::fprintf(stderr, "fixture %s: %zu bytes, expected %zu\n", name.c_str(), bytes.size(), golden.size()); }
	CHECK(golden == bytes);
}

static const std::array<std::uint8_t, 16> id = {'f', 'i', 'x', 't', 'u', 'r', 'e', 's', 0, 0, 0, 0, 0, 0, 0, 1};
static const char* words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};

// Document d's terms, deterministic: each word at a few positions, some words absent.
static std::uint32_t terms_of(std::uint32_t d, std::vector<std::vector<std::uint32_t>>& pos, std::vector<posting_input>& in) {
	pos.assign(5, {});
	in.clear();
	std::uint32_t length = 0;
	for (std::uint32_t k = 0; k < 5; ++k) {
		if ((d * 7 + k * 3) % (k + 2) != 0) { continue; }
		for (std::uint32_t p = 0; p <= (d + k) % 4; ++p) { pos[k].push_back(1 + k + 5 * p); }
		length += std::uint32_t(pos[k].size());
	}
	for (std::uint32_t k = 0; k < 5; ++k) {
		if (!pos[k].empty()) { in.push_back({words[k], std::uint32_t(pos[k].size()), pos[k]}); }
	}
	return length;
}

// A segment of `docs` documents; stored records of `stored` bytes, keys when `keyed`.
static std::string build(std::uint32_t docs, std::size_t stored, bool keyed, bool wide = false, std::uint32_t first = 1, std::uint32_t step = 1, bool lists = false) {
	segment_writer w;
	if (lists) { w.structures |= strata::format::bit::term_lists; }
	w.builder<prospect::inverted_plugin>().wide_skip_offsets = wide;
	if (!keyed) { w.structures &= ~strata::format::bit::keys; }  // keyless: no keys structure (F4, F9)
	std::vector<std::vector<std::uint32_t>> pos;
	std::vector<posting_input> in;
	for (std::uint32_t d = 0; d < docs; ++d) {
		std::uint32_t length = terms_of(d, pos, in);
		std::string key;
		if (keyed) { key = "Q" + std::to_string(first + d * step); }  // the document's key, not a term (F8)
		if (in.empty()) {
			pos[0] = {1};
			in.push_back({words[0], 1, pos[0]});
			length = 1;
		}
		std::string record;
		for (std::size_t i = 0; i < stored; ++i) { record.push_back(char('a' + (d + i) % 26)); }
		w.add(first + d * step, in, length, record, keyed ? 1 + d % 3 : 0, key);  // a version belongs to a key (F9)
	}
	return w.flush(id);
}

// Document d's values: slot 1 on every document (three distinct, one empty), slot 7 on every
// fifth with arbitrary bytes, slot 100000 on every 37th, a long value on slot 0 now and then.
static void add_values(segment_writer& w, ordinal_t ord, std::uint32_t d) {
	auto& b = w.builder<values_plugin>();
	if (d % 37 == 0) { b.add_value(ord, 0, std::string(300 + d % 7, char('A' + d % 26))); }
	b.add_value(ord, 1, d % 3 == 0 ? std::string() : d % 3 == 1 ? "one" : "two");
	if (d % 5 == 0) { b.add_value(ord, 7, std::string("\x00\xff\x80", 3) + std::to_string(d * 31 % 17)); }
	if (d % 37 == 5) { b.add_value(ord, 100000, "rare " + std::to_string(d)); }
}

// A keyed segment of `docs` documents with values (docids first, first + step, ...).
static std::string build_values(std::uint32_t docs, std::uint32_t first = 1, std::uint32_t step = 1) {
	segment_writer w;
	for (std::uint32_t d = 0; d < docs; ++d) {
		const std::uint32_t docid = first + d * step;
		const ordinal_t ord = w.add(docid, {}, 0, "v" + std::to_string(d), 1 + d % 3, "Q" + std::to_string(docid));
		add_values(w, ord, docid);
	}
	return w.flush(id);
}

static std::string merged(const std::vector<std::string>& files, const std::vector<std::vector<std::uint64_t>>& deleted) {
	std::vector<segment> segs;
	segs.reserve(files.size());
	for (const auto& f : files) { segs.emplace_back(f); }
	std::vector<merge_source> sources;
	for (std::size_t i = 0; i < segs.size(); ++i) { sources.push_back({&segs[i], deleted[i].empty() ? nullptr : &deleted[i]}); }
	std::string out;
	string_output sink(out);
	string_spill spill;
	(void)merge_segments(sources, id, sink, spill, {.key_prefix = "Q"});
	return out;
}

// Generic over layouts (L11): contains<P>() guards get<P>() at compile time, holds<P>() reads the file.
struct outside_plugin {};
template <class P, class S>
static std::uint64_t docs_if_held(const S& s) {
	if constexpr (S::template contains<P>()) {
		if (s.template holds<P>()) { return s.template get<P>().doc_count(); }
	}
	return 0;
}
static_assert(segment::contains<prospect::inverted_plugin>() && segment::contains<strata::keys_plugin>() && segment::contains<strata::stored_plugin>());
static_assert(!segment::contains<outside_plugin>());

int main() {
	// Stored records across several 64 KiB frames.
	pin("stored-frames.sx", build(300, 1000, true));
	// No keys: no key filter section.
	pin("keyless.sx", build(200, 0, false));
	// The document profile (L12): keys and stored records, no inverted structure (F4).
	{
		segment_writer w;
		w.structures = strata::format::bit::keys | strata::format::bit::stored;
		for (std::uint32_t d = 0; d < 150; ++d) { w.add(d + 1, {}, 0, "record " + std::to_string(d), 1 + d % 3, "Q" + std::to_string(d)); }
		pin("document.sx", w.flush(id));
	}
	// Wide skip offsets.
	pin("wide-skips.sx", build(5000, 0, true, true));
	// A two-segment merge, and one with deletes in both inputs.
	const std::string a = build(400, 16, true, false, 1, 2), b = build(300, 16, true, false, 2, 2);
	pin("merge-two.sx", merged({a, b}, {{}, {}}));
	std::vector<std::uint64_t> da((400 + 63) / 64, 0), db((300 + 63) / 64, 0);
	for (std::uint32_t o = 0; o < 400; o += 3) { da[o >> 6] |= std::uint64_t(1) << (o & 63); }
	for (std::uint32_t o = 1; o < 300; o += 5) { db[o >> 6] |= std::uint64_t(1) << (o & 63); }
	pin("merge-deletes.sx", merged({a, b}, {da, db}));
	// Values: a flush, and a merge of two with deletes in both.
	pin("values.sx", build_values(400));
	{
		const std::string va = build_values(400, 1, 2), vb = build_values(300, 2, 2);
		pin("values-merge.sx", merged({va, vb}, {da, db}));
	}
	// Term lists: a flush, whose lists read back as the documents' terms, and a merge with
	// deletes in both inputs.
	{
		const std::string file = build(300, 40, true, false, 1, 1, true);
		pin("term-lists.sx", file);
		const segment s(file);
		CHECK(s.holds<prospect::term_lists_plugin>());
		prospect::term_lists_plugin::reader::frame frame;
		std::vector<std::vector<std::uint32_t>> pos;
		std::vector<posting_input> in;
		for (std::uint32_t d = 0; d < 300; ++d) {
			(void)terms_of(d, pos, in);
			if (in.empty()) { in.push_back({words[0], 1, {}}); }
			std::vector<std::pair<std::string, std::uint32_t>> want;
			for (const auto& t : in) { want.emplace_back(std::string(t.term), t.wdf); }
			std::sort(want.begin(), want.end());
			std::vector<std::pair<std::string, std::uint32_t>> got;
			for (auto c = s.get<prospect::term_lists_plugin>().list(d, frame).begin(); c.next();) { got.emplace_back(std::string(c.term()), c.wdf()); }
			CHECK(got == want);
		}
	}
	{
		const std::string a2 = build(400, 16, true, false, 1, 2, true), b2 = build(300, 16, true, false, 2, 2, true);
		std::vector<std::uint64_t> da2((400 + 63) / 64, 0), db2((300 + 63) / 64, 0);
		for (std::uint32_t o = 0; o < 400; o += 3) { da2[o >> 6] |= std::uint64_t(1) << (o & 63); }
		for (std::uint32_t o = 1; o < 300; o += 5) { db2[o >> 6] |= std::uint64_t(1) << (o & 63); }
		const std::string m = merged({a2, b2}, {da2, db2});
		pin("term-lists-merge.sx", m);
		const segment ms(m);
		prospect::check_report rep;
		prospect::check_segment(ms, nullptr, rep, "Q");
		CHECK(rep.listed_documents == ms.core().doc_count() && rep.listed_documents == 400 - 134 + 300 - 60);
	}
	// A segment moved after open still answers: terms, postings, keys, and stored records
	// The source is freed before any read, so under ASan a reader left pointing into it faults.
	{
		const std::string file = build(300, 1000, true);
		auto first = std::make_unique<segment>(file);
		segment moved(std::move(*first));
		first.reset();
		const auto info = moved.get<prospect::inverted_plugin>().term("gamma");
		CHECK(info && info->df > 0);
		postings_iterator it(moved.get<prospect::inverted_plugin>(), *info);
		std::uint64_t n = 0;
		for (auto d = it.next(); d != no_more_docs; d = it.next()) { ++n; }
		CHECK(n == info->df);
		CHECK(moved.get<strata::keys_plugin>().may_hold("Q7") && moved.get<strata::keys_plugin>().version(6) == 1 + 6 % 3);
		CHECK(moved.get<strata::stored_plugin>().stored(5).size() == 1000);
		moved.verify_all();
		// Presence is per segment (L11): this segment holds every plugin of its layout.
		CHECK(moved.holds<prospect::inverted_plugin>() && moved.holds<strata::keys_plugin>() && moved.holds<strata::stored_plugin>());
		CHECK(!moved.holds<outside_plugin>());
		CHECK(docs_if_held<prospect::inverted_plugin>(moved) == 300 && docs_if_held<outside_plugin>(moved) == 0);
	}
	// An empty flush: empty.sx is pinned, so a writer that starts refusing one fails here.
	try {
		segment_writer w;
		pin("empty.sx", w.flush(id));
	} catch (const std::exception& e) {
		std::fprintf(stderr, "empty.sx: the writer refuses an empty flush: %s\n", e.what());
		CHECK(false);
	}
	// The digest of a fixed script: keys, replaces, deletes, metadata, buffered documents.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options o;
		o.key_prefix = "Q";
		o.auto_merge = false;
		const std::array<char, 16> uuid = {'f', 'i', 'x', 't', 'u', 'r', 'e', 's', 'd', 'i', 'g', 'e', 's', 't', '0', '1'};
		auto ix = index::create(io, o, uuid);
		auto doc = [](std::uint32_t d) {
			document x;
			for (std::uint32_t k = 0; k < 5; ++k) {
				if ((d + k) % 3 == 0) { continue; }
				x.terms.push_back({words[k], 1 + (d + k) % 3, {}});
				for (std::uint32_t p = 0; p < x.terms.back().wdf; ++p) { x.terms.back().positions.push_back(1 + k + 7 * p); }
				x.length += x.terms.back().wdf;
			}
			x.key = "Q" + std::to_string(d % 50);
			x.stored = "doc" + std::to_string(d);
			return x;
		};
		for (std::uint32_t d = 0; d < 120; ++d) {
			transaction t;
			operation op;
			op.type = operation::replace_key;  // an upsert: the first of a key adds, later ones replace
			op.key = "Q" + std::to_string(d % 50);
			op.doc = doc(d);
			t.ops.push_back(op);
			if (d % 10 == 9) {
				operation del;
				del.type = operation::delete_key;
				del.key = "Q" + std::to_string((d * 7) % 50);
				t.ops.push_back(del);
			}
			if (d % 25 == 0) {
				operation m;
				m.type = operation::set_meta;
				m.meta_key = "m" + std::to_string(d);
				m.meta_value = std::to_string(d * d);
				t.ops.push_back(m);
			}
			ix.apply(t, {});
			if (d % 40 == 39) { ix.commit(); }
		}
		const std::uint64_t digest = ix.digest();  // with documents still buffered
		char text[32];
		std::snprintf(text, sizeof text, "%016llx\n", static_cast<unsigned long long>(digest));
		pin("digest.txt", text);
		// The same script's documents with values, still buffered, flushed, and merged away.
		const std::array<char, 16> uuid2 = {'f', 'i', 'x', 't', 'u', 'r', 'e', 's', 'v', 'a', 'l', 'u', 'e', 's', '0', '1'};
		faultfs::filesystem fs2;
		faultfs::io io2(fs2);
		auto vx = index::create(io2, o, uuid2);
		for (std::uint32_t d = 0; d < 120; ++d) {
			transaction t;
			operation op;
			op.type = operation::replace_key;
			op.key = "Q" + std::to_string(d % 50);
			op.doc = doc(d);
			if (d % 3) { op.doc.values.push_back({1, "g" + std::to_string(d % 4)}); }
			if (d % 7 == 0) { op.doc.values.push_back({9, std::string(d, 'z')}); }
			t.ops.push_back(op);
			vx.apply(t, {});
		}
		const std::uint64_t buffered = vx.digest();
		vx.refresh();
		CHECK(vx.digest() == buffered);
		vx.force_merge();
		CHECK(vx.digest() == buffered);
		char vtext[32];
		std::snprintf(vtext, sizeof vtext, "%016llx\n", static_cast<unsigned long long>(buffered));
		pin("digest-values.txt", vtext);
	}
	// F13 (M20): the seed pairs of a fixed corpus, pinned (seeds.txt) so every platform writes the
	// same pairs. The average length is exactly 5, which makes exact ties between levels (the
	// durability review's replay, DM-R51), and lengths and wdfs vary per term.
	{
		std::mt19937_64 rng(13);
		segment_writer w;
		w.structures &= ~strata::format::bit::keys;
		std::vector<std::string> names;
		for (int t = 0; t < 24; ++t) { names.push_back("s" + std::to_string(t)); }
		std::vector<std::vector<std::uint32_t>> none(names.size());
		for (std::uint32_t d = 0; d < 6000; ++d) {
			const std::uint32_t delta = std::uint32_t(rng() % 5);
			const std::uint32_t length = d % 2 ? 5 - delta : 5 + delta;  // pairs average 5
			std::vector<posting_input> in;
			for (std::size_t t = 0; t < names.size(); ++t) {
				if (rng() % (t + 2) != 0) { continue; }
				const std::uint32_t top = std::uint32_t(1 + t % 6);
				in.push_back({names[t], std::uint32_t(1 + rng() % top), none[t]});
			}
			w.add(1 + d, in, length);
		}
		const std::string bytes = w.flush(id);
		const segment seg(bytes);
		const auto& inv = seg.get<inverted_plugin>();
		std::string text;
		for (const auto& name : names) {
			const auto info = inv.term(name);
			if (!info) { continue; }
			postings_iterator it(inv, *info);
			for (std::size_t j = 0; j < it.seed_count(); ++j) {
				const auto& p = it.seed_at(j);
				char line[96];
				std::snprintf(line, sizeof line, "%s %u %u %u %u %u\n", name.c_str(), prospect::format::seed_ks[j], p.code_wdf, unsigned(p.code), p.exact_wdf, p.exact);
				text += line;
			}
		}
		CHECK(!text.empty());
		pin("seeds.txt", text);
	}
	// C17: each family's bytes change only with its pre-freeze version. pre_freeze.txt pins
	// both versions beside a hash of the family's fixtures (the segments' .sx, the files'
	// .bin), so a re-pin that keeps the version fails, and so does a bump left unrecorded.
	{
		auto family = [](const char* ext) {
			std::vector<std::string> names;
			for (const auto& e : std::filesystem::directory_iterator(PROSPECT_FIXTURES)) {
				if (e.is_regular_file() && e.path().extension() == ext) { names.push_back(e.path().filename().string()); }
			}
			std::sort(names.begin(), names.end());
			std::string all;
			for (const auto& n : names) {
				std::ifstream in(std::string(PROSPECT_FIXTURES) + "/" + n, std::ios::binary);
				const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				all += n + '\0' + std::to_string(bytes.size()) + '\0' + bytes;
			}
			return xxh64::hash(all);
		};
		struct row {
			unsigned long long version = 0, hash = 0;
		};
		const row now[2] = {{strata::format::pre_freeze, family(".sx")}, {files::pre_freeze, family(".bin")}};
		const char* names[2] = {"segments", "files"};
		const std::string path = std::string(PROSPECT_FIXTURES) + "/pre_freeze.txt";
		row was[2];
		if (std::FILE* f = std::fopen(path.c_str(), "r")) {
			for (auto& w : was) {
				char name[16];
				if (std::fscanf(f, "%15s %llx %llx", name, &w.version, &w.hash) != 3) { break; }
			}
			std::fclose(f);
		}
		bool bumped = true;
		for (int i = 0; i < 2; ++i) {
			if (now[i].hash != was[i].hash && now[i].version == was[i].version) {
				std::fprintf(stderr, "pre_freeze.txt: the %s fixtures changed without a pre-freeze bump (C17)\n", names[i]);
				bumped = false;
			}
		}
		CHECK(bumped);
		char text[128];
		std::snprintf(text, sizeof text, "segments %08llx %016llx\nfiles %08llx %016llx\n", now[0].version, now[0].hash, now[1].version, now[1].hash);
		if (bumped) { pin("pre_freeze.txt", text); }
	}
	std::printf("fixtures: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
