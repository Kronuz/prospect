// Format 13's open rules (docs/design/format-13.md T1, step 7c-1): the frozen prefix (F2),
// F3's kinds and F5's canonical layout, F4's structure bits and presence, F6's reserved
// words, the sink's file order, and the format-12 refusal fixtures.
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/merge.hh"
#include "strata/check.hh"
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/searcher.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
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

// A segment holding `structures`, keyed when they include keys.
std::string profile_segment(std::uint32_t structures, std::uint32_t first = 1) {
	strata::segment_writer w;
	w.structures = structures;
	const bool inverted = structures & strata::format::bit::inverted, stored = structures & strata::format::bit::stored;
	for (std::uint32_t d = 0; d < 200; ++d) {
		const std::vector<std::uint32_t> pos{1, 3};
		std::vector<strata::posting_input> in;
		if (inverted) { in.push_back({d % 2 ? "odd" : "even", 2, pos}); }
		const bool keyed = structures & strata::format::bit::keys;
		w.add(first + d, in, inverted ? 2 + d % 9 : 0, stored ? std::string(1 + d % 5, 's') : std::string(), keyed ? 1 + d % 4 : 0,
			keyed ? "Q" + std::to_string(first - 1 + d) : std::string());
	}
	return w.flush({});
}

using document_layout = strata::layout<strata::keys_plugin, strata::stored_plugin>;
using document_segment = strata::basic_segment<document_layout>;

template <class S>
outcome open_as(const std::string& file) {
	try {
		S s(file);
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
		std::vector<strata::posting_input> in{{"alpha", 2, pos}};
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

// The file with one section's bytes replaced, laid out again with every checksum good.
std::string with_section(const std::string& file, section kind, const std::string& bytes) {
	auto f = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, f);
	std::string out;
	for (auto& e : toc) {
		while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
		const std::string b = e.kind == kind ? bytes : file.substr(e.offset, e.length);
		e = strata::format::describe(e.kind, out.size(), b, e.region_shift);
		out += b;
	}
	while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
	const std::string t = strata::format::encode_toc(toc);
	f.toc_offset = out.size();
	f.toc_length = t.size();
	f.toc_crc = hashes::crc32c(t.data(), t.size());
	return out + t + strata::format::encode_footer(f);
}

// The file with its prefix's version set to v and the prefix CRC recomputed.
std::string with_version(std::string file, std::uint32_t v) {
	std::memcpy(file.data() + 8, &v, 4);
	const std::uint32_t crc = hashes::crc32c(file.data(), 12);
	std::memcpy(file.data() + 12, &crc, 4);
	return file;
}

// The docids a term query matches over an index's live documents, through the planner's
// routing (F8): the term table resolves a key-prefixed term as a key leaf.
std::vector<std::uint64_t> matches(const strata::index& ix, std::string_view term, std::string_view key_prefix) {
	std::vector<strata::segment_snapshot> segs;
	for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted, v.dead}); }
	strata::term_table table(segs, key_prefix);
	auto build = [&](const strata::segment& seg) -> strata::scorer_ptr {
		const auto* info = table.info(seg, term);
		if (!info) { return nullptr; }
		return std::make_unique<strata::term_scorer>(seg.get<strata::inverted_plugin>(), *info);
	};
	std::vector<std::uint64_t> out;
	for (const auto& h : strata::search(segs, build, 1000)) { out.push_back(h.docid); }
	std::sort(out.begin(), out.end());
	return out;
}

strata::operation upsert(const std::string& key, const std::string& word) {
	strata::operation op;
	op.type = strata::operation::replace_key;
	op.key = key;
	op.doc.key = key;
	op.doc.terms.push_back({word, 1, {1}});
	op.doc.length = 1;
	op.doc.stored = key;
	return op;
}

