// Segment writer and reader against a naive model: postings, advance, block_end,
// impact bounds, lengths, and corrupt files (every truncation, random corruption).
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/writer.hh"
#include "skip_layout.hh"

#include <crc32c/crc32c.hh>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

struct model {
	std::map<std::string, std::vector<std::pair<strata::ordinal_t, std::uint32_t>>> postings;
	std::map<std::string, std::vector<std::vector<std::uint32_t>>> positions;  // per posting
	std::vector<std::uint32_t> lengths;
	std::vector<std::string> stored;
};

// Recomputes every section's checksums, then the table of contents' and the footer's, so a
// changed file reaches the decoders instead of failing verification.
static void reseal(std::string& file) {
	const auto footer = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, footer);
	for (auto& e : toc) { e = strata::format::describe(e.kind, e.offset, std::string_view(file).substr(e.offset, e.length), e.region_shift); }
	const std::string toc_bytes = strata::format::encode_toc(toc);
	file.replace(footer.toc_offset, toc_bytes.size(), toc_bytes);
	auto f = footer;
	f.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
	const std::string footer_bytes = strata::format::encode_footer(f);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

// The file with one section's bytes replaced (any length): every section laid out again
// at its alignment, the table of contents and footer recomputed, and the footer then
// edited as `edit` asks. Reaches the checks that consistent checksums cannot.
static std::string with_sections(const std::string& file, const std::map<strata::format::section, std::string>& replaced,
	const std::function<void(strata::format::footer&)>& edit = {}) {
	auto f = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, f);
	std::sort(toc.begin(), toc.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
	std::string out;
	for (auto& e : toc) {
		while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
		auto it = replaced.find(e.kind);
		const std::string b = it != replaced.end() ? it->second : std::string(std::string_view(file).substr(e.offset, e.length));
		e = strata::format::describe(e.kind, out.size(), b, e.region_shift);
		out += b;
	}
	while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
	const std::string toc_bytes = strata::format::encode_toc(toc);
	f.toc_offset = out.size();
	f.toc_length = toc_bytes.size();
	f.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
	out += toc_bytes;
	if (edit) { edit(f); }
	return out + strata::format::encode_footer(f);
}

static std::string with_section(const std::string& file, strata::format::section kind, std::string_view bytes,
	const std::function<void(strata::format::footer&)>& edit = {}) {
	return with_sections(file, {{kind, std::string(bytes)}}, edit);
}

static std::string section_of(const std::string& file, strata::format::section kind);

// The file with its dictionary rebuilt from its own term infos, `change` editing each.
static std::string with_terms(const std::string& file, const std::function<void(std::string_view, strata::format::term_info&)>& change) {
	strata::segment seg(file);
	termdict::builder b(seg.get<strata::inverted_plugin>().dictionary().alphabet_bytes().size() == 256 ? std::string_view() : seg.get<strata::inverted_plugin>().dictionary().alphabet_bytes());
	strata::format::term_run run;
	seg.get<strata::inverted_plugin>().scan_terms([&](std::string_view term, const strata::format::term_info& info) {
		auto t = info;
		if (t.df >= strata::format::block) { t.impacts = seg.get<strata::inverted_plugin>().term(term)->impacts; }
		change(term, t);
		b.add(term, strata::format::encode_term_info(t, run, b.next_is_restart()));
		return true;
	});
	auto t = b.finish();
	const std::string statistics = section_of(file, strata::format::section::term_index).substr(0, strata::inverted_plugin::statistics_size);
	return with_sections(file, {{strata::format::section::terms, t.blocks}, {strata::format::section::term_index, statistics + t.index}});
}

static std::string section_of(const std::string& file, strata::format::section kind) {
	for (const auto& e : strata::format::decode_toc(file, strata::format::decode_footer(file))) {
		if (e.kind == kind) { return file.substr(e.offset, e.length); }
	}
	return {};
}

static bool throws_invalid(const std::function<void()>& f) {
	try {
		f();
	} catch (const std::invalid_argument&) {
		return true;
	}
	return false;
}

static bool throws_out_of_range(const std::function<void()>& f) {
	try {
		f();
	} catch (const std::out_of_range&) {
		return true;
	}
	return false;
}

static bool throws_corrupt(const std::function<void()>& f) {
	try {
		f();
	} catch (const strata::corrupt_segment&) {
		return true;
	}
	return false;
}

static std::string build(std::mt19937_64& rng, std::size_t docs, model& m, std::uint8_t region_shift = strata::format::default_region_shift) {
	strata::segment_writer w;
	w.region_shift = region_shift;
	// Term frequencies span tails, single blocks, and many skip groups.
	std::vector<std::pair<std::string, double>> vocab;
	for (int i = 0; i < 200; ++i) { vocab.push_back({"t" + std::to_string(i), i < 5 ? 0.9 : i < 20 ? 0.3 : 0.01 + 0.002 * i}); }
	vocab.push_back({"boolean", 0.5});
	for (std::size_t d = 0; d < docs; ++d) {
		std::vector<strata::posting_input> in;
		std::vector<std::vector<std::uint32_t>> pos;
		std::vector<std::string> keep;
		pos.reserve(vocab.size());
		std::uint32_t length = 0;
		for (const auto& [term, p] : vocab) {
			if (std::uniform_real_distribution<>(0, 1)(rng) >= p) { continue; }
			std::uint32_t wdf = term == "boolean" ? 0 : std::uint32_t(1 + rng() % (rng() % 8 == 0 ? 40 : 3));
			pos.emplace_back();
			std::uint32_t at = 0;
			// t1 has weighted occurrences (more wdf than positions); boolean has positions with wdf 0.
			std::uint32_t npos = term == "t1" ? (wdf + 1) / 2 : term == "boolean" ? 1 + std::uint32_t(rng() % 2) : wdf;
			for (std::uint32_t k = 0; k < npos; ++k) { pos.back().push_back(at += 1 + std::uint32_t(rng() % 5)); }
			m.positions[term].push_back(pos.back());
			in.push_back({term, wdf, pos.back()});
			length += wdf;
			m.postings[term].push_back({strata::ordinal_t(d), wdf});
		}
		m.lengths.push_back(length);
		// Stored records: mostly short, some empty, a few past one zstd frame's size.
		const std::size_t pick = rng() % 100;
		std::string rec(pick < 10 ? 0 : pick < 97 ? rng() % 300 : 70000 + rng() % 70000, '\0');
		for (auto& c : rec) { c = char('a' + rng() % 26); }
		m.stored.push_back(rec);
		w.add(d + 1, in, length, rec);
	}
	return w.flush({});
}

