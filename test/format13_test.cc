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

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
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
std::string profile_segment(std::uint64_t structures, std::uint32_t first = 1) {
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

// A later release's segment (F23): `file`'s sections with `extra` inserted before the first
// section of kind `before` (at the end without one), laid out canonically, `structures` in
// the footer, every checksum good; `toc` replaces the table's encoding.
std::string later_release(const std::string& file, std::optional<std::pair<section, std::string>> extra, std::optional<section> before, std::uint64_t structures,
	const std::function<std::string(const entries&)>& toc = {}) {
	auto f = strata::format::decode_footer(file);
	auto old = strata::format::decode_toc(file, f);
	std::vector<std::pair<section, std::string>> parts;
	for (const auto& e : old) { parts.push_back({e.kind, file.substr(e.offset, e.length)}); }
	if (extra) {
		auto it = parts.begin();
		while (before && it != parts.end() && it->first != *before) { ++it; }
		if (!before) { it = parts.end(); }
		parts.insert(it, *extra);
	}
	std::string out;
	entries es;
	for (const auto& [kind, bytes] : parts) {
		while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
		es.push_back(strata::format::describe(kind, out.size(), bytes));
		out += bytes;
	}
	while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
	const std::string t = toc ? toc(es) : strata::format::encode_toc(es);
	f.toc_offset = out.size();
	f.toc_length = t.size();
	f.toc_crc = hashes::crc32c(t.data(), t.size());
	f.structures = structures;
	return out + t + strata::format::encode_footer(f);
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
		CHECK(s.core().version() == strata::format::version && strata::format::version == strata::format::pre_freeze);
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
		// The prefix CRC of the pre-freeze version, from the bytes (13's is 0x4ff2df7b).
		CHECK(strata::format::get<std::uint32_t>(good.data() + 12) == 0x4062b616u);
		CHECK(strata::format::read_version(good) == strata::format::pre_freeze);
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
		// Until the freeze, 13 itself is another build's layout: unsupported (CR11).
		CHECK(open(with_version(good, 13)) == outcome::unsupported);
		CHECK(open(with_version(good, strata::format::pre_freeze - 1)) == outcome::unsupported);
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

	// --- T7: a fresh open touches the header page and the tail only (F13-R25 item 5) -----------
	{
		// Large enough that whole pages lie between the header page and the tail at any page
		// size the CI hosts use (16 KiB on macOS arm64). Every profile an index can write,
		// norms absent included (F13-R36).
		auto big = [](std::uint64_t structures) {
			strata::segment_writer w;
			w.structures = structures;
			std::mt19937_64 r(5);
			for (std::uint32_t d = 0; d < 20000; ++d) {
				const std::vector<std::uint32_t> pos{1 + d % 7};
				const std::string term = "t" + std::to_string(r() % 5000);  // outlives the posting's view
				std::vector<strata::posting_input> in;
				if (structures & bit::inverted) { in = {{term, 1, pos}, {"all", 1, {}}}; }
				w.add(d + 1, in, (structures & bit::inverted) ? 2 + d % 11 : 0, std::string(40 + d % 60, char('a' + d % 26)), 1 + d % 9, "Q" + std::to_string(d));
			}
			return w.flush({});
		};
		auto check_open = [](const std::string& file) {
			const std::size_t page = std::size_t(::sysconf(_SC_PAGESIZE));
			const std::size_t mapped = (file.size() + page - 1) / page * page;
			void* map = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			CHECK(map != MAP_FAILED);
			std::memcpy(map, file.data(), file.size());
			// The tail starts at the first pinned section after the header (F5).
			std::uint64_t tail = file.size();
			for (const auto& e : strata::format::decode_toc(file, strata::format::decode_footer(file))) {
				if (e.kind != section::header && strata::format::rule_of(e.kind)->verify == strata::format::check::pinned) { tail = std::min(tail, e.offset); }
			}
			const std::size_t from = page, to = std::size_t(tail) / page * page;  // whole pages strictly between
			CHECK(to > from);
			if (to > from) { CHECK(::mprotect(static_cast<char*>(map) + from, to - from, PROT_NONE) == 0); }
			{
				strata::segment s(std::string_view(static_cast<const char*>(map), file.size()));  // a read there faults
				CHECK(s.core().verified_regions() == 0);
				CHECK(s.core().doc_count() == 20000 && s.get<strata::keys_plugin>().may_hold("Q7"));
			}
			if (to > from) { CHECK(::mprotect(static_cast<char*>(map) + from, to - from, PROT_READ | PROT_WRITE) == 0); }
			::munmap(map, mapped);
		};
		const std::uint32_t all = bit::inverted | bit::norms | bit::keys | bit::stored;
		check_open(big(all));
		check_open(big(all & ~bit::norms));
		check_open(big(bit::keys | bit::stored));
		// Norms derived on first use equal the built ones, and a damaged lengths section fails
		// that first use as damage, not at open.
		{
			const std::string with = big(all), without = big(all & ~bit::norms);
			strata::segment a(with), b(without);
			for (strata::ordinal_t o = 0; o < 20000; o += 37) { CHECK(a.get<strata::inverted_plugin>().norm(o) == b.get<strata::inverted_plugin>().norm(o)); }
			std::string bad = without;
			for (const auto& e : strata::format::decode_toc(bad, strata::format::decode_footer(bad))) {
				if (e.kind == section::lengths) { bad[e.offset + e.length / 2] ^= 1; }
			}
			strata::segment c(bad);
			bool threw = false;
			try {
				(void)c.get<strata::inverted_plugin>().norm(0);
			} catch (const strata::corrupt_segment&) {
				threw = true;
			}
			CHECK(threw);
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
		// The footer's second structures word holds bits 32 to 63 (F13-R40): each byte of it
		// set and resealed is a structure this binary lacks, unsupported, never damage.
		for (int byte = 44; byte < 48; ++byte) {
			std::string file = good;
			const std::size_t at = file.size() - strata::format::footer_size;
			file[at + byte] = 1;
			const std::uint32_t crc = hashes::crc32c(file.data() + at, 48);
			std::memcpy(file.data() + at + 48, &crc, 4);
			CHECK(open(file) == outcome::unsupported);
		}
	}
	{
		std::string file = good;
		file[16] = 1;  // the header's flags word, past the prefix
		CHECK(open(file) == outcome::corrupt);  // the header's section CRC fails, unresealed
		CHECK(open(relayout(file, [](entries&) {})) == outcome::corrupt);
	}

	// --- F4: structure bits --------------------------------------------------------------
	{
		const auto with_bits = [&](std::uint64_t structures) {
			return open(relayout(good, [](entries&) {}, [&](strata::format::footer& f) { f.structures = structures; }));
		};
		const std::uint64_t built = bit::inverted | bit::norms | bit::keys | bit::stored;
		// A built structure's bit cleared: its sections present without it, or norms without
		// inverted: damage.
		for (std::uint64_t b : {bit::inverted, bit::norms, bit::keys, bit::stored}) { CHECK(with_bits(built & ~b) == outcome::corrupt); }
		// index_sort without values: damage.
		CHECK(with_bits(built | bit::index_sort) == outcome::corrupt);
		// A sub-structure without its owner's bit: damage, whatever the binary reads.
		CHECK(with_bits(bit::norms | bit::keys | bit::stored) == outcome::corrupt);
		CHECK(with_bits((built & ~bit::stored) | bit::stored_spans) == outcome::corrupt);
		CHECK(with_bits(bit::keys | bit::stored | bit::postings_runs) == outcome::corrupt);  // bit 14 without inverted (F13-R32)
		CHECK(with_bits((built & ~bit::keys) | bit::version_clocks) == outcome::corrupt);  // bit 16 without keys (durability D12)
		// Every other bit, defined, reserved, or unlisted: one this binary lacks, so
		// unsupported (F4; F13-R30), and never read past.
		for (unsigned b = 0; b < 64; ++b) {  // bits 32 to 63 too (F13-R40)
			const std::uint64_t v = std::uint64_t(1) << b;
			if (built & v || v == bit::index_sort) { continue; }
			const auto got = with_bits(built | v);
			if (got != outcome::unsupported) { std::fprintf(stderr, "bit %u: %s\n", b, name(got)); }
			CHECK(got == outcome::unsupported);
		}
		CHECK(with_bits(built | bit::values | bit::index_sort) == outcome::unsupported);
		// What a later format-13 release writes, its sections placed by F3's rule and its table
		// changed where its bit allows: unsupported, since the bits are read before the table
		// (F23 rules 3 and 5; F13-R35).
		const std::string opaque(64, '\x5a');
		CHECK(open(later_release(good, std::pair{section::pair_summaries, opaque}, section::keys, built | bit::pair_summaries)) == outcome::unsupported);
		CHECK(open(later_release(good, std::pair{section::key_hash, opaque}, section::stored, built | bit::key_hash)) == outcome::unsupported);
		CHECK(open(later_release(good, std::pair{section::points, opaque}, section::docids, built | bit::points)) == outcome::unsupported);
		CHECK(open(later_release(good, std::pair{section(23), opaque}, std::nullopt, built | (1u << 17))) == outcome::unsupported);  // an unlisted bit
		CHECK(open(later_release(good, std::nullopt, std::nullopt, built | bit::compact, [](const entries& es) {
			std::string t;  // offsets implied, lengths as LEB128: a form this binary cannot parse
			strata::format::put<std::uint32_t>(t, std::uint32_t(es.size()));
			for (const auto& e : es) {
				strata::format::put<std::uint16_t>(t, std::uint16_t(e.kind));
				strata::format::put_leb(t, e.length);
			}
			return t;
		})) == outcome::unsupported);
		// The same kinds with their bits clear stay damage.
		CHECK(open(later_release(good, std::pair{section::pair_summaries, opaque}, section::keys, built)) == outcome::corrupt);
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

	// --- F20: the manifest's structures and versions (7c-6) -----------------------------------
	{
		enum class verdict { opens, corrupt, unsupported, other };
		auto open_index = [](kronuz::journal::IO& io, const strata::index::options& o) {
			try {
				auto ix = strata::index::open(io, o);
				return verdict::opens;
			} catch (const strata::unsupported_index&) {
				return verdict::unsupported;
			} catch (const strata::corrupt_index&) {
				return verdict::corrupt;
			} catch (const strata::corrupt_segment&) {
				return verdict::corrupt;
			} catch (...) {
				return verdict::other;
			}
		};
		strata::index::options o;
		o.key_prefix = "Q";
		// A committed index; returns its head manifest's generation.
		auto make = [&](strata::faultfs::io& io) {
			auto ix = strata::index::create(io, o, {});
			CHECK(apply(ix, upsert("Q1", "w")) == strata::verdict::accepted);
			ix.commit();
			const auto m = ix.manifest();
			CHECK(m.configured == (bit::inverted | bit::norms | bit::keys | bit::stored) && m.mask == m.configured);
			CHECK(m.min_version == strata::format::pre_freeze && m.max_version == strata::format::pre_freeze);
			return m.generation;
		};
		// Rewrites the head manifest with `edit` applied.
		auto rewrite = [](strata::faultfs::filesystem& fs, strata::faultfs::io& io, std::uint64_t gen, auto edit) {
			const std::string name = strata::index_detail::manifest_name(gen);
			auto m = strata::files::decode_manifest(std::string(fs.view(name)));
			edit(m);
			const std::string bytes = strata::files::encode(m);
			auto f = io.open_existing(name);
			f->truncate(0);
			f->write_at(0, bytes);
		};
		const auto with = [&](auto edit) {
			strata::faultfs::filesystem fs;
			strata::faultfs::io io(fs);
			const auto gen = make(io);
			rewrite(fs, io, gen, edit);
			return open_index(io, o);
		};
		CHECK(with([](strata::files::manifest&) {}) == verdict::opens);
		// A mask bit this binary cannot read: unsupported, before any segment opens.
		CHECK(with([](strata::files::manifest& m) { m.mask |= bit::values; }) == verdict::unsupported);
		CHECK(with([](strata::files::manifest& m) { m.mask |= bit::compact; }) == verdict::unsupported);
		CHECK(with([](strata::files::manifest& m) { m.mask |= std::uint64_t(1) << 40; }) == verdict::unsupported);  // bits 32 to 63 (F13-R40)
		CHECK(with([](strata::files::manifest& m) { m.mask |= std::uint64_t(1) << 63; }) == verdict::unsupported);
		// A later release's field (a values generation) under a mask bit this binary lacks: the
		// mask is classified first, so unsupported; the field alone is damage (F13-R42).
		CHECK(with([](strata::files::manifest& m) {
			m.mask |= std::uint64_t(1) << 20;
			m.segments.back().values_generation = 1;
		}) == verdict::unsupported);
		CHECK(with([](strata::files::manifest& m) { m.segments.back().values_generation = 1; }) == verdict::corrupt);
		CHECK(with([](strata::files::manifest& m) {
			m.mask |= std::uint64_t(1) << 20;
			m.slots = {1, 10, 0};
		}) == verdict::unsupported);
		// A highest segment version above this binary's: unsupported.
		CHECK(with([](strata::files::manifest& m) { m.max_version = strata::format::version + 1; }) == verdict::unsupported);
		// An index an earlier pre-freeze build wrote, segments at 13: unsupported (CR11).
		CHECK(with([](strata::files::manifest& m) { m.min_version = m.max_version = 13; }) == verdict::unsupported);
		// A segment that sets a bit outside its manifest's mask: damage.
		CHECK(with([](strata::files::manifest& m) { m.mask = m.configured = bit::inverted | bit::norms | bit::keys; }) == verdict::corrupt);
		// Configured bits no flush could write: damage at open (CR5). Clearing stored is a
		// valid configuration.
		CHECK(with([](strata::files::manifest& m) { m.configured = bit::inverted | bit::norms | bit::stored; }) == verdict::corrupt);  // keys off, prefix Q
		CHECK(with([](strata::files::manifest& m) { m.configured = bit::norms | bit::keys | bit::stored; }) == verdict::corrupt);    // norms without inverted
		CHECK(with([](strata::files::manifest& m) { m.configured = bit::inverted | bit::norms | bit::keys; }) == verdict::opens);
		// A version-1 manifest: unsupported.
		{
			strata::faultfs::filesystem fs;
			strata::faultfs::io io(fs);
			const auto gen = make(io);
			const std::string v1 = read_file(std::string(STRATA_FIXTURES) + "/format12/manifest.bin");
			auto f = io.open_existing(strata::index_detail::manifest_name(gen));
			f->truncate(0);
			f->write_at(0, v1);
			CHECK(open_index(io, o) == verdict::unsupported);
		}
		// install refuses a checkpoint whose mask holds a bit this binary cannot read.
		{
			strata::faultfs::filesystem fs_a, fs_b;
			strata::faultfs::io io_a(fs_a), io_b(fs_b);
			std::uint64_t gen = 0;
			{
				auto a = strata::index::create(io_a, o, {});
				CHECK(apply(a, upsert("Q1", "w")) == strata::verdict::accepted);
				gen = a.checkpoint();
			}
			rewrite(fs_a, io_a, gen, [](strata::files::manifest& m) {
				m.mask |= bit::points;
				m.segments.back().values_generation = 1;  // a later field under it: still unsupported (F13-R42)
			});
			auto b = strata::index::create(io_b, o, {});
			bool unsupported = false;
			try {
				b.install(io_a, gen);
			} catch (const strata::unsupported_index&) {
				unsupported = true;
			}
			CHECK(unsupported);
		}
		// The segment identity (F6, F13-R41): unique across replicas, the producing term first,
		// kept by install, and checked against the manifest's entry at open (I13).
		{
			auto header_id = [](const strata::index& ix, std::size_t i) {
				return std::string(ix.segments()[i].seg->core().section(section::header).bytes.substr(strata::format::identity_offset, 16));
			};
			auto entry_id = [](const strata::index& ix, std::size_t i) {
				const auto& id = ix.manifest().segments[i].identity;
				return std::string(reinterpret_cast<const char*>(id.data()), 16);
			};
			strata::faultfs::filesystem fs_a, fs_b;
			strata::faultfs::io io_a(fs_a), io_b(fs_b);
			auto a = strata::index::create(io_a, o, {});
			a.set_producer_term(7);
			CHECK(apply(a, upsert("Q1", "w")) == strata::verdict::accepted);
			const std::uint64_t gen = a.checkpoint();
			const std::string first = header_id(a, 0);
			std::uint64_t term = 0;
			std::memcpy(&term, first.data(), 8);
			CHECK(term == 7 && entry_id(a, 0) == first);
			auto b = strata::index::create(io_b, o, {});
			b.install(io_a, gen);
			CHECK(b.segments().size() == 1 && header_id(b, 0) == first && entry_id(b, 0) == first);  // kept by the copy
			// The probe's case: each replica commits one upsert of its own in the same term.
			b.set_producer_term(7);
			CHECK(apply(a, upsert("Q2", "a")) == strata::verdict::accepted);
			CHECK(apply(b, upsert("Q2", "b")) == strata::verdict::accepted);
			a.commit();
			b.commit();
			CHECK(a.segments().size() == 2 && b.segments().size() == 2);
			CHECK(header_id(a, 1) != header_id(b, 1) && header_id(a, 1) != first && header_id(b, 1) != first);
			CHECK(entry_id(a, 1) == header_id(a, 1) && entry_id(b, 1) == header_id(b, 1));
		}
		{
			// A manifest entry naming another identity than its file's: damage, before any read.
			CHECK(with([](strata::files::manifest& m) { m.segments.back().identity[15] ^= 1; }) == verdict::corrupt);
			// A segment attribute this binary lacks (F13-R43): a required one refuses the index, a
			// cache opens and leaves at the next commit.
			CHECK(with([](strata::files::manifest& m) { m.segments.back().attributes = {{1, "later"}}; }) == verdict::unsupported);
			CHECK(with([](strata::files::manifest& m) { m.segments.back().attributes = {{2, "bounds"}}; }) == verdict::opens);
		}
		// install refuses a checkpoint whose configured bits no flush could write (CR5).
		{
			strata::faultfs::filesystem fs_a, fs_b;
			strata::faultfs::io io_a(fs_a), io_b(fs_b);
			std::uint64_t gen = 0;
			{
				auto a = strata::index::create(io_a, o, {});
				CHECK(apply(a, upsert("Q1", "w")) == strata::verdict::accepted);
				gen = a.checkpoint();
			}
			rewrite(fs_a, io_a, gen, [](strata::files::manifest& m) { m.configured &= ~bit::keys; });
			auto b = strata::index::create(io_b, o, {});
			bool corrupt = false;
			try {
				b.install(io_a, gen);
			} catch (const strata::corrupt_index&) {
				corrupt = true;
			}
			CHECK(corrupt);
		}
	}

	// --- At the index: a manifest-named unsupported segment is damage ------------------------
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
			auto refuses = [](std::uint64_t structures, bool terms, bool record, std::uint64_t version = 0, std::string key = {}) {
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
			// A merge's output takes the configuration too (F4; CR6, M4).
			CHECK(apply(ix, add) == strata::verdict::accepted);
			ix.commit();
			ix.force_merge();
			CHECK(ix.segments().size() == 1);
			for (const auto& sv : ix.segments()) { CHECK(!sv.seg->holds<strata::keys_plugin>() && sv.seg->core().entry(section::keys) == nullptr); }
			CHECK(ix.digest() != before);
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
		// check() refuses keys sections that lookups would misread (F13-R37), each resealed as
		// fuzz inputs are: two keys naming one ordinal, a range other than the first and last
		// key, and a filter that misses held keys.
		{
			strata::segment_writer w;
			for (std::uint32_t d = 0; d < 10; ++d) {
				std::vector<strata::posting_input> in{{"w", 1, {}}};
				w.add(d + 1, in, 1, {}, 0, "Q" + std::to_string(d));
			}
			const std::string good10 = w.flush({});
			auto refused = [](const std::string& file) {
				try {
					strata::check_report rr;
					strata::check_segment(strata::segment(file), nullptr, rr, "Q");
				} catch (const strata::corrupt_segment&) {
					return true;
				}
				return false;
			};
			CHECK(!refused(good10));
			std::vector<std::string> names;
			for (std::uint32_t d = 0; d < 10; ++d) { names.push_back("Q" + std::to_string(d)); }
			std::vector<std::pair<std::string_view, strata::ordinal_t>> e;
			for (std::uint32_t d = 0; d < 10; ++d) { e.push_back({names[d], d == 1 ? 0 : d}); }
			const auto twice = strata::keys::encode(e);
			CHECK(refused(with_section(with_section(good10, section::keys, twice.keys), section::key_index, twice.index)));
			// key_index rebuilt with another maximum, or a filter over other keys.
			const std::string index = std::string(strata::segment(good10).core().section(section::key_index).bytes);
			auto reindex = [&](const std::string* max, bool other_filter) {
				const char* q = index.data();
				const char* end = q + index.size();
				const std::uint64_t count = strata::format::get_leb(&q, end);
				auto field = [&] {
					const std::uint64_t n = strata::format::get_leb(&q, end);
					std::string out(q, std::size_t(n));
					q += n;
					return out;
				};
				const std::string mn = field(), mx = field(), filter = field(), rest(q, end);
				std::string f = filter;
				if (other_filter) {
					std::vector<std::uint64_t> h;
					for (int i = 0; i < 64; ++i) { h.push_back(xxh64::hash("X" + std::to_string(i))); }
					f = fusefilter::filter8::build(h).serialize();
				}
				std::string out;
				strata::format::put_leb(out, count);
				for (const std::string* v : std::initializer_list<const std::string*>{&mn, max ? max : &mx, &f}) {
					strata::format::put_leb(out, v->size());
					out += *v;
				}
				return out + rest;
			};
			const std::string q8 = "Q8", q99 = "Q99";
			CHECK(refused(with_section(good10, section::key_index, reindex(&q8, false))));   // max below the last key
			CHECK(refused(with_section(good10, section::key_index, reindex(&q99, false))));  // max above it
			CHECK(refused(with_section(good10, section::key_index, reindex(nullptr, true))));
			// MR1's M1: one keys byte changed so a key repeats the one before, every key still in
			// range and filter and every ordinal distinct; only the cursor's order test refuses it.
			// MR2: a merge of it is damage too, never std::invalid_argument.
			const std::string kb(strata::segment(good10).core().section(section::keys).bytes);
			std::string repeat;
			for (std::size_t i = 0; i < kb.size() && repeat.empty(); ++i) {
				for (int x : {1, 2, 3}) {
					std::string k = kb;
					k[i] = char(k[i] ^ x);
					const std::string file = with_section(good10, section::keys, k);
					try {
						strata::segment sg(file);
						std::vector<std::string> got;
						std::vector<strata::ordinal_t> ords;
						sg.get<strata::keys_plugin>().scan("", [&](std::string_view key, strata::ordinal_t o) {
							got.push_back(std::string(key));
							ords.push_back(o);
							return true;
						});
						std::sort(ords.begin(), ords.end());
						const bool known = std::all_of(got.begin(), got.end(), [&](const std::string& g) { return std::find(names.begin(), names.end(), g) != names.end(); });
						if (got.size() == 10 && known && std::is_sorted(got.begin(), got.end()) && std::adjacent_find(got.begin(), got.end()) != got.end() &&
							std::adjacent_find(ords.begin(), ords.end()) == ords.end()) {
							repeat = file;
							break;
						}
					} catch (const std::exception&) {
					}
				}
			}
			auto merge_refused = [](const std::string& file) {
				strata::segment sg(file);
				const std::vector<strata::merge_source> src{{&sg, nullptr}};
				std::string out;
				strata::string_output so(out);
				strata::string_spill spill;
				strata::merge_options mo;
				mo.key_prefix = "Q";
				try {
					(void)strata::merge_segments(src, {}, so, spill, mo);
				} catch (const strata::corrupt_segment&) {
					return true;
				}
				return false;
			};
			CHECK(!repeat.empty() && refused(repeat) && merge_refused(repeat));
			// CR3: a merge refuses two keys naming one ordinal as damage, never keeping the last.
			CHECK(merge_refused(with_section(with_section(good10, section::keys, twice.keys), section::key_index, twice.index)));
			// MR1's M3: nine keys of ten documents, key_index's count raised to 10.
			{
				strata::segment_writer w9;
				for (std::uint32_t d = 0; d < 10; ++d) {
					std::vector<strata::posting_input> in{{"w", 1, {}}};
					w9.add(d + 1, in, 1, {}, 0, d == 9 ? std::string() : "Q" + std::to_string(d));
				}
				const std::string good9 = w9.flush({});
				CHECK(!refused(good9));
				std::string ix9(strata::segment(good9).core().section(section::key_index).bytes);
				CHECK(!ix9.empty() && ix9[0] == 9);
				ix9[0] = 10;  // one LEB byte either way
				CHECK(refused(with_section(good9, section::key_index, ix9)));
			}
		}
		// The merge's backstop refuses a key-prefixed term too, never drops it (F13-R10; CR6, M33).
		{
			strata::segment_writer w;
			std::vector<strata::posting_input> in{{"Qx", 1, {}}};
			w.add(1, in, 1);
			const std::string bytes = w.flush({});
			strata::segment seg(bytes);
			const std::vector<strata::merge_source> src{{&seg, nullptr}};
			std::string out;
			strata::string_output so(out);
			strata::string_spill spill;
			strata::merge_options mo;
			mo.key_prefix = "Q";
			bool threw = false;
			try {
				(void)strata::merge_segments(src, {}, so, spill, mo);
			} catch (const std::invalid_argument&) {
				threw = true;
			}
			CHECK(threw);
		}
		// A length-0 document keeps its length record through a flush (F21; CR6, M5).
		{
			auto empty = upsert("Qempty", "w");
			empty.doc.terms.clear();
			empty.doc.length = 0;
			CHECK(apply(ix, empty) == strata::verdict::accepted);
			const std::uint64_t buffered = ix.digest();
			ix.commit();
			CHECK(ix.digest() == buffered);
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
	// A keyless index: no prefix, so no routing and no term refused for its bytes (F13-R33). A
	// term that starts like a key is an ordinary term; a document that carries a key is
	// invalid, never stored with its key dropped (F13-R39).
	{
		strata::faultfs::filesystem fs;
		strata::faultfs::io io(fs);
		strata::index::options o;
		auto ix = strata::index::create(io, o, {});
		strata::operation op;
		op.type = strata::operation::add;
		op.doc.terms.push_back({"Qa", 1, {1}});
		op.doc.terms.push_back({"w", 1, {2}});
		op.doc.length = 2;
		strata::operation keyed = op;
		keyed.doc.key = "Qa";
		CHECK(apply(ix, keyed) == strata::verdict::invalid);
		keyed.type = strata::operation::replace_docid;
		keyed.docid = 1;
		CHECK(apply(ix, keyed) == strata::verdict::invalid);
		CHECK(apply(ix, op) == strata::verdict::accepted);
		ix.commit();
		CHECK(matches(ix, "Qa", {}).size() == 1 && matches(ix, "w", {}).size() == 1);
		CHECK(!ix.find_key("Qa"));
		CHECK(ix.check().keys == 0);
	}

	// inverted's statistics equal its documents' (F7; CR6, M28): total, minimum, and maximum
	// length, and most positions, at the head of term_index.
	{
		strata::segment_writer w;
		const std::uint32_t lengths[] = {3, 7, 5};
		const std::vector<std::uint32_t> pos[] = {{1}, {1, 2, 3, 4}, {2, 5}};
		for (std::uint32_t d = 0; d < 3; ++d) {
			std::vector<strata::posting_input> in{{"w", std::uint32_t(pos[d].size()), pos[d]}};
			w.add(d + 1, in, lengths[d], {});
		}
		const std::string bytes = w.flush({});
		strata::segment s(bytes);
		const auto& inv = s.get<strata::inverted_plugin>();
		CHECK(inv.total_length() == 15 && inv.max_positions() == 4);
		const std::string_view head = s.core().section(section::term_index).bytes;
		CHECK(strata::format::get<std::uint64_t>(head.data()) == 15);
		CHECK(strata::format::get<std::uint32_t>(head.data() + 8) == 3 && strata::format::get<std::uint32_t>(head.data() + 12) == 7);
		CHECK(strata::format::get<std::uint32_t>(head.data() + 16) == 4);
	}

	// The buffer and the disk estimate count a document's key as they count a term (CR12).
	{
		strata::document d;
		d.terms.push_back({"w", 1, {}});
		const std::uint64_t bytes = d.part_bytes(), disk = d.disk_estimate();
		d.key = "Q" + std::string(999, 'k');
		CHECK(d.part_bytes() == bytes + 1000 && d.disk_estimate() >= disk + 1000);
	}

	// CR8's fourth item: a segment without keys or stored collects nothing for them, about
	// 16 bytes a document less held, and still refuses a version without a key.
	{
		auto held = [](std::uint64_t structures) {
			strata::flush_charge charge(nullptr);
			strata::segment_writer w;
			w.structures = structures;
			w.memory = &charge;
			for (std::uint32_t d = 0; d < 20000; ++d) {
				std::vector<strata::posting_input> in{{"w", 1, {}}};
				w.add(d + 1, in, 1);
			}
			return charge.held();
		};
		const std::uint64_t all = held(bit::inverted | bit::norms | bit::keys | bit::stored), bare = held(bit::inverted | bit::norms);
		CHECK(all >= bare + 16 * 20000);
		strata::segment_writer w;
		w.structures = bit::inverted | bit::norms;
		std::vector<strata::posting_input> in{{"w", 1, {}}};
		bool refused = false;
		try {
			w.add(1, in, 1, {}, 5);
		} catch (const std::invalid_argument&) {
			refused = true;
		}
		CHECK(refused);
	}
	std::printf("format13: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
