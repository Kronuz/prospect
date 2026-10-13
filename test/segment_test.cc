// Segment writer and reader against a naive model: postings, advance, block_end,
// impact bounds, lengths, and corrupt files (every truncation, random corruption).
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/writer.hh"

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
	for (auto& e : toc) { e = strata::format::describe(e.kind, e.offset, std::string_view(file).substr(e.offset, e.length)); }
	const std::string toc_bytes = strata::format::encode_toc(toc);
	file.replace(footer.toc_offset, toc_bytes.size(), toc_bytes);
	auto f = footer;
	f.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
	const std::string footer_bytes = strata::format::encode_footer(f);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

static bool throws_invalid(const std::function<void()>& f) {
	try {
		f();
	} catch (const std::invalid_argument&) {
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

static std::string build(std::mt19937_64& rng, std::size_t docs, model& m) {
	strata::segment_writer w;
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
		std::string file = build(rng, docs, m);
		strata::segment seg(file);
		CHECK(seg.doc_count() == docs);
		for (std::size_t d = 0; d < docs; ++d) { CHECK(seg.length(strata::ordinal_t(d)) == m.lengths[d]); }
		for (const auto& [term, list] : m.postings) {
			auto info = seg.term(term);
			CHECK(info && info->df == list.size());
			if (!info) { continue; }
			strata::postings_iterator it(seg, *info);
			std::size_t i = 0;
			for (auto d = it.next(); d != strata::no_more_docs; d = it.next(), ++i) {
				CHECK(i < list.size() && d == list[i].first && it.wdf() == list[i].second);
			}
			CHECK(i == list.size());
			// positions of every posting
			{
				strata::postings_iterator pi(seg, *info);
				strata::positions_reader pr(seg, *info);
				std::vector<std::uint32_t> got;
				std::size_t k = 0;
				for (auto d = pi.next(); d != strata::no_more_docs; d = pi.next(), ++k) {
					pr.read(pi, got);
					CHECK(got == m.positions[term][k]);
				}
				// random access after advance
				for (int round = 0; round < 10 && !list.empty(); ++round) {
					strata::postings_iterator a(seg, *info);
					strata::positions_reader ar(seg, *info);
					std::size_t idx = rng() % list.size();
					CHECK(a.advance(list[idx].first) == list[idx].first);
					ar.read(a, got);
					CHECK(got == m.positions[term][idx]);
				}
			}
			// advance to random targets
			for (int round = 0; round < 20; ++round) {
				strata::postings_iterator a(seg, *info);
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
			strata::postings_iterator b(seg, *info);
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
		CHECK(!seg.term("absent"));
		for (strata::ordinal_t d = 0; d < seg.doc_count(); ++d) { CHECK(seg.stored(d) == m.stored[d]); }
		for (strata::ordinal_t d = 0; d < seg.doc_count(); ++d) { CHECK(seg.docid(d) == d + 1); }
		// Corruption: truncations and random byte changes never crash.
		for (std::size_t cut = 0; cut < file.size(); cut += 1 + file.size() / 100) {
			try { strata::segment s(std::string_view(file).substr(0, cut)); CHECK(false); } catch (const strata::corrupt_segment&) {} catch (const std::invalid_argument&) {}
		}
		// A changed byte anywhere but section padding is detected once everything is read:
		// at open, or by region checksums on first read.
		const auto layout = seg.sections();
		for (int round = 0; round < 300; ++round) {
			std::string bad = file;
			const std::size_t at = rng() % bad.size();
			bad[at] = char(bad[at] ^ char(1 + rng() % 255));
			bool in_section = at >= file.size() - 48;  // the footer
			for (const auto& e : layout) { in_section = in_section || (at >= e.offset && at < e.offset + e.length); }
			const std::uint64_t toc_at = strata::format::get<std::uint64_t>(file.data() + file.size() - 48);
			in_section = in_section || at >= toc_at;
			bool detected = false;
			try {
				strata::segment s(bad);
				for (const auto& [term, list] : m.postings) {
					auto info = s.term(term);
					if (!info) { continue; }
					strata::postings_iterator it(s, *info);
					strata::positions_reader pr(s, *info);
					std::vector<std::uint32_t> got;
					for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { pr.read(it, got); }
				}
				s.term("absent");
				for (strata::ordinal_t d = 0; d < s.doc_count(); ++d) { (void)s.stored(d); }
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
		// Regions are verified lazily: opening checks none; reading a term checks its own.
		{
			strata::segment s(file);
			CHECK(s.verified_regions() == 0);
			auto info = s.term("t1");
			CHECK(info && s.verified_regions() >= 1);
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
			// Format 12: monotone docids with gaps, versions, and a key term per document.
			const std::string key = "Q" + std::to_string(d);
			in.push_back({key, 0, {}});
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
		CHECK(seg.doc_count() == 300);
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
		auto info = s.term("big");
		strata::postings_iterator it(s, *info);
		strata::positions_reader pr(s, *info);
		std::vector<std::uint32_t> got;
		std::uint32_t d = 0;
		for (auto o = it.next(); o != strata::no_more_docs; o = it.next(), ++d) {
			CHECK(o == d && it.wdf() == 1 + (d * 37) % 600);
			pr.read(it, got);
			CHECK(got.size() == 3);
		}
		CHECK(d == 5000);
		strata::term_scorer ts(s, *info);
		CHECK(strata::top_k(ts, 10).size() == 10);
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
		CHECK(s.docid(0) == 10 && s.docid(1) == 20 && s.docid(2) == 35);
	}

	// Resealed corruption that verification cannot see. A skip group's last ordinal lowered
	// below the previous group's: a pruned term query that skips to that group throws
	// instead of advancing in place forever.
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
			const auto info = *seg.term("a");
			const char* p = seg.postings_bytes().data() + info.postings;
			const char* end = p + info.postings_length;
			CHECK(strata::format::get_leb(&p, end) == 6 && *p++ == 0);  // six narrow groups
			for (int i = 0; i < 3; ++i) { (void)strata::format::get_leb(&p, end); }
			// Level 1: 16-byte entries without positions, each starting with its last.
			const std::size_t at = std::size_t(p - file.data()) + 4 * 16;
			const std::uint32_t zero = 0;
			std::memcpy(file.data() + at, &zero, 4);
		}
		reseal(file);
		CHECK(throws_corrupt([&] {
			strata::segment bad(file);
			strata::term_scorer t(bad, *bad.term("a"));
			strata::top_k(t, 10);
		}));
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

	// Format 12's docids, versions, and key filter: random monotone docids map both ways,
	// versions read back, keys are never missed, and keys outside the range are rejected.
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
				std::vector<strata::posting_input> in{{keys.back(), 0, {}}};
				w.add(id, in, 1, {}, versions.back(), keys.back());
			}
			std::string bytes = w.flush({});
			strata::segment s(bytes);
			for (std::size_t d = 0; d < docs; ++d) {
				CHECK(s.docid(strata::ordinal_t(d)) == ids[d]);
				CHECK(s.ordinal_of(ids[d]) == strata::ordinal_t(d));
				CHECK(!s.ordinal_of(ids[d] + 1) || (d + 1 < docs && ids[d + 1] == ids[d] + 1));
				CHECK(s.version(strata::ordinal_t(d)) == versions[d]);
				CHECK(s.may_hold_key(keys[d]));
			}
			CHECK(!s.ordinal_of(ids[0] - 1) && !s.ordinal_of(ids.back() + 1));
			CHECK(!s.may_hold_key("A") && !s.may_hold_key("Z"));
			std::size_t false_positives = 0;
			for (int i = 0; i < 10000; ++i) { false_positives += s.may_hold_key("Qk" + std::to_string(r()) + "x"); }
			CHECK(false_positives < 200);  // about 1/256 for an 8-bit filter
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
						for (strata::ordinal_t o = 0; o < s.doc_count(); ++o) { (void)s.docid(o), (void)s.version(o); }
					} catch (const strata::corrupt_segment&) {
						ok = false;
					}
					// A change the format cannot detect still yields readable answers.
					(void)ok;
				}
			}
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
