// A media fault in the terms section (a bit flipped, checksums not repaired) of a segment with two
// large own dictionaries, a few pooled fields and more than one checksum region: scan_terms, a
// term_cursor walk and a merge each throw corrupt_segment for every flipped bit, whichever
// dictionary or the pool it lands in; none reads the damage as data or writes it into a new segment.
#include "prospect/check.hh"
#include "prospect/merge.hh"
#include "prospect/reader.hh"
#include "prospect/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using terms_t = std::map<std::uint32_t, std::vector<std::string>>;

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

static terms_t base_a() {
	terms_t m;
	for (int i = 0; i < 3000; ++i) { char b[16]; std::snprintf(b, sizeof b, "a%05d", i); m[0].push_back(b); }
	for (int i = 0; i < 2500; ++i) { char b[16]; std::snprintf(b, sizeof b, "b%05d", i * 7); m[5].push_back(b); }
	for (std::uint32_t f : {1u, 2u, 3u, 239u, 240u, 70000u}) {
		m[f].push_back("");
		for (int i = 0; i < 5; ++i) { m[f].push_back("p" + std::to_string(f) + "_" + std::to_string(i)); }
		std::sort(m[f].begin(), m[f].end());
	}
	return m;
}

static std::string build(const terms_t& t, std::uint64_t docid0) {
	prospect::segment_writer w;
	std::uint64_t docid = docid0;
	for (const auto& [f, ts] : t) {
		for (const auto& term : ts) {
			const prospect::posting_input in{term, 1, {}, f};
			prospect::detail::writer_access::add_texts(w, ++docid, std::span<const prospect::posting_input>(&in, 1), {}, 1, {}, 0, {});
		}
	}
	return w.flush({});
}

using flat_t = std::vector<std::pair<std::uint32_t, std::string>>;

static flat_t scan_all(const prospect::segment& s) {
	flat_t out;
	s.get<prospect::inverted_plugin>().scan_terms([&](std::uint32_t f, std::string_view t, const prospect::format::term_info&) {
		out.emplace_back(f, std::string(t));
		return true;
	});
	return out;
}

// Whether `op` throws corrupt_segment; any other outcome (it returns, or throws something else) is not.
template <class F>
static bool corrupt(F&& op) {
	try {
		op();
	} catch (const strata::corrupt_segment&) {
		return true;
	} catch (...) {
		std::fprintf(stderr, "a non-corrupt exception escaped\n");
	}
	return false;
}

int main() {
	const terms_t a = base_a();
	terms_t b;
	b[0] = {"a00000", "a00003"};
	b[9] = {"x", "y"};
	const std::string fa = build(a, 1000), fb = build(b, 900000);
	strata::format::section_entry sec{};
	for (const auto& e : strata::format::decode_toc(fa, strata::format::decode_footer(fa))) {
		if (e.kind == strata::format::section::terms) { sec = e; }
	}
	CHECK(sec.length > 4 * 4096);  // several checksum regions
	{  // undamaged: the same operations answer
		prospect::segment x(fa), y(fb);
		CHECK(scan_all(x).size() > 5500);
		const std::vector<prospect::merge_source> src{{&x, nullptr}, {&y, nullptr}};
		std::string out;
		strata::string_output so(out);
		strata::string_spill spill;
		(void)prospect::merge_segments(src, {}, so, spill, {});
		prospect::segment m(out);
		CHECK(scan_all(m).size() == scan_all(x).size() + 2);
	}
	unsigned flips = 0, scan_bad = 0, cursor_bad = 0, merge_bad = 0;
	for (std::size_t p = 0; p < sec.length; p += 257) {
		std::string file = fa;
		file[sec.offset + p] ^= char(1 << (p % 8));
		++flips;
		scan_bad += !corrupt([&] {
			prospect::segment s(file);
			(void)scan_all(s);
		});
		cursor_bad += !corrupt([&] {
			prospect::segment s(file);
			const auto& inv = s.get<prospect::inverted_plugin>();
			prospect::inverted_plugin::reader::term_cursor c(inv);
			while (c.next()) {}
		});
		merge_bad += !corrupt([&] {
			prospect::segment x(file), y(fb);
			const std::vector<prospect::merge_source> src{{&x, nullptr}, {&y, nullptr}};
			std::string out;
			strata::string_output so(out);
			strata::string_spill spill;
			(void)prospect::merge_segments(src, {}, so, spill, {});
		});
	}
	std::printf("%u flips: scan_terms %u, term_cursor %u, merge %u not refused\n", flips, scan_bad, cursor_bad, merge_bad);
	CHECK(flips > 100);
	CHECK(scan_bad == 0 && cursor_bad == 0 && merge_bad == 0);
	if (failures) { return 1; }
	std::puts("fielddict_media_test: ok");
	return 0;
}
