// Format 13's open rules (docs/design/format-13.md T1, step 7c-1): the frozen prefix (F2),
// F3's kinds and F5's canonical layout, F4's structure bits and presence, F6's reserved
// words, the sink's file order, and the format-12 refusal fixtures.
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/reader.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 40) std::exit(1); } \
	} while (0)

namespace {

using strata::format::section;
namespace bit = strata::format::bit;
using entries = std::vector<strata::format::section_entry>;

enum class outcome { opens, corrupt, unsupported, other };

const char* name(outcome o) {
	switch (o) {
	case outcome::opens: return "opens";
	case outcome::corrupt: return "corrupt";
	case outcome::unsupported: return "unsupported";
	default: return "other";
	}
}

outcome open(const std::string& file) {
	try {
		strata::segment s(file);
		return outcome::opens;
	} catch (const strata::corrupt_segment&) {
		return outcome::corrupt;
	} catch (const strata::unsupported_segment&) {
		return outcome::unsupported;
	} catch (...) {
		return outcome::other;
	}
}

std::string keyed_segment() {
	strata::segment_writer w;
	for (std::uint32_t d = 0; d < 300; ++d) {
		const std::string key = "Q" + std::to_string(d);
		const std::vector<std::uint32_t> pos{1, 4}, beta{7};
		std::vector<strata::posting_input> in{{"alpha", 2, pos}, {key, 0, {}}};
		if (d % 3 == 0) { in.push_back({"beta", 1, beta}); }
		w.add(2 * d + 1, in, 3, std::string(d % 7, 'x'), 1 + d % 5, key);
	}
	return w.flush({});
}

// The file laid out again from `edit`'s entries, in the order given, each section's bytes
// copied from the original and `pad` extra zero bytes before entry `pad_at`; every checksum
// recomputed; `footer` then edits the footer.
std::string relayout(const std::string& file, const std::function<void(entries&)>& edit, const std::function<void(strata::format::footer&)>& footer = {},
	std::size_t pad_at = ~std::size_t(0), std::size_t pad = 0) {
	auto f = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, f);
	std::vector<std::string> bytes;
	for (const auto& e : toc) { bytes.push_back(file.substr(e.offset, e.length)); }
	for (std::size_t i = 0; i < toc.size(); ++i) { toc[i].offset = i; }  // remember each entry's source
	edit(toc);
	std::string out;
	for (std::size_t i = 0; i < toc.size(); ++i) {
		auto& e = toc[i];
		while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
		if (i == pad_at) { out.append(pad, '\0'); }
		const std::string& b = bytes[e.offset];
		e = strata::format::describe(e.kind, out.size(), b, e.region_shift);
		out += b;
	}
	while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
	if (pad_at == toc.size()) { out.append(pad, '\0'); }
	const std::string t = strata::format::encode_toc(toc);
	f.toc_offset = out.size();
	f.toc_length = t.size();
	f.toc_crc = hashes::crc32c(t.data(), t.size());
	out += t;
	if (footer) { footer(f); }
	return out + strata::format::encode_footer(f);
}

// The file with its prefix's version set to v and the prefix CRC recomputed.
std::string with_version(std::string file, std::uint32_t v) {
	std::memcpy(file.data() + 8, &v, 4);
	const std::uint32_t crc = hashes::crc32c(file.data(), 12);
	std::memcpy(file.data() + 12, &crc, 4);
	return file;
}

std::string read_file(const std::string& path) {
	std::string out;
	if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
		for (int c; (c = std::fgetc(f)) != EOF;) { out.push_back(char(c)); }
		std::fclose(f);
	}
	return out;
}

}  // namespace