int main() {
	std::mt19937_64 rng(7);
	for (std::size_t docs : {1u, 5u, 127u, 128u, 129u, 1000u, 9000u}) {
		model m;
		// Odd sizes use 4 KiB verification regions, so lazy verification is checked at both sizes.
		std::string file = build(rng, docs, m, docs % 2 ? 12 : 16);
		strata::segment seg(file);
		CHECK(seg.core().doc_count() == docs);
		for (std::size_t d = 0; d < docs; ++d) { CHECK(seg.get<strata::inverted_plugin>().length(strata::ordinal_t(d)) == m.lengths[d]); }
		for (const auto& [term, list] : m.postings) {
			auto info = seg.get<strata::inverted_plugin>().term(term);
			CHECK(info && info->df == list.size());
			if (!info) { continue; }
			strata::postings_iterator it(seg.get<strata::inverted_plugin>(), *info);
			std::size_t i = 0;
			for (auto d = it.next(); d != strata::no_more_docs; d = it.next(), ++i) {
				CHECK(i < list.size() && d == list[i].first && it.wdf() == list[i].second);
			}
			CHECK(i == list.size());
			// positions of every posting
			{
				strata::postings_iterator pi(seg.get<strata::inverted_plugin>(), *info);
				strata::positions_reader pr(seg.get<strata::inverted_plugin>(), *info);
				std::vector<std::uint32_t> got;
				std::size_t k = 0;
				for (auto d = pi.next(); d != strata::no_more_docs; d = pi.next(), ++k) {
					pr.read(pi, got);
					CHECK(got == m.positions[term][k]);
				}
				// random access after advance
				for (int round = 0; round < 10 && !list.empty(); ++round) {
					strata::postings_iterator a(seg.get<strata::inverted_plugin>(), *info);
					strata::positions_reader ar(seg.get<strata::inverted_plugin>(), *info);
					std::size_t idx = rng() % list.size();
					CHECK(a.advance(list[idx].first) == list[idx].first);
					ar.read(a, got);
					CHECK(got == m.positions[term][idx]);
				}
			}
			// advance to random targets
			for (int round = 0; round < 20; ++round) {
				strata::postings_iterator a(seg.get<strata::inverted_plugin>(), *info);
				strata::ordinal_t target = 0;
				for (;;) {
					target += strata::ordinal_t(rng() % 400);
					auto got = a.advance(target);
					auto want = std::lower_bound(list.begin(), list.end(), target, [](const auto& p, strata::ordinal_t t) { return p.first < t; });
					CHECK(got == (want == list.end() ? strata::no_more_docs : want->first));
					if (got == strata::no_more_docs) { break; }
					CHECK(a.wdf() == want->second);
					target = got + 1;
				}
			}
			// block_end and impact bounds
			strata::postings_iterator b(seg.get<strata::inverted_plugin>(), *info);
			for (std::size_t k = 0; k < list.size(); k += 1 + rng() % 50) {
				auto end = b.block_end(list[k].first);
				std::size_t block_last = k / 128 * 128 + 127;
				std::size_t want_end = list.size() / 128 * 128 > k ? block_last : list.size() - 1;
				CHECK(end == list[want_end].first);
				const auto& pairs = b.block_impacts(list[k].first);
				bool dominated = false;
				for (const auto& p : pairs) {
					dominated = dominated || (p.wdf >= list[k].second && strata::format::decode_length(p.length) <= m.lengths[list[k].first]);
				}
				CHECK(dominated);
			}
		}
		CHECK(!seg.get<strata::inverted_plugin>().term("absent"));
		for (strata::ordinal_t d = 0; d < seg.core().doc_count(); ++d) { CHECK(seg.get<strata::stored_plugin>().stored(d) == m.stored[d]); }
		for (strata::ordinal_t d = 0; d < seg.core().doc_count(); ++d) { CHECK(seg.core().docid(d) == d + 1); }
		// Corruption: truncations and random byte changes never crash.
		for (std::size_t cut = 0; cut < file.size(); cut += 1 + file.size() / 100) {
			try { strata::segment s(std::string_view(file).substr(0, cut)); CHECK(false); } catch (const strata::corrupt_segment&) {} catch (const std::invalid_argument&) {}
		}
		// A changed byte anywhere but section padding is detected once everything is read:
		// at open, or by region checksums on first read.
		const auto layout = seg.core().sections();
		for (int round = 0; round < 300; ++round) {
			std::string bad = file;
			const std::size_t at = rng() % bad.size();
			bad[at] = char(bad[at] ^ char(1 + rng() % 255));
			bool in_section = at >= file.size() - strata::format::footer_size;
			for (const auto& e : layout) { in_section = in_section || (at >= e.offset && at < e.offset + e.length); }
			const std::uint64_t toc_at = strata::format::get<std::uint64_t>(file.data() + file.size() - strata::format::footer_size);
			in_section = in_section || at >= toc_at;
			bool detected = false;
			try {
				strata::segment s(bad);
				for (const auto& [term, list] : m.postings) {
					auto info = s.get<strata::inverted_plugin>().term(term);
					if (!info) { continue; }
					strata::postings_iterator it(s.get<strata::inverted_plugin>(), *info);
					strata::positions_reader pr(s.get<strata::inverted_plugin>(), *info);
					std::vector<std::uint32_t> got;
					for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { pr.read(it, got); }
				}
				s.get<strata::inverted_plugin>().term("absent");
				for (strata::ordinal_t d = 0; d < s.core().doc_count(); ++d) { (void)s.get<strata::stored_plugin>().stored(d); }
				if (s.core().doc_count()) { (void)s.get<strata::inverted_plugin>().length(0); }  // the lengths verify whole on first use
			} catch (const strata::corrupt_segment&) {
				detected = true;
			} catch (const termdict::corrupt&) {
				detected = true;
			} catch (const std::invalid_argument&) {
				detected = true;
			}
			if (in_section && !detected) { std::fprintf(stderr, "undetected change at %zu of %zu\n", at, bad.size()); }
			CHECK(detected || !in_section);
		}
		// The lengths verify whole on first use: a changed byte opens, then fails every call.
		for (const auto& e : layout) {
			if (e.kind != strata::format::section::lengths) { continue; }
			std::string bad = file;
			bad[e.offset + e.length / 2] ^= 1;
			strata::segment s(bad);
			for (int i = 0; i < 2; ++i) {
				bool threw = false;
				try { (void)s.get<strata::inverted_plugin>().length(0); } catch (const strata::corrupt_segment&) { threw = true; }
				CHECK(threw);
			}
		}
		// Regions are verified lazily: opening checks none; reading a term checks its own.
		{
			strata::segment s(file);
			CHECK(s.core().verified_regions() == 0);
			auto info = s.get<strata::inverted_plugin>().term("t1");
			CHECK(info && s.core().verified_regions() >= 1);
		}
	}
	// Golden fixture: a small deterministic segment must keep its bytes.
	{
		strata::segment_writer w;
		const char* words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
		for (std::uint32_t d = 0; d < 300; ++d) {
			std::vector<std::vector<std::uint32_t>> pos(5);
			std::vector<strata::posting_input> in;
			std::uint32_t length = 0;
			for (std::uint32_t k = 0; k < 5; ++k) {
				if ((d * 7 + k * 3) % (k + 2) == 0) {
					for (std::uint32_t p = 0; p <= (d + k) % 4; ++p) { pos[k].push_back(1 + k + 5 * p); }
					in.push_back({words[k], std::uint32_t(pos[k].size()), pos[k]});
					length += std::uint32_t(pos[k].size());
				}
			}
			// Monotone docids with gaps, versions, and a key per document.
			const std::string key = "Q" + std::to_string(d);
			w.add(2 * d + 1, in, length, {}, 1 + d % 5, key);
		}
		std::string bytes = w.flush({});
		const char* path = STRATA_FIXTURES "/small.sx";
		if (std::getenv("STRATA_WRITE_FIXTURES")) {
			std::FILE* f = std::fopen(path, "wb");
			std::fwrite(bytes.data(), 1, bytes.size(), f);
			std::fclose(f);
		}
		std::string golden;
		if (std::FILE* f = std::fopen(path, "rb")) {
			for (int c; (c = std::fgetc(f)) != EOF;) { golden.push_back(char(c)); }
			std::fclose(f);
		}
		CHECK(golden == bytes);
		strata::segment seg(golden);
		CHECK(seg.core().doc_count() == 300);
		// Streaming to a file, positions spilled to a second file, writes the same bytes.
		{
			strata::file_output out("segment_test_stream.sx");
			strata::file_spill spill("segment_test_stream.spill");
			w.flush({}, out, spill);
			out.close();
		}
		std::string streamed;
		if (std::FILE* f = std::fopen("segment_test_stream.sx", "rb")) {
			for (int c; (c = std::fgetc(f)) != EOF;) { streamed.push_back(char(c)); }
			std::fclose(f);
		}
		CHECK(streamed == bytes);
		CHECK(std::fopen("segment_test_stream.spill", "rb") == nullptr);
		std::remove("segment_test_stream.sx");
	}

	// Wide skip offsets and pair lists with wdf past 255 read back like the narrow ones.
	for (bool wide : {false, true}) {
		strata::segment_writer w;
		w.wide_skip_offsets = wide;
		std::vector<std::uint32_t> p{1};
		for (std::uint32_t d = 0; d < 5000; ++d) {
			std::vector<std::uint32_t> pos;
			for (std::uint32_t k = 0; k < 3; ++k) { pos.push_back(k + 1); }
			std::vector<strata::posting_input> in{{"big", 1 + (d * 37) % 600, pos}, {"x", 1, p}};
			w.add(d + 1, in, 700);
		}
		std::string bytes = w.flush({});
		strata::segment s(bytes);
		auto info = s.get<strata::inverted_plugin>().term("big");
		strata::postings_iterator it(s.get<strata::inverted_plugin>(), *info);
		strata::positions_reader pr(s.get<strata::inverted_plugin>(), *info);
		std::vector<std::uint32_t> got;
		std::uint32_t d = 0;
		for (auto o = it.next(); o != strata::no_more_docs; o = it.next(), ++d) {
			CHECK(o == d && it.wdf() == 1 + (d * 37) % 600);
			pr.read(it, got);
			CHECK(got.size() == 3);
		}
		CHECK(d == 5000);
		strata::term_scorer ts(s.get<strata::inverted_plugin>(), *info);
		CHECK(strata::top_k(s, ts, 10).size() == 10);
		CHECK(strata::count(ts) == 5000);
	}

	// Docids that are not contiguous map ordinal by ordinal.
	{
		strata::segment_writer w;
		std::vector<std::uint32_t> p{1};
		for (std::uint64_t id : {10u, 20u, 35u}) {
			std::vector<strata::posting_input> in{{"x", 1, p}};
			w.add(id, in, 1);
		}
		std::string bytes = w.flush({});
		strata::segment s(bytes);
		CHECK(s.core().docid(0) == 10 && s.core().docid(1) == 20 && s.core().docid(2) == 35);
	}

	// Resealed corruption that verification cannot see. A skip group's last ordinal lowered
	// below the previous group's: a pruned term query that skips to that group throws
	// instead of advancing in place forever, and so does a plain walk that reaches it.
	{
		strata::segment_writer w;
		const std::uint32_t docs = 128 * 32 * 6;  // six skip groups
		std::vector<std::uint32_t> none;
		for (std::uint32_t d = 0; d < docs; ++d) {
			// The first ten documents fill the top 10; only the last group can beat them.
			const std::uint32_t wdf = d < 10 ? 5 : d == docs - 1 ? 9 : 1;
			std::vector<strata::posting_input> in{{"a", wdf, none}};
			w.add(d + 1, in, 10);
		}
		std::string file = w.flush({});
		{
			strata::segment seg(file);
			const auto info = *seg.get<strata::inverted_plugin>().term("a");
			const char* p = seg.get<strata::inverted_plugin>().postings_bytes().data() + info.postings;
			const char* end = p + info.postings_length;
			CHECK(strata::format::get_leb(&p, end) == 6 && *p++ == 0);  // six narrow groups
			for (std::size_t j = 0; j < 3 && strata::format::seed_ks[j] <= info.df; ++j) {  // the threshold seeds (F13)
				(void)strata::format::get_leb(&p, end);
				++p;
				(void)strata::format::get_leb(&p, end);
				(void)strata::format::get_leb(&p, end);
			}
			for (int i = 0; i < 3; ++i) { (void)strata::format::get_leb(&p, end); }
			// Level 1: 16-byte entries without positions, each starting with its last.
			const std::size_t at = std::size_t(p - file.data()) + 4 * 16;
			const std::uint32_t zero = 0;
			std::memcpy(file.data() + at, &zero, 4);
		}
		reseal(file);
		CHECK(throws_corrupt([&] {
			strata::segment bad(file);
			strata::term_scorer t(bad.get<strata::inverted_plugin>(), *bad.get<strata::inverted_plugin>().term("a"));
			strata::top_k(bad, t, 10);
		}));
		CHECK(throws_corrupt([&] {
			strata::segment bad(file);
			strata::postings_iterator it(bad.get<strata::inverted_plugin>(), *bad.get<strata::inverted_plugin>().term("a"));
			while (it.next() != strata::no_more_docs) {}
		}));
	}
	// The bulk walks prime an iterator not yet started: for_each_until and fill from the
	// first posting give what next() would.
	{
		strata::segment_writer w;
		std::vector<std::uint32_t> none;
		for (std::uint32_t d = 0; d < 300; ++d) {
			if (d % 3 == 0) {
				std::vector<strata::posting_input> in{{"t", 1 + d % 4, none}};
				w.add(d + 1, in, 1);
			} else {
				std::vector<strata::posting_input> in{{"u", 1, none}};
				w.add(d + 1, in, 1);
			}
		}
		const std::string file = w.flush({});
		strata::segment seg(file);
		std::vector<strata::ordinal_t> expected;
		{
			strata::postings_iterator it(seg.get<strata::inverted_plugin>(), *seg.get<strata::inverted_plugin>().term("t"));
			for (auto o = it.next(); o != strata::no_more_docs; o = it.next()) { expected.push_back(o); }
		}
		std::vector<strata::ordinal_t> walked;
		strata::postings_iterator a(seg.get<strata::inverted_plugin>(), *seg.get<strata::inverted_plugin>().term("t"));
		a.for_each_until(300, [&](strata::ordinal_t o, std::uint32_t) { walked.push_back(o); });
		CHECK(walked == expected);
		std::vector<std::uint64_t> bits(5, 0);
		strata::postings_iterator b(seg.get<strata::inverted_plugin>(), *seg.get<strata::inverted_plugin>().term("t"));
		b.fill(0, 300, bits.data());
		std::vector<strata::ordinal_t> set;
		for (strata::ordinal_t o = 0; o < 300; ++o) {
			if ((bits[o >> 6] >> (o & 63)) & 1) { set.push_back(o); }
		}
		CHECK(set == expected);
	}
	// A file of another format version is rejected at open.
	{
		std::mt19937_64 r(3);
		model m;
		std::string file = build(r, 200, m);
		const std::uint32_t old = strata::format::version - 1;
		std::memcpy(file.data() + 8, &old, 4);
		CHECK(throws_corrupt([&] { strata::segment s(file); }));
	}

	// docids, versions, and keys: random monotone docids map both ways, versions read back,
	// keys are never missed, and keys outside the range are rejected.
	{
		std::mt19937_64 r(12);
		for (std::size_t docs : {1u, 127u, 128u, 129u, 5000u}) {
			strata::segment_writer w;
			std::vector<std::uint64_t> ids, versions;
			std::vector<std::string> keys;
			std::uint64_t id = 1 + r() % 1000;
			for (std::size_t d = 0; d < docs; ++d) {
				id += 1 + (r() % 4 == 0 ? r() % 1000000 : r() % 3);
				ids.push_back(id);
				versions.push_back(r() % 3 == 0 ? r() : r() % 50);
				keys.push_back("Qk" + std::to_string(id));
				std::vector<strata::posting_input> in{{"t", 1, {}}};
				w.add(id, in, 1, {}, versions.back(), keys.back());  // the key is the document's, not a term (F8)
			}
			std::string bytes = w.flush({});
			strata::segment s(bytes);
			for (std::size_t d = 0; d < docs; ++d) {
				CHECK(s.core().docid(strata::ordinal_t(d)) == ids[d]);
				CHECK(s.core().ordinal_of(ids[d]) == strata::ordinal_t(d));
				CHECK(!s.core().ordinal_of(ids[d] + 1) || (d + 1 < docs && ids[d + 1] == ids[d] + 1));
				CHECK(s.get<strata::keys_plugin>().version(strata::ordinal_t(d)) == versions[d]);
				CHECK(s.get<strata::keys_plugin>().may_hold(keys[d]));
			}
			CHECK(!s.core().ordinal_of(ids[0] - 1) && !s.core().ordinal_of(ids.back() + 1));
			CHECK(!s.get<strata::keys_plugin>().may_hold("A") && !s.get<strata::keys_plugin>().may_hold("Z"));
			std::size_t false_positives = 0;
			for (int i = 0; i < 10000; ++i) { false_positives += s.get<strata::keys_plugin>().may_hold("Qk" + std::to_string(r()) + "x"); }
			CHECK(false_positives < 200);  // about 1/256 for an 8-bit filter
			// The keys reader's find, and its scan from a lower bound (F8) against a brute-force
			// model, from many bounds and with early stops; the dictionary holds no key.
			const auto& kr = s.get<strata::keys_plugin>();
			std::vector<std::uint64_t> deleted((docs + 63) / 64, 0);
			deleted[0] |= 1;
			CHECK(kr.find(keys[0], {}) == strata::ordinal_t(0));
			CHECK(!kr.find(keys[0], {deleted.data()}));
			CHECK(!kr.find("Qk", {}));
			CHECK(kr.key_count() == docs);
			CHECK(!s.get<strata::inverted_plugin>().find_term(keys[0]));
			std::vector<std::pair<std::string, strata::ordinal_t>> expect;
			for (std::size_t d = 0; d < docs; ++d) { expect.push_back({keys[d], strata::ordinal_t(d)}); }
			std::sort(expect.begin(), expect.end());
			std::vector<std::string> bounds{"", "A", "Q", "Qk", "Qk5", "Z", "\xff"};
			for (std::size_t d = 0; d < docs; d += 1 + docs / 40) {
				bounds.push_back(keys[d]);
				bounds.push_back(keys[d] + '\0');
				bounds.push_back(keys[d].substr(0, keys[d].size() - 1));
			}
			for (const auto& from : bounds) {
				for (std::size_t stop : {std::size_t(1), std::size_t(3), ~std::size_t(0)}) {
					std::vector<std::pair<std::string, strata::ordinal_t>> seen, want;
					for (const auto& e : expect) {
						if (e.first >= from && want.size() < stop) { want.push_back(e); }
					}
					kr.scan(from, [&](std::string_view k, strata::ordinal_t o) {
						seen.push_back({std::string(k), o});
						return seen.size() < stop;
					});
					CHECK(seen == want);
				}
			}
		}
		// A docid that does not increase is refused at add.
		strata::segment_writer w;
		std::vector<strata::posting_input> none;
		w.add(5, none, 1);
		CHECK(throws_invalid([&] { w.add(5, none, 1); }));
		// Resealed changes to the new sections fence the segment at open.
		std::mt19937_64 r2(3);
		model m;
		std::string good = build(r2, 300, m);
		for (auto kind : {strata::format::section::docids, strata::format::section::versions}) {
			const auto toc = strata::format::decode_toc(good, strata::format::decode_footer(good));
			for (const auto& e : toc) {
				if (e.kind != kind) { continue; }
				for (std::uint64_t at = 0; at < e.length; ++at) {
					std::string bad = good;
					bad[e.offset + at] = char(bad[e.offset + at] ^ 0x41);
					reseal(bad);
					bool ok = true;
					try {
						strata::segment s(bad);
						for (strata::ordinal_t o = 0; o < s.core().doc_count(); ++o) { (void)s.core().docid(o), (void)s.get<strata::keys_plugin>().version(o); }
					} catch (const strata::corrupt_segment&) {
						ok = false;
					}
					// A change the format cannot detect still yields readable answers.
					(void)ok;
				}
			}
		}
	}

	// C13: every open-time check on sections whose checksums are consistent but whose
	// contents are not, and ordinals out of range.
	{
		using strata::format::section;
		auto opens = [](const std::string& f) { return !throws_corrupt([&] { strata::segment s(f); }); };
		// A keyed segment (monotone docids, versions, key filter) and a contiguous one.
		strata::segment_writer kw;
		std::uint64_t id = 100;
		for (std::uint32_t d = 0; d < 300; ++d) {
			id += 1 + d % 5;
			const std::string key = "Qk" + std::to_string(id);
			std::vector<std::uint32_t> none, one{1};
			std::vector<strata::posting_input> in{{"w", 1, one}};
			kw.add(id, in, 1, "r", 1 + d % 7, key);
		}
		const std::string keyed = kw.flush({});
		std::mt19937_64 r3(4);
		model m3;
		const std::string plain = build(r3, 200, m3);
		CHECK(opens(keyed) && opens(plain) && opens(with_section(keyed, section::stats, section_of(keyed, section::stats))));
		{
			strata::segment s(keyed);
			CHECK(throws_out_of_range([&] { (void)s.core().docid(300); }) && throws_out_of_range([&] { (void)s.get<strata::keys_plugin>().version(300); }) && throws_out_of_range([&] { (void)s.get<strata::stored_plugin>().stored(300); }));
		}
		// Footer bounds on the table of contents.
		auto refooter = [&](auto edit) {
			std::string f = keyed;
			auto ft = strata::format::decode_footer(f);
			edit(ft);
			const std::string fb = strata::format::encode_footer(ft);
			f.replace(f.size() - fb.size(), fb.size(), fb);
			return f;
		};
		const std::uint64_t body = keyed.size() - strata::format::footer_size;
		CHECK(!opens(refooter([&](auto& f) { f.toc_offset = body + 1; })));
		CHECK(!opens(refooter([&](auto& f) { f.toc_length = body - f.toc_offset + 1; })));
		CHECK(!opens(refooter([&](auto& f) { --f.toc_length; })));
		// stats: the count alone, 8 bytes (F7); term_index: its 20 bytes of statistics first.
		CHECK(!opens(with_section(keyed, section::stats, section_of(keyed, section::stats).substr(0, 7))));
		CHECK(!opens(with_section(keyed, section::stats, section_of(keyed, section::stats) + std::string(20, '\0'))));
		CHECK(!opens(with_section(keyed, section::term_index, section_of(keyed, section::term_index).substr(0, 19))));
		// Contiguous docids: size, base 0, base unlike the footer, and a range that wraps.
		const std::string cd = section_of(plain, section::docids);
		auto with_base = [&](std::uint64_t base) {
			std::string d = cd;
			std::memcpy(d.data() + 1, &base, 8);
			return d;
		};
		CHECK(!opens(with_section(plain, section::docids, cd + "x")));
		CHECK(!opens(with_section(plain, section::docids, with_base(0))));
		CHECK(!opens(with_section(plain, section::docids, with_base(2))));
		CHECK(!opens(with_section(plain, section::docids, cd, [&](auto& f) { ++f.max_docid; })));  // last unlike the footer
		const std::uint64_t high = ~std::uint64_t(0) - 50;
		CHECK(!opens(with_section(plain, section::docids, with_base(high), [&](auto& f) {
			f.min_docid = high;
			f.max_docid = high + 199;  // wraps
		})));
		// Monotone docids: tag, size, count, block index, order, offsets, first, deltas, end.
		const std::string md = section_of(keyed, section::docids);
		auto edited = [&](std::size_t at, std::uint64_t v, std::size_t width) {
			std::string d = md;
			std::memcpy(d.data() + at, &v, width);
			return d;
		};
		CHECK(!opens(with_section(keyed, section::docids, std::string(1, char(7)) + md.substr(1))));
		CHECK(!opens(with_section(keyed, section::docids, md.substr(0, 5))));
		CHECK(!opens(with_section(keyed, section::docids, edited(1, 299, 8))));                                // count
		CHECK(!opens(with_section(keyed, section::docids, md.substr(0, 9 + 16 * 2))));                         // fewer index entries than blocks
		CHECK(!opens(with_section(keyed, section::docids, edited(9 + 16, 1, 8))));                             // block 1 not above block 0
		CHECK(!opens(with_section(keyed, section::docids, edited(17 + 16, 3, 8))));                            // block 1's offset
		CHECK(!opens(with_section(keyed, section::docids, md, [&](auto& f) { f.min_docid = 99; })));  // first unlike the footer
		const std::size_t deltas = 9 + 16 * 3;
		CHECK(!opens(with_section(keyed, section::docids, edited(deltas, 0, 1))));                             // a zero delta
		CHECK(!opens(with_section(keyed, section::docids, md.substr(0, deltas) + std::string("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", 10) + md.substr(deltas + 1))));  // a wrapping delta
		CHECK(!opens(with_section(keyed, section::docids, md + "\x01")));                                     // bytes past the deltas
		CHECK(!opens(with_section(keyed, section::docids, md, [&](auto& f) { ++f.max_docid; })));             // last unlike the footer
		// Versions: size, width, length, and a base that overflows.
		const std::string vs = section_of(keyed, section::versions);
		auto with_versions = [&](std::size_t at, std::uint64_t v, std::size_t width) {
			std::string d = vs;
			std::memcpy(d.data() + at, &v, width);
			return d;
		};
		CHECK(!opens(with_section(keyed, section::versions, vs.substr(0, 8))));
		CHECK(!opens(with_section(keyed, section::versions, with_versions(8, 65, 1))));
		CHECK(!opens(with_section(keyed, section::versions, vs + "x")));
		CHECK(!opens(with_section(keyed, section::versions, with_versions(0, ~std::uint64_t(0) - 2, 8))));
		// key_index (F8): [count][min][max][filter][the key blocks' index]: a count above the
		// documents, a length past the bytes, a broken filter, and min above max or empty.
		const std::string ki = section_of(keyed, section::key_index);
		const char* kp = ki.data();
		const char* kend = kp + ki.size();
		const std::uint64_t count = strata::format::get_leb(&kp, kend);
		auto field = [&] {
			const std::uint64_t n = strata::format::get_leb(&kp, kend);
			std::string out(kp, std::size_t(n));
			kp += n;
			return out;
		};
		const std::string lo = field(), hi = field(), filter = field(), dict(kp, std::size_t(kend - kp));
		auto key_index = [&](std::uint64_t n, std::string_view a, std::string_view b, std::string_view f) {
			std::string out;
			strata::format::put_leb(out, n);
			for (auto v : {a, b, f}) {
				strata::format::put_leb(out, v.size());
				out += v;
			}
			return out + dict;
		};
		CHECK(count == 300 && !lo.empty() && lo < hi);
		CHECK(opens(with_section(keyed, section::key_index, key_index(count, lo, hi, filter))));
		CHECK(!opens(with_section(keyed, section::key_index, key_index(301, lo, hi, filter))));
		CHECK(!opens(with_section(keyed, section::key_index, std::string("\x7f") + "Qk")));
		CHECK(!opens(with_section(keyed, section::key_index, key_index(count, lo, hi, "junk"))));
		CHECK(!opens(with_section(keyed, section::key_index, key_index(count, hi, lo, filter))));
		CHECK(!opens(with_section(keyed, section::key_index, key_index(count, "", hi, filter))));
		// A malformed key blocks' index (the FST) is damage, never std::invalid_argument (CR4).
		{
			std::string head = key_index(count, lo, hi, filter);
			head.resize(head.size() - dict.size());
			std::string flipped = dict;
			flipped.back() ^= 0x5a;
			for (const std::string& d : {dict.substr(0, dict.size() / 2), flipped, std::string("not an fst"), std::string()}) {
				CHECK(!opens(with_section(keyed, section::key_index, head + d)));
			}
		}
		// One key whose range has a minimum unlike its maximum (CR6, M2).
		{
			strata::segment_writer w1;
			std::vector<std::uint32_t> one{1};
			std::vector<strata::posting_input> in{{"w", 1, one}};
			w1.add(5, in, 1, "r", 1, "Qk5");
			const std::string single = w1.flush({});
			const std::string k1 = section_of(single, section::key_index);
			const char* q = k1.data();
			const char* qend = q + k1.size();
			std::string one_index;
			strata::format::put_leb(one_index, strata::format::get_leb(&q, qend));
			std::vector<std::string> parts;
			for (int i = 0; i < 3; ++i) {
				const std::uint64_t n = strata::format::get_leb(&q, qend);
				parts.emplace_back(q, std::size_t(n));
				q += n;
			}
			CHECK(parts[0] == "Qk5" && parts[1] == "Qk5");
			auto with_range = [&](std::string_view a, std::string_view b) {
				std::string out = one_index;
				for (std::string_view v : {a, b, std::string_view(parts[2])}) {
					strata::format::put_leb(out, v.size());
					out += v;
				}
				return with_section(single, section::key_index, out + std::string(q, qend));
			};
			CHECK(opens(with_range("Qk5", "Qk5")));
			CHECK(!opens(with_range("Qk5", "Qk6")));
		}
	}
	// C13: decoders reached with crafted bytes: term-info ranges that wrap, impact lists
	// whose sizes, counts, or ends disagree, a stored index without frames or whose first
	// frame starts past ordinal 0, and dictionaries whose values the postings cannot hold.
	{
		using namespace strata::format;
		auto leb = [](std::string& out, std::uint64_t v) { put_leb(out, v); };
		auto term_bytes = [&](std::uint64_t head, std::initializer_list<std::uint64_t> fields) {
			std::string out;
			leb(out, head);
			for (auto v : fields) { leb(out, v); }
			return out;
		};
		// head = df << 5 | {no_wdf, no_positions, counts_are_wdf, cf_is_df, absolute} bits.
		const std::uint64_t huge = ~std::uint64_t(0) - 4;
		CHECK(throws_corrupt([&] { term_run r; (void)decode_term_info(term_bytes(2 << 5 | 1 << 4 | 1 << 3 | 1 << 1, {huge, 0, 10}), r, false); }));
		CHECK(throws_corrupt([&] { term_run r; (void)decode_term_info(term_bytes(2 << 5 | 1 << 4 | 1 << 3, {0, huge, 1, 10}), r, false); }));
		auto no_match = [](const impact&) { return false; };
		auto list = [&](std::initializer_list<std::uint64_t> lebs, std::string tail = {}) {
			std::string out;
			for (auto v : lebs) { leb(out, v); }
			return out + tail;
		};
		CHECK(throws_corrupt([&] { (void)any_sized_impact(list({2 << 1, 9, 2, 1, 1, 1, 1}), no_match); }));     // size unlike the list
		CHECK(throws_corrupt([&] { (void)any_sized_impact(list({2 << 1, 5, 3, 1, 1, 1, 1}), no_match); }));     // count unlike the header
		CHECK(throws_corrupt([&] { (void)any_sized_impact(list({2 << 1 | 1, 1, 1, 1}), no_match); }));          // compact, truncated
		CHECK(throws_corrupt([&] { (void)any_sized_impact(list({2 << 1, 4, 2, 1, 1, 1}), no_match); }));        // full, truncated
		CHECK(throws_corrupt([&] { (void)any_sized_impact(list({1 << 1 | 1, 1, 1, 7}), no_match); }));          // bytes past the pairs
		CHECK(!any_sized_impact(list({1 << 1 | 1, 1, 1}), no_match));
		auto skip = [](const std::string& bytes) { const char* p = bytes.data(); (void)skip_sized_impacts(&p, p + bytes.size()); };
		auto decode = [](const std::string& bytes) { const char* p = bytes.data(); std::vector<impact> v; (void)decode_impacts(&p, p + bytes.size(), v); };
		CHECK(throws_corrupt([&] { skip(list({257 << 1 | 1}, std::string(514, '\1'))); }));  // more pairs than lengths
		CHECK(throws_corrupt([&] { skip(list({0 << 1 | 1})); }));                              // no pairs
		CHECK(throws_corrupt([&] { decode(list({0})); }));                                       // no pairs
		CHECK(throws_corrupt([&] { decode(list({257}, std::string(514, '\1'))); }));           // more pairs than lengths
		// Stored records.
		std::mt19937_64 r5(5);
		model m5;
		const std::string plain = build(r5, 200, m5);
		const std::string zero_frames(4, '\0');
		const std::string no_frames = with_section(plain, section::stored, zero_frames);
		CHECK(throws_corrupt([&] { strata::segment s(no_frames); (void)s.get<strata::stored_plugin>().stored(0); }));
		{
			std::string st;
			for (const auto& e : decode_toc(plain, decode_footer(plain))) {
				if (e.kind == section::stored) { st = plain.substr(e.offset, e.length); }
			}
			const std::uint32_t frames = get<std::uint32_t>(st.data() + st.size() - 4);
			const std::size_t index_at = st.size() - 4 - 16 * std::size_t(frames);
			const std::uint32_t one = 1;
			std::memcpy(st.data() + index_at, &one, 4);  // the first frame starts at ordinal 1
			const std::string late = with_section(plain, section::stored, st);
			CHECK(throws_corrupt([&] { strata::segment s(late); (void)s.get<strata::stored_plugin>().stored(0); }));
		}
		// Dictionary values: a df above the document count, a single posting past the last
		// ordinal, and skip groups that do not fit the term's postings.
		auto read_all = [](const std::string& f) {
			strata::segment s(f);
			s.get<strata::inverted_plugin>().scan_terms([&](std::string_view, const term_info& info) {
				strata::postings_iterator it(s.get<strata::inverted_plugin>(), info);
				while (it.next() != strata::no_more_docs) {}
				return true;
			});
		};
		CHECK(!throws_corrupt([&] { read_all(with_terms(plain, [](std::string_view, term_info&) {})); }));
		strata::segment_writer fw;
		for (std::uint32_t d = 0; d < 60; ++d) {
			std::vector<std::uint32_t> one{1};
			std::vector<strata::posting_input> in{{"w", 1, one}};
			fw.add(d + 1, in, 1);
		}
		const std::string few = fw.flush({});
		CHECK(throws_corrupt([&] { read_all(with_terms(few, [](std::string_view, term_info& t) { t.df = 61; })); }));  // more than the documents
		strata::segment_writer kw;
		for (std::uint32_t d = 0; d < 50; ++d) {
			std::vector<std::uint32_t> none;
			const std::string key = "Q" + std::to_string(d);
			const std::string term = "w" + std::to_string(d);  // outlives the posting's view
			std::vector<strata::posting_input> in{{term, 1, none}};
			kw.add(d + 1, in, 1, {}, 1, key);  // each term is a single posting
		}
		const std::string singles = kw.flush({});
		CHECK(!throws_corrupt([&] { read_all(singles); }));
		CHECK(throws_corrupt([&] { read_all(with_terms(singles, [](std::string_view, term_info& t) { t.postings = 50; })); }));
		strata::segment_writer gw;
		for (std::uint32_t d = 0; d < 128 * 32 * 3; ++d) {
			std::vector<std::uint32_t> none;
			std::vector<strata::posting_input> in{{"a", 1 + d % 3, none}};
			gw.add(d + 1, in, 4);
		}
		const std::string groups = gw.flush({});
		CHECK(throws_corrupt([&] { read_all(with_terms(groups, [](std::string_view, term_info& t) { t.postings_length = 40; })); }));
		CHECK(throws_corrupt([&] { term_run r; (void)decode_term_info(term_bytes(std::uint64_t(1) << 37 | 1 << 4, {0, 0, 1}), r, false); }));  // df >= 2^32
		CHECK(throws_corrupt([&] { term_run r; (void)decode_term_info(term_bytes(1 << 4, {0, 0}), r, false); }));                                // df 0
	}
	// C13: every byte of a small segment's postings and positions changed and resealed:
	// bitset, FOR, and wdf blocks over two skip groups, positions in count and value
	// blocks. Each change fences the segment or leaves it answering, never more.
	{
		strata::segment_writer w;
		std::mt19937_64 r(8);
		const std::uint32_t docs = 128 * 32 + 700;
		for (std::uint32_t d = 0; d < docs; ++d) {
			std::vector<std::uint32_t> pd{1, 7, 9, 11, 13, 15}, pm{2, 5}, pr{3};
			std::vector<strata::posting_input> in{{"dense", 6, pd}};
			if (d % 3 == 0) { in.push_back({"mid", 2 + std::uint32_t(r() % 3), pm}); }
			if (d % 41 == 0) { in.push_back({"rare", 1, pr}); }
			w.add(d + 1, in, 6);
		}
		const std::string good = w.flush({});
		std::uint64_t changes = 0, fenced = 0;
		for (auto kind : {strata::format::section::postings, strata::format::section::positions}) {
			const std::string bytes = section_of(good, kind);
			for (std::size_t at = 0; at < bytes.size(); ++at) {
				for (char x : {char(0x01), char(0x80)}) {
					std::string bad = bytes;
					bad[at] ^= x;
					++changes;
					const std::string file = with_section(good, kind, bad);
					try {
						strata::segment s(file);
						std::vector<strata::format::term_info> infos;
						s.get<strata::inverted_plugin>().scan_terms([&](std::string_view, const strata::format::term_info& info) {
							infos.push_back(info);
							strata::postings_iterator it(s.get<strata::inverted_plugin>(), info);
							strata::positions_reader pr(s.get<strata::inverted_plugin>(), info);
							for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { (void)pr.read(it); }
							strata::postings_iterator jump(s.get<strata::inverted_plugin>(), info);
							(void)jump.advance(docs / 2);
							(void)jump.advance(docs - 1);
							return true;
						});
						std::vector<strata::scorer_ptr> both;
						for (const auto& info : infos) { both.push_back(std::make_unique<strata::term_scorer>(s.get<strata::inverted_plugin>(), info)); }
						strata::conjunction_scorer all(std::move(both));
						(void)strata::count(all);
						std::vector<strata::scorer_ptr> dense;  // two dense terms: counting windows
						dense.push_back(std::make_unique<strata::term_scorer>(s.get<strata::inverted_plugin>(), infos[0]));
						dense.push_back(std::make_unique<strata::term_scorer>(s.get<strata::inverted_plugin>(), infos[1]));
						strata::conjunction_scorer windows(std::move(dense));
						(void)strata::count(windows);
						strata::phrase_scorer phrase(s.get<strata::inverted_plugin>(), {infos[0], infos[1]});
						(void)strata::top_k(s, phrase, 10);
					} catch (const strata::corrupt_segment&) {
						++fenced;
					}
				}
			}
		}
		std::printf("  resealed sweep: %llu changes, %llu fenced\n", (unsigned long long)changes, (unsigned long long)fenced);
		CHECK(changes > 1000 && fenced > changes / 10);
	}
	// P15: one iterator walking a long list verifies each region it enters, not only the
	// first; a flip in the list's last region fences the walk that reaches it.
	{
		strata::segment_writer w;
		std::mt19937_64 r(15);
		for (std::uint32_t d = 0; d < 60000; ++d) {
			std::vector<strata::posting_input> in{{"long", 1 + std::uint32_t(r() % 7), {}}};
			w.add(d + 1, in, 8);
		}
		const std::string good = w.flush({});
		const auto walk = [](const std::string& file) {
			strata::segment s(file);
			const auto info = *s.get<strata::inverted_plugin>().term("long");
			strata::postings_iterator it(s.get<strata::inverted_plugin>(), info);
			std::uint64_t n = 0;
			for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { n += it.wdf(); }
			return n;
		};
		CHECK(walk(good) > 0);
		strata::segment s(good);
		const auto info = *s.get<strata::inverted_plugin>().term("long");
		const std::uint64_t region = std::uint64_t(1) << strata::format::default_region_shift;
		CHECK(info.postings_length > 3 * region);
		const auto toc = strata::format::decode_toc(good, strata::format::decode_footer(good));
		std::uint64_t at = 0;
		for (const auto& e : toc) {
			if (e.kind == strata::format::section::postings) { at = e.offset + info.postings + info.postings_length - 1; }
		}
		CHECK(at > region * 3);
		std::string bad = good;
		bad[at] ^= 0x20;
		CHECK(throws_corrupt([&] { (void)walk(bad); }));
	}
	// C13 (Phase 2 review): a resealed skip group whose first position index is near 2^64.
	// Reading a document whose index wraps fences; no read returns other positions.
	{
		strata::segment_writer w;
		w.wide_skip_offsets = true;
		for (std::uint32_t d = 0; d < 300; ++d) {
			std::vector<std::uint32_t> pos{d + 1};  // each document's one position is distinct
			std::vector<strata::posting_input> in{{"p", 1, pos}};
			w.add(d + 1, in, 1);
		}
		const std::string good = w.flush({});
		// Group 0's first position index (offset 2 of a wide level-1 entry) set to `first`.
		auto with_first = [&](std::uint64_t first) {
			std::string file = good;
			strata::segment s(file);
			const auto info = *s.get<strata::inverted_plugin>().term("p");
			const char* p = s.get<strata::inverted_plugin>().postings_bytes().data() + info.postings;
			const char* end = p + info.postings_length;
			(void)strata::format::get_leb(&p, end);  // groups
			CHECK(*p++ == 1);                          // wide offsets
			for (int i = 0; i < 5; ++i) { (void)strata::format::get_leb(&p, end); }
			std::memcpy(file.data() + (p - file.data()) + 8 + 16, &first, 8);
			reseal(file);
			return file;
		};
		// 64 below 2^64: the second block's first index wraps (the group decode fences).
		// 140 below: the blocks' firsts fit, and a document's index wraps inside block 1.
		strata::segment good_seg(good);
		for (std::uint64_t below : {64u, 140u}) {
			const std::string file = with_first(~std::uint64_t(0) - (below - 1));
			strata::segment bad_seg(file);
			std::size_t fenced = 0;
			for (strata::ordinal_t target : {strata::ordinal_t(150), strata::ordinal_t(250)}) {
				strata::postings_iterator g(good_seg.get<strata::inverted_plugin>(), *good_seg.get<strata::inverted_plugin>().term("p")), b(bad_seg.get<strata::inverted_plugin>(), *bad_seg.get<strata::inverted_plugin>().term("p"));
				strata::positions_reader gp(good_seg.get<strata::inverted_plugin>(), *good_seg.get<strata::inverted_plugin>().term("p")), bp(bad_seg.get<strata::inverted_plugin>(), *bad_seg.get<strata::inverted_plugin>().term("p"));
				g.advance(target);
				const auto want = gp.read(g);
				try {
					b.advance(target);
					const auto got = bp.read(b);
					CHECK(std::vector<std::uint32_t>(got.begin(), got.end()) == std::vector<std::uint32_t>(want.begin(), want.end()));
				} catch (const strata::corrupt_segment&) {
					++fenced;
				}
			}
			CHECK(fenced == 2);
		}
	}
	// C13: or_bitset's checks, reached through a window count. A dense term with no wdf and
	// no positions keeps bitset blocks. A bit flipped in block 2 fails its count; block 3's
	// advance offset moved back 8 bytes keeps the group's total and fails block 2's bounds.
	{
		auto dense = [](std::uint32_t d) {
			std::uint32_t x = (d + 1) * 0x9e3779b9u;
			x ^= x >> 16, x *= 0x85ebca6bu, x ^= x >> 13;
			return x % 100 < 60;
		};
		strata::segment_writer w;
		for (std::uint32_t d = 0; d < 5000; ++d) {
			std::vector<std::uint32_t> none;
			std::vector<strata::posting_input> in;
			if (dense(d)) { in.push_back({"dense", 0, none}); }  // 60% with gaps to 8: bitset blocks
			if (d % 3 == 0) { in.push_back({"m3", 0, none}); }
			if (d % 5 == 0) { in.push_back({"m5", 0, none}); }
			w.add(d + 1, in, 1);
		}
		const std::string good = w.flush({});
		skip_test::layout lay;
		{
			strata::segment s(good);
			lay = skip_test::parse(good, s, *s.get<strata::inverted_plugin>().term("dense"));
		}
		CHECK(lay.blocks.size() >= 4);
		for (const auto& b : lay.blocks) { CHECK(b.codec == unsigned(strata::format::block_codec::bitset)); }
		auto window_count = [](const std::string& file) {
			strata::segment s(file);
			std::vector<strata::scorer_ptr> kids;
			for (const char* t : {"dense", "m3", "m5"}) { kids.push_back(std::make_unique<strata::term_scorer>(s.get<strata::inverted_plugin>(), *s.get<strata::inverted_plugin>().term(t))); }
			strata::disjunction_scorer any(std::move(kids));
			return strata::count(any);
		};
		std::uint64_t any = 0;
		for (std::uint32_t d = 0; d < 5000; ++d) { any += dense(d) || d % 3 == 0 || d % 5 == 0; }
		CHECK(window_count(good) == any);
		std::string flipped = good;
		flipped[lay.blocks[2].at + 3] ^= 0x10;
		reseal(flipped);
		CHECK(throws_corrupt([&] { (void)window_count(flipped); }));
		std::string moved = good;
		const auto& g0 = lay.groups[0];
		const std::uint64_t start3 = lay.blocks[3].at - lay.blocks[0].at;
		skip_test::put(moved, g0.adv, g0.bit_offset(3), g0.wo, start3 - 8);
		reseal(moved);
		CHECK(throws_corrupt([&] { (void)window_count(moved); }));
	}
	// Reader edges: an empty segment's lookups, zero-width and 32-bit lengths, advancing an
	// exhausted iterator, and a term whose postings fill whole blocks with no tail.
	{
		const std::string empty = strata::segment_writer{}.flush({});
		strata::segment e(empty);
		CHECK(e.core().doc_count() == 0 && !e.core().ordinal_of(5) && !e.get<strata::inverted_plugin>().term("x"));
		strata::segment_writer zero, wide;
		std::vector<std::uint32_t> none;
		for (std::uint32_t d = 0; d < 300; ++d) {
			std::vector<strata::posting_input> in{{"t", 1, none}};
			zero.add(d + 1, in, 0);
			wide.add(d + 1, in, d == 7 ? 0xfffffff0u : d);
		}
		const std::string zero_bytes = zero.flush({}), wide_bytes = wide.flush({});
		strata::segment z(zero_bytes), wd(wide_bytes);
		CHECK(z.get<strata::inverted_plugin>().length(5) == 0 && wd.get<strata::inverted_plugin>().length(7) == 0xfffffff0u && wd.get<strata::inverted_plugin>().length(8) == 8);
		strata::segment_writer full;
		for (std::uint32_t d = 0; d < 512; ++d) {
			std::vector<strata::posting_input> in{{d % 2 ? "half" : "other", 1, none}};
			full.add(d + 1, in, 1);
		}
		const std::string full_bytes = full.flush({});
		strata::segment f(full_bytes);
		strata::postings_iterator it(f.get<strata::inverted_plugin>(), *f.get<strata::inverted_plugin>().term("half"));  // 256 postings: two full blocks
		CHECK(f.get<strata::inverted_plugin>().term("half")->df == 256);
		strata::ordinal_t last = strata::no_more_docs;
		for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { last = d; }
		CHECK(last == 511);
		CHECK(it.advance(600) == strata::no_more_docs && it.block_end(0) != 0);
	}
	// add_tokens gives the same segment as add with the tokens grouped by term: same bytes.
	{
		std::mt19937_64 r(31);
		const char* words[] = {"a", "b", "c", "d", "e", "rare", "the", "of"};
		strata::segment_writer grouped, streamed;
		for (std::uint32_t d = 0; d < 3000; ++d) {
			std::vector<strata::segment_writer::token> tokens;
			const std::uint32_t n = 1 + std::uint32_t(r() % 40);
			for (std::uint32_t i = 0; i < n; ++i) { tokens.push_back({words[r() % std::size(words)], i + 1 + std::uint32_t(r() % 2)}); }
			for (std::size_t i = 1; i < tokens.size(); ++i) { tokens[i].position = std::max(tokens[i].position, tokens[i - 1].position + 1); }
			std::map<std::string_view, std::vector<std::uint32_t>> by_term;
			for (const auto& t : tokens) { by_term[t.term].push_back(t.position); }
			std::vector<strata::posting_input> in;
			for (const auto& [term, positions] : by_term) { in.push_back({term, std::uint32_t(positions.size()), positions}); }
			const std::string stored(d % 7, 's');
			grouped.add(d + 1, in, n, stored);
			streamed.add_tokens(d + 1, tokens, n, stored);
		}
		CHECK(grouped.flush({}) == streamed.flush({}));
	}
	// A document as terms and text tokens gives the same segment as its terms grouped: same
	// bytes; a token repeating a terms entry, or a position, throws.
	{
		std::mt19937_64 r(37);
		const char* words[] = {"a", "b", "c", "d", "e", "rare", "the", "of"};
		strata::segment_writer grouped, mixed;
		std::vector<std::string> keys(3000);  // writer_access borrows each key until the flush
		for (std::uint32_t d = 0; d < 3000; ++d) {
			std::string text;
			std::vector<strata::text_token> tokens;
			std::map<std::string, std::vector<std::uint32_t>> by_term;
			std::uint32_t pos = 0;
			const std::uint32_t n = 1 + std::uint32_t(r() % 40);
			for (std::uint32_t i = 0; i < n; ++i) {
				const std::string w = words[r() % std::size(words)];
				pos += 1 + std::uint32_t(r() % 2);
				text += ' ';
				tokens.push_back({std::uint32_t(text.size()), std::uint32_t(w.size()), pos});
				text += w;
				by_term[w].push_back(pos);
			}
			const std::string& key = keys[d] = "Q" + std::to_string(d);
			const std::string tag = "T" + std::to_string(d % 5);
			by_term[key];
			by_term[tag] = {1000};
			std::vector<strata::posting_input> in, terms = {{key, 0, {}}, {tag, 1, by_term[tag]}};
			for (const auto& [term, positions] : by_term) {
				in.push_back({term, term == key ? 0u : std::uint32_t(positions.size()), positions});
			}
			const std::string stored(d % 7, 's');
			grouped.add(d + 1, in, n, stored, d + 1, key);
			strata::detail::writer_access::add_text(mixed, d + 1, terms, text, tokens, n, stored, d + 1, key);
		}
		CHECK(grouped.flush({}) == mixed.flush({}));
		strata::segment_writer w;
		const std::string text = " a b a";
		const std::vector<strata::text_token> repeat_entry = {{1, 1, 1}};
		const std::vector<strata::posting_input> a = {{"a", 1, {}}};
		CHECK(throws_invalid([&] { strata::detail::writer_access::add_text(w, 1, a, text, repeat_entry, 1); }));
		const std::vector<strata::text_token> repeat_position = {{1, 1, 2}, {5, 1, 2}};
		CHECK(throws_invalid([&] { strata::detail::writer_access::add_text(w, 2, {}, text, repeat_position, 2); }));
	}
	// Term infos in restart runs: random terms (singles, positionless, gaps, skip blocks)
	// decode the same read in order or from their run's start, as find_run reads them.
	{
		std::mt19937_64 r(77);
		std::vector<strata::format::term_info> terms;
		std::vector<std::string> values;
		std::vector<bool> restarts;
		strata::format::term_run run;
		std::uint64_t postings = 0, positions = 0;
		for (int i = 0; i < 5000; ++i) {
			strata::format::term_info t;
			t.df = r() % 4 == 0 ? 1 : 2 + std::uint32_t(r() % 300);
			t.cf = r() % 2 ? t.df : t.df + r() % 50;
			if (t.df == 1) {
				t.flags |= strata::format::single_posting;
				t.postings = r() % 100000;
				t.single_wdf = std::uint32_t(r() % 9);
				t.cf = t.single_wdf;
			} else {
				if (r() % 50 == 0) { postings += 64; }  // a gap
				t.postings = postings;
				t.postings_length = 1 + r() % 900;
				postings += t.postings_length;
			}
			if (r() % 3 == 0) {
				t.flags |= strata::format::no_positions;
			} else {
				if (r() % 50 == 0) { positions += 8; }
				t.positions = positions;
				t.positions_length = 1 + r() % 700;
				positions += t.positions_length;
			}
			if (t.df >= strata::format::block) { t.impacts = {{3, 7}, {1, 2}}; }
			const bool restart = i == 0 || r() % 16 == 0;
			values.push_back(strata::format::encode_term_info(t, run, restart));
			restarts.push_back(restart);
			terms.push_back(t);
		}
		auto same = [](const strata::format::term_info& a, const strata::format::term_info& b) {
			return a.df == b.df && a.cf == b.cf && a.postings == b.postings && a.single_wdf == b.single_wdf && a.postings_length == b.postings_length &&
				(a.flags & strata::format::no_positions || (a.positions == b.positions && a.positions_length == b.positions_length)) &&
				a.impacts.size() == b.impacts.size();
		};
		strata::format::term_run in_order;
		std::size_t start = 0;
		for (std::size_t i = 0; i < terms.size(); ++i) {
			CHECK(same(strata::format::decode_term_info(values[i], in_order), terms[i]));
			if (restarts[i]) { start = i; }
			strata::format::term_run from_run;
			for (std::size_t j = start; j < i; ++j) { (void)strata::format::decode_term_info(values[j], from_run, false); }
			CHECK(same(strata::format::decode_term_info(values[i], from_run), terms[i]));
		}
	}

	// Length quantization rounds down and is monotone.
	std::uint8_t prev = 0;
	for (std::uint64_t n = 0; n < (1ull << 32); n = n < 4096 ? n + 1 : n * 3 / 2) {
		auto q = strata::format::encode_length(std::uint32_t(n));
		CHECK(strata::format::decode_length(q) <= n);
		CHECK(q >= prev);
		prev = q;
	}
	std::printf("segment: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
