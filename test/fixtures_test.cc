// Fixtures pinning format 12's bytes and the index digest before the layers refactor
// (layers.md, L14 step 0): every refactor step must reproduce them exactly. Regenerate
// only for an intended format change: STRATA_WRITE_FIXTURES=1 fixtures_test.
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/merge.hh"
#include "strata/reader.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

using namespace strata;

// Compares bytes with the named fixture, or writes it under STRATA_WRITE_FIXTURES.
static void pin(const std::string& name, const std::string& bytes) {
	const std::string path = std::string(STRATA_FIXTURES) + "/" + name;
	if (std::getenv("STRATA_WRITE_FIXTURES")) {
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
static std::string build(std::uint32_t docs, std::size_t stored, bool keyed, bool wide = false, std::uint32_t first = 1, std::uint32_t step = 1) {
	segment_writer w;
	w.wide_skip_offsets = wide;
	std::vector<std::vector<std::uint32_t>> pos;
	std::vector<posting_input> in;
	for (std::uint32_t d = 0; d < docs; ++d) {
		std::uint32_t length = terms_of(d, pos, in);
		std::string key;
		if (keyed) {
			key = "Q" + std::to_string(first + d * step);
			in.push_back({key, 0, {}});
		}
		if (in.empty()) {
			pos[0] = {1};
			in.push_back({words[0], 1, pos[0]});
			length = 1;
		}
		std::string record;
		for (std::size_t i = 0; i < stored; ++i) { record.push_back(char('a' + (d + i) % 26)); }
		w.add(first + d * step, in, length, record, 1 + d % 3, key);
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
	(void)merge_segments(sources, id, sink, spill, "Q");
	return out;
}

int main() {
	// Stored records across several 64 KiB frames.
	pin("stored-frames.sx", build(300, 1000, true));
	// No keys: no key filter section.
	pin("keyless.sx", build(200, 0, false));
	// Wide skip offsets.
	pin("wide-skips.sx", build(5000, 0, true, true));
	// A two-segment merge, and one with deletes in both inputs.
	const std::string a = build(400, 16, true, false, 1, 2), b = build(300, 16, true, false, 2, 2);
	pin("merge-two.sx", merged({a, b}, {{}, {}}));
	std::vector<std::uint64_t> da((400 + 63) / 64, 0), db((300 + 63) / 64, 0);
	for (std::uint32_t o = 0; o < 400; o += 3) { da[o >> 6] |= std::uint64_t(1) << (o & 63); }
	for (std::uint32_t o = 1; o < 300; o += 5) { db[o >> 6] |= std::uint64_t(1) << (o & 63); }
	pin("merge-deletes.sx", merged({a, b}, {da, db}));
	// A segment moved after open still answers: terms, postings, keys, and stored records
	// (under ASan in CI, any reader left pointing into the moved-from object faults).
	{
		const std::string file = build(300, 1000, true);
		segment first(file);
		segment moved(std::move(first));
		const auto info = moved.term("gamma");
		CHECK(info && info->df > 0);
		postings_iterator it(moved, *info);
		std::uint64_t n = 0;
		for (auto d = it.next(); d != no_more_docs; d = it.next()) { ++n; }
		CHECK(n == info->df);
		CHECK(moved.may_hold_key("Q7") && moved.version(6) == 1 + 6 % 3);
		CHECK(moved.stored(5).size() == 1000);
		moved.verify_all();
	}
	// An empty flush, when the writer allows one.
	try {
		segment_writer w;
		pin("empty.sx", w.flush(id));
	} catch (const std::exception&) {
		std::printf("  empty: the writer refuses an empty flush\n");
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
			x.terms.push_back({"Q" + std::to_string(d % 50), 0, {}});
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
	}
	std::printf("fixtures: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