int main() {
	const std::string good = keyed_segment();
	CHECK(open(good) == outcome::opens);
	{
		strata::segment s(good);
		CHECK(s.core().version() == 13);
		CHECK(s.core().structures() == (bit::inverted | bit::norms | bit::keys | bit::stored));
		// F5's order: the streamed sections, then the pinned tail by rank.
		const std::vector<section> order{section::header, section::postings, section::positions, section::terms, section::lengths, section::stored,
			section::docids, section::stats, section::norms, section::term_index, section::versions, section::pk_filter};
		std::vector<section> got;
		for (const auto& e : s.core().sections()) { got.push_back(e.kind); }
		CHECK(got == order);
		// Every section at the previous one's 8-byte-aligned end (F5).
		CHECK(s.core().sections()[0].offset == 0 && s.core().sections()[0].length == strata::format::header_size);
	}
	// The layout is canonical: laying it out again gives the same bytes.
	CHECK(relayout(good, [](entries&) {}) == good);

	// --- F2: the prefix ------------------------------------------------------------------
	{
		// The prefix CRC of version 13, from the bytes.
		CHECK(strata::format::get<std::uint32_t>(good.data() + 12) == 0x4ff2df7bu);
		CHECK(strata::format::read_version(good) == 13);
		// The magic wrong: damage.
		for (std::size_t at = 0; at < 8; ++at) {
			std::string bad = good;
			bad[at] ^= 0x20;
			CHECK(open(bad) == outcome::corrupt);
		}
		// Any bit of bytes 8 to 15 flipped: the CRC fails and bytes 12 to 15 are not zero, so
		// damage, never another version (rule 4).
		for (std::size_t at = 8; at < 16; ++at) {
			for (unsigned b = 0; b < 8; ++b) {
				std::string bad = good;
				bad[at] = char(bad[at] ^ (1 << b));
				const auto got = open(bad);
				if (got != outcome::corrupt) { std::fprintf(stderr, "prefix byte %zu bit %u: %s\n", at, b, name(got)); }
				CHECK(got == outcome::corrupt);
			}
		}
		// Another version with a good CRC: unsupported, below or above (rule 2).
		CHECK(open(with_version(good, 14)) == outcome::unsupported);
		CHECK(open(with_version(good, 12)) == outcome::unsupported);
		// Format 12's zero flags word where the CRC sits: unsupported (rule 3).
		{
			std::string old = good;
			const std::uint32_t v = 12, zero = 0;
			std::memcpy(old.data() + 8, &v, 4);
			std::memcpy(old.data() + 12, &zero, 4);
			CHECK(open(old) == outcome::unsupported);
		}
		// Too short for a prefix: damage.
		CHECK(open(good.substr(0, 10)) == outcome::corrupt);
		// The real format-12 files: each refused as unsupported (F2).
		for (const char* f : {"small.sx", "permuted.sx", "stored-frames.sx", "keyless.sx", "wide-skips.sx", "merge-two.sx", "merge-deletes.sx", "empty.sx"}) {
			const std::string bytes = read_file(std::string(STRATA_FIXTURES) + "/format12/" + f);
			CHECK(!bytes.empty());
			const auto got = open(bytes);
			if (got != outcome::unsupported) { std::fprintf(stderr, "format12/%s: %s\n", f, name(got)); }
			CHECK(got == outcome::unsupported);
		}
	}

	// --- F3 and F5: kinds and the canonical layout -----------------------------------------
	{
		const auto toc = strata::format::decode_toc(good, strata::format::decode_footer(good));
		// Every section removed: damage. pk_filter is optional until key_index replaces it.
		for (std::size_t i = 0; i < toc.size(); ++i) {
			if (toc[i].kind == section::pk_filter) { continue; }
			const auto got = open(relayout(good, [&](entries& t) { t.erase(t.begin() + std::ptrdiff_t(i)); }));
			if (got != outcome::corrupt) { std::fprintf(stderr, "removing %s: %s\n", strata::format::name_of(toc[i].kind), name(got)); }
			CHECK(got == outcome::corrupt);
		}
		// Kinds outside F3's table, and kinds whose structure bit is clear (the reserved ones,
		// and keys and key_index before F8), in place of the last section: damage.
		for (std::uint16_t kind : {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 200, 0xffff}) {
			const auto got = open(relayout(good, [&](entries& t) { t.back().kind = section(kind); }));
			if (got != outcome::corrupt) { std::fprintf(stderr, "kind %u: %s\n", unsigned(kind), name(got)); }
			CHECK(got == outcome::corrupt);
		}
		// Entries out of rank order, a duplicate, the header moved: damage.
		CHECK(open(relayout(good, [](entries& t) { std::swap(t[1], t[2]); })) == outcome::corrupt);
		CHECK(open(relayout(good, [](entries& t) { std::swap(t[t.size() - 1], t[t.size() - 2]); })) == outcome::corrupt);
		CHECK(open(relayout(good, [](entries& t) { std::swap(t[0], t[1]); })) == outcome::corrupt);
		CHECK(open(relayout(good, [](entries& t) { t.push_back(t.back()); })) == outcome::corrupt);
		// A gap, a misaligned section, and a gap before the table: damage, even with every
		// checksum good.
		for (std::size_t at = 1; at <= toc.size(); ++at) {
			CHECK(open(relayout(good, [](entries&) {}, {}, at, 8)) == outcome::corrupt);
			CHECK(open(relayout(good, [](entries&) {}, {}, at, 1)) == outcome::corrupt);
		}
		// Overlapping sections: an entry moved back into its predecessor.
		{
			std::string bad = good;
			auto f = strata::format::decode_footer(bad);
			auto t = strata::format::decode_toc(bad, f);
			t[3].offset -= 8;
			const std::string tb = strata::format::encode_toc(t);
			bad.replace(f.toc_offset, tb.size(), tb);
			f.toc_crc = hashes::crc32c(tb.data(), tb.size());
			const std::string fb = strata::format::encode_footer(f);
			bad.replace(bad.size() - fb.size(), fb.size(), fb);
			CHECK(open(bad) == outcome::corrupt);
		}
	}

	// --- F6: reserved words ----------------------------------------------------------------
	CHECK(open(relayout(good, [](entries&) {}, [](strata::format::footer& f) { (void)f; })) == outcome::opens);
	{
		std::string file = good;
		const std::size_t at = file.size() - strata::format::footer_size;
		file[at + 44] = 1;
		const std::uint32_t crc = hashes::crc32c(file.data() + at, 48);
		std::memcpy(file.data() + at + 48, &crc, 4);
		CHECK(open(file) == outcome::corrupt);
	}
	{
		std::string file = good;
		file[16] = 1;  // the header's flags word, past the prefix
		CHECK(open(file) == outcome::corrupt);  // the header's section CRC fails, unresealed
		CHECK(open(relayout(file, [](entries&) {})) == outcome::corrupt);
	}

	// --- F4: structure bits --------------------------------------------------------------
	{
		const auto with_bits = [&](std::uint32_t structures) {
			return open(relayout(good, [](entries&) {}, [&](strata::format::footer& f) { f.structures = structures; }));
		};
		const std::uint32_t built = bit::inverted | bit::norms | bit::keys | bit::stored;
		// A built structure's bit cleared: its sections present without it, or norms without
		// inverted: damage.
		for (std::uint32_t b : {bit::inverted, bit::norms, bit::keys, bit::stored}) { CHECK(with_bits(built & ~b) == outcome::corrupt); }
		// index_sort without values: damage.
		CHECK(with_bits(built | bit::index_sort) == outcome::corrupt);
		// A sub-structure without its owner's bit: damage, whatever the binary reads.
		CHECK(with_bits(bit::norms | bit::keys | bit::stored) == outcome::corrupt);
		CHECK(with_bits((built & ~bit::stored) | bit::stored_spans) == outcome::corrupt);
		// Every other bit, defined, reserved, or unlisted: one this binary lacks, so
		// unsupported (F4; F13-R30), and never read past.
		for (unsigned b = 0; b < 32; ++b) {
			const std::uint32_t v = std::uint32_t(1) << b;
			if (built & v || v == bit::index_sort) { continue; }
			const auto got = with_bits(built | v);
			if (got != outcome::unsupported) { std::fprintf(stderr, "bit %u: %s\n", b, name(got)); }
			CHECK(got == outcome::unsupported);
		}
		CHECK(with_bits(built | bit::values | bit::index_sort) == outcome::unsupported);
		// The classification at the format level.
		const auto toc = strata::format::decode_toc(good, strata::format::decode_footer(good));
		bool threw = false;
		try {
			strata::format::check_structures(toc, built | bit::compact, built);
		} catch (const strata::unsupported_segment&) {
			threw = true;
		}
		CHECK(threw);
	}

	// --- F5: the sink's file order ---------------------------------------------------------
	{
		std::string bytes;
		strata::string_output out(bytes);
		strata::section_sink sink(out);
		sink.write(section::header, strata::format::encode_header({}));
		sink.write(section::stored, "s");
		bool threw = false;
		try {
			sink.write(section::terms, "t");  // rank 3 after rank 6
		} catch (const std::logic_error&) {
			threw = true;
		}
		CHECK(threw);
		threw = false;
		try {
			sink.write(section::header, strata::format::encode_header({}));
		} catch (const std::logic_error&) {
			threw = true;
		}
		CHECK(threw);
		// Pinned sections wait for finish() and come out in rank order.
		sink.write(section::versions, "v");
		sink.write(section::docids, "d");
		CHECK(sink.toc().size() == 2);
		sink.finish();
		CHECK(sink.toc().size() == 4 && sink.toc()[2].kind == section::docids && sink.toc()[3].kind == section::versions);
		CHECK(sink.toc()[3].offset % strata::format::section_alignment == 0);
	}

	// --- At the index: a manifest-named unsupported segment is damage (before F20) -----------
	{
		strata::faultfs::filesystem fs;
		strata::faultfs::io io(fs);
		strata::index::options o;
		o.key_prefix = "Q";
		{
		auto ix = strata::index::create(io, o, {});
		strata::transaction t;
		strata::operation op;
		op.type = strata::operation::replace_key;
		op.key = "Q1";
		op.doc.terms.push_back({"alpha", 1, {1}});
		op.doc.terms.push_back({"Q1", 0, {}});
		op.doc.length = 1;
		t.ops.push_back(op);
		ix.apply(t, {});
		ix.commit();
		}
		std::string seg;
		for (const auto& n : fs.list()) {
			if (n.starts_with("seg-")) { seg = n; }
		}
		CHECK(!seg.empty());
		const std::string bytes(fs.view(seg));
		{
			auto f = io.open_existing(seg);
			f->write_at(0, with_version(bytes, 14).substr(0, 16));
		}
		bool corrupt = false, unsupported = false;
		try {
			auto again = strata::index::open(io, o);
		} catch (const strata::corrupt_segment&) {
			corrupt = true;
		} catch (const strata::corrupt_index&) {
			corrupt = true;
		} catch (const strata::unsupported_segment&) {
			unsupported = true;
		}
		CHECK(corrupt && !unsupported);
	}

	std::printf("format13: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