strata::verdict::outcome apply(strata::index& ix, const strata::operation& op) {
	strata::transaction t;
	t.ops.push_back(op);
	return ix.apply(t, {}).result;
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
		const std::vector<section> order{section::header, section::postings, section::positions, section::terms, section::lengths, section::keys, section::stored,
			section::docids, section::stats, section::norms, section::term_index, section::versions, section::key_index};
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
		// Every section removed: damage.
		for (std::size_t i = 0; i < toc.size(); ++i) {
			const auto got = open(relayout(good, [&](entries& t) { t.erase(t.begin() + std::ptrdiff_t(i)); }));
			if (got != outcome::corrupt) { std::fprintf(stderr, "removing %s: %s\n", strata::format::name_of(toc[i].kind), name(got)); }
			CHECK(got == outcome::corrupt);
		}
		// Kinds outside F3's table, the retired pk_filter, and kinds whose structure bit is
		// clear (the reserved ones), in place of the last section: damage.
		for (std::uint16_t kind : {11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 200, 0xffff}) {
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
		op.doc.key = "Q1";
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

	// --- F4: free and absent structures, and the document profile (7c-4) -------------------
	{
		const std::uint32_t all = bit::inverted | bit::norms | bit::keys | bit::stored;
		const std::string full = profile_segment(all), no_stored = profile_segment(all & ~bit::stored),
			no_norms = profile_segment(all & ~bit::norms), document = profile_segment(bit::keys | bit::stored);
		// stored absent with its bit clear: opens, every record empty.
		{
			strata::segment s(no_stored);
			CHECK(!s.holds<strata::stored_plugin>() && s.holds<strata::inverted_plugin>() && s.holds<strata::keys_plugin>());
			CHECK(s.core().entry(section::stored) == nullptr);
			for (strata::ordinal_t o = 0; o < s.core().doc_count(); ++o) { CHECK(s.get<strata::stored_plugin>().stored(o).empty()); }
			CHECK(s.get<strata::keys_plugin>().version(3) == 1 + 3 % 4);
			s.verify_all();
		}
		// norms absent with its bit clear: each document's from its exact length, so scores
		// equal a segment that built them.
		{
			strata::segment with(full), without(no_norms);
			CHECK(without.core().entry(section::norms) == nullptr && without.holds<strata::inverted_plugin>());
			for (strata::ordinal_t o = 0; o < 200; ++o) { CHECK(without.get<strata::inverted_plugin>().norm(o) == with.get<strata::inverted_plugin>().norm(o)); }
			for (const char* t : {"odd", "even"}) {
				const auto a = with.get<strata::inverted_plugin>().term(t), b = without.get<strata::inverted_plugin>().term(t);
				CHECK(a && b);
				strata::term_scorer sa(with.get<strata::inverted_plugin>(), *a), sb(without.get<strata::inverted_plugin>(), *b);
				for (auto d = sa.next(), e = sb.next(); d != strata::no_more_docs || e != strata::no_more_docs; d = sa.next(), e = sb.next()) {
					CHECK(d == e && sa.score() == sb.score());
					if (d != e) { break; }
				}
			}
		}
		// inverted absent with its bit clear, the document profile (L12): no terms, every
		// length 0, keys and records as written; the search layout reads it too.
		{
			strata::segment s(document);
			CHECK(!s.holds<strata::inverted_plugin>() && s.holds<strata::keys_plugin>() && s.holds<strata::stored_plugin>());
			const auto& inv = s.get<strata::inverted_plugin>();
			CHECK(!inv.term("odd") && inv.length(7) == 0 && inv.total_length() == 0 && inv.norm(7) == 0);
			std::size_t terms = 0;
			inv.scan_terms([&](std::string_view, const strata::format::term_info&) { return ++terms, true; });
			CHECK(terms == 0);
			CHECK(s.get<strata::keys_plugin>().find("Q7", {}) == strata::ordinal_t(7) && s.get<strata::stored_plugin>().stored(7) == std::string(3, 's'));
			s.verify_all();
			// A binary with the document layout opens it, and refuses one with inverted.
			CHECK(open_as<document_segment>(document) == outcome::opens);
			CHECK(open_as<document_segment>(full) == outcome::unsupported);
			document_segment d(document);
			CHECK(d.get<strata::keys_plugin>().find("Q9", {}) == strata::ordinal_t(9) && d.get<strata::stored_plugin>().stored(9).size() == 5);
			CHECK(!d.holds<strata::inverted_plugin>());
			// A clear bit with its sections present, or a set bit with them absent: damage.
			CHECK(open(relayout(document, [](entries&) {}, [](strata::format::footer& f) { f.structures |= bit::inverted; })) == outcome::corrupt);
			CHECK(open(relayout(no_stored, [](entries&) {}, [](strata::format::footer& f) { f.structures |= bit::stored; })) == outcome::corrupt);
			CHECK(open(relayout(full, [](entries&) {}, [](strata::format::footer& f) { f.structures &= ~bit::stored; })) == outcome::corrupt);
		}
		// The writer takes structures from its configuration and refuses what they cannot
		// hold, and a nonzero version without a key anywhere (F9, F13-R9).
		{
			auto refuses = [](std::uint32_t structures, bool terms, bool record, std::uint64_t version = 0, std::string key = {}) {
				strata::segment_writer w;
				w.structures = structures;
				std::vector<strata::posting_input> in;
				if (terms) { in.push_back({"t", 1, {}}); }
				try {
					w.add(1, in, 1, record ? "r" : "", version, key);
					(void)w.flush({});
				} catch (const std::invalid_argument&) {
					return true;
				}
				return false;
			};
			CHECK(!refuses(bit::inverted | bit::norms | bit::stored, true, true));        // keyless (the log profile)
			CHECK(refuses(bit::inverted | bit::norms | bit::stored, true, true, 0, "Q1"));  // a key without keys
			CHECK(refuses(bit::inverted | bit::norms | bit::stored, true, true, 3));        // a version without a key
			CHECK(refuses(bit::inverted | bit::norms | bit::keys | bit::stored, true, true, 3));
			CHECK(!refuses(bit::inverted | bit::norms | bit::keys | bit::stored, true, true, 3, "Q1"));
			CHECK(refuses(bit::norms | bit::keys | bit::stored, false, true));    // norms without inverted
			CHECK(refuses(bit::keys | bit::stored, true, false));                 // terms without inverted
			CHECK(refuses(bit::inverted | bit::keys, true, true));                // a record without stored
			CHECK(!refuses(bit::keys | bit::stored, false, true));
		}
		// A merge takes its layout's structures, whatever its inputs held: records absent from
		// a source read as empty, terms absent as none.
		{
			const std::string later = profile_segment(bit::keys | bit::stored, 1001);
			strata::segment x(no_stored), y(later);
			const std::vector<strata::merge_source> src{{&x, nullptr}, {&y, nullptr}};
			std::string out;
			strata::string_output o(out);
			strata::string_spill spill;
			(void)strata::merge_segments(src, {}, o, spill, {});
			strata::segment m(out);
			CHECK(m.core().structures() == all && m.core().doc_count() == 400);
			CHECK(m.get<strata::stored_plugin>().stored(0).empty() && m.get<strata::stored_plugin>().stored(200) == std::string(1, 's'));
			CHECK(m.get<strata::inverted_plugin>().term("odd")->df == 100 && m.get<strata::inverted_plugin>().length(200) == 0);
			CHECK(m.get<strata::keys_plugin>().find("Q1000", {}) == strata::ordinal_t(200) && m.get<strata::keys_plugin>().version(200) == 1);
		}
	}

	// --- F9: keyless segments and the version rule (7c-5) ------------------------------------
	{
		// keys absent with its bit clear (the log profile): no key, every version 0.
		const std::string log = profile_segment(bit::inverted | bit::norms | bit::stored, 1);
		{
			strata::segment s(log);
			CHECK(!s.holds<strata::keys_plugin>() && s.holds<strata::inverted_plugin>());
			for (section k : {section::keys, section::key_index, section::versions}) { CHECK(s.core().entry(k) == nullptr); }
			const auto& keys = s.get<strata::keys_plugin>();
			CHECK(keys.version(5) == 0 && !keys.may_hold("Q5") && !keys.find("Q5", {}) && keys.key_count() == 0);
			std::size_t n = 0;
			keys.scan("", [&](std::string_view, strata::ordinal_t) { return ++n, true; });
			CHECK(n == 0);
			CHECK(s.get<strata::inverted_plugin>().term("odd")->df == 100);
		}
		// Versions near 2^64 - 1 at a width above 1: the writer writes base 0 at width 64, so
		// the reader's range check passes (F13-R25 item 7).
		{
			strata::segment_writer w;
			const std::uint64_t top = ~std::uint64_t(0);
			w.add(1, {}, 0, {}, top - 2, "Q1");
			w.add(2, {}, 0, {}, top, "Q2");
			const std::string bytes = w.flush({});
			CHECK(open(bytes) == outcome::opens);
			strata::segment s(bytes);
			CHECK(s.get<strata::keys_plugin>().version(0) == top - 2 && s.get<strata::keys_plugin>().version(1) == top);
		}
		// check() refuses a nonzero version without a key (F13-R9), which no writer writes.
		{
			strata::segment_writer w;
			for (std::uint64_t d = 1; d <= 10; ++d) { w.add(d, {}, 0, {}, d == 3 ? 7 : 0, d == 3 ? "Q3" : ""); }
			const std::string good_versions = w.flush({});
			strata::check_report r;
			strata::check_segment(strata::segment(good_versions), nullptr, r, "Q");
			std::string v;
			strata::format::put<std::uint64_t>(v, 5);
			v.push_back('\0');  // every document at version 5, width 0
			const std::string bad = with_section(good_versions, section::versions, v);
			CHECK(open(bad) == outcome::opens);
			bool threw = false;
			try {
				strata::check_report rr;
				strata::check_segment(strata::segment(bad), nullptr, rr, "Q");
			} catch (const strata::corrupt_segment&) {
				threw = true;
			}
			CHECK(threw);
		}
		// A keyless index writes keyless segments, and a document's version is 0 before and
		// after a flush, a docid replace included (F9, F13-R18).
		{
			strata::faultfs::filesystem fs;
			strata::faultfs::io io(fs);
			auto ix = strata::index::create(io, strata::index::options{}, {});
			strata::operation add;
			add.type = strata::operation::add;
			add.doc.terms.push_back({"w", 1, {1}});
			add.doc.length = 1;
			strata::transaction t;
			t.ops.push_back(add);
			const auto v1 = ix.apply(t, {});
			CHECK(v1.result == strata::verdict::accepted && v1.versions == std::vector<std::uint64_t>{0});
			strata::operation replace = add;
			replace.type = strata::operation::replace_docid;
			replace.docid = v1.docids[0];
			strata::transaction t2;
			t2.ops.push_back(replace);
			CHECK(ix.apply(t2, {}).versions == std::vector<std::uint64_t>{0});  // buffered
			const std::uint64_t before = ix.digest();
			ix.commit();
			CHECK(ix.digest() == before);
			CHECK(ix.apply(t2, {}).versions == std::vector<std::uint64_t>{0});  // flushed
			ix.commit();
			for (const auto& sv : ix.segments()) { CHECK(!sv.seg->holds<strata::keys_plugin>() && sv.seg->core().entry(section::versions) == nullptr); }
		}
	}

	// --- F8: keys leave the dictionary (7c-3) ------------------------------------------------
	{
		strata::faultfs::filesystem fs;
		strata::faultfs::io io(fs);
		strata::index::options o;
		o.key_prefix = "Q";
		o.auto_merge = false;
		auto ix = strata::index::create(io, o, {});
		// Stage 1: a term with the key prefix, a key without it, and a key unlike the
		// operation's are invalid (F8; F13-R25 item 2).
		{
			auto op = upsert("Qa", "w");
			op.doc.terms.push_back({"Qb", 0, {}});
			CHECK(apply(ix, op) == strata::verdict::invalid);
			auto add = upsert("Qa", "w");
			add.type = strata::operation::add;
			add.doc.key = "Xa";
			CHECK(apply(ix, add) == strata::verdict::invalid);
			auto other = upsert("Qa", "w");
			other.doc.key = "Qz";
			CHECK(apply(ix, other) == strata::verdict::invalid);
		}
		// Keys across segments, replaced and deleted, against a model of the live ones.
		std::map<std::string, std::uint64_t> live;  // key -> its live docid
		std::mt19937_64 r(13);
		for (int i = 0; i < 600; ++i) {
			const std::string key = "Q" + std::to_string(r() % 120);
			if (r() % 5 == 0) {
				strata::operation del;
				del.type = strata::operation::delete_key;
				del.key = key;
				(void)apply(ix, del);
				live.erase(key);
			} else {
				CHECK(apply(ix, upsert(key, "w" + std::to_string(r() % 7))) == strata::verdict::accepted);
				live[key] = 0;
			}
			if (i % 97 == 96) { ix.commit(); }
		}
		ix.commit();
		for (auto& [k, d] : live) { d = ix.find_key(k)->first; }
		CHECK(ix.segments().size() > 2);
		for (int k = 0; k < 120; ++k) {
			const std::string key = "Q" + std::to_string(k);
			const auto got = matches(ix, key, o.key_prefix);
			const auto it = live.find(key);
			CHECK(got == (it == live.end() ? std::vector<std::uint64_t>{} : std::vector<std::uint64_t>{it->second}));
			// The dictionary holds no key: without the prefix's routing, nothing matches.
			CHECK(matches(ix, key, {}).empty());
		}
		// The term table's df counts the segments holding a key, deleted copies included.
		{
			std::vector<strata::segment_snapshot> segs;
			std::uint64_t held = 0;
			for (const auto& v : ix.segments()) {
				segs.push_back({v.seg, v.deleted, v.dead});
				held += v.seg->get<strata::keys_plugin>().find("Q7", {}).has_value();
			}
			CHECK(strata::snapshot_df(segs, "Q7", o.key_prefix) == held);
		}
		const auto report = ix.check();
		CHECK(report.keys >= live.size() && report.terms >= 7);
		// check() refuses a dictionary that holds a key-prefixed term.
		{
			strata::segment_writer w;
			std::vector<strata::posting_input> in{{"Qx", 1, {}}};
			w.add(1, in, 1);
			const std::string bytes = w.flush({});
			strata::segment seg(bytes);
			strata::check_report rr;
			bool threw = false;
			try {
				strata::check_segment(seg, nullptr, rr, "Q");
			} catch (const strata::corrupt_segment&) {
				threw = true;
			}
			CHECK(threw);
			strata::check_report plain;
			strata::check_segment(seg, nullptr, plain);  // no prefix: an ordinary term
			CHECK(plain.terms == 1);
		}
		// The writer's backstop refuses a key-prefixed term, never drops it.
		{
			strata::segment_writer w;
			w.key_prefix = "Q";
			std::vector<strata::posting_input> in{{"Qx", 1, {}}};
			bool threw = false;
			try {
				w.add(1, in, 1);
			} catch (const std::invalid_argument&) {
				threw = true;
			}
			CHECK(threw);
		}
	}
	// A keyless index: no prefix, so no routing and no refusal (F13-R33). A term that
	// starts like a key is an ordinary term, and a document's key is ignored.
	{
		strata::faultfs::filesystem fs;
		strata::faultfs::io io(fs);
		strata::index::options o;
		auto ix = strata::index::create(io, o, {});
		strata::operation op;
		op.type = strata::operation::add;
		op.doc.terms.push_back({"Qa", 1, {1}});
		op.doc.terms.push_back({"w", 1, {2}});
		op.doc.key = "Qa";
		op.doc.length = 2;
		CHECK(apply(ix, op) == strata::verdict::accepted);
		ix.commit();
		CHECK(matches(ix, "Qa", {}).size() == 1 && matches(ix, "w", {}).size() == 1);
		CHECK(!ix.find_key("Qa"));
		CHECK(ix.check().keys == 0);
	}

	std::printf("format13: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
