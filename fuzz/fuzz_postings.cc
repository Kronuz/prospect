// libFuzzer target for one term's postings decoders (format-13.md F10, F11, F13): the input
// shapes a small segment's terms (gaps, wdfs, positions, wide offsets), whose postings must
// read back exactly; then it flips bytes inside one term's postings, reseals the checksums,
// and every read path must fence (corrupt_segment) or answer with ordinals increasing and
// below the document count, advance() at or past its target. Inputs are bounded: at most
// 6,000 documents and 64 flips. Aborts on a wrong answer, so a finding is a crash.
#include "prospect/check.hh"
#include "prospect/reader.hh"
#include "prospect/search.hh"
#include "prospect/writer.hh"

#include <crc32c/crc32c.hh>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

namespace f = prospect::format;
using strata::ordinal_t;

struct reader {
	const std::uint8_t* p;
	std::size_t n;
	std::uint8_t byte() {
		if (!n) { return 0; }
		--n;
		return *p++;
	}
	std::uint32_t u16() { return std::uint32_t(byte()) | std::uint32_t(byte()) << 8; }
};

[[noreturn]] void fail(const char* what) {
	std::fprintf(stderr, "fuzz_postings: %s\n", what);
	std::abort();
}

void reseal(std::string& file) {
	const auto footer = f::decode_footer(file);
	auto toc = f::decode_toc(file, footer);
	for (auto& e : toc) { e = f::describe(e.kind, e.offset, std::string_view(file).substr(e.offset, e.length), e.region_shift); }
	const std::string toc_bytes = f::encode_toc(toc);
	file.replace(footer.toc_offset, toc_bytes.size(), toc_bytes);
	auto ft = footer;
	ft.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
	const std::string footer_bytes = f::encode_footer(ft);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

// The read paths over one term; throws corrupt_segment to fence, aborts on a wrong answer.
void walk(const prospect::segment& s, const f::term_info& info) {
	const auto& inv = s.get<prospect::inverted_plugin>();
	const ordinal_t n = s.core().doc_count();
	{
		prospect::postings_iterator it(inv, info);
		prospect::positions_reader pr(inv, info);
		ordinal_t prev = 0;
		bool first = true;
		for (ordinal_t d = it.next(); d != strata::no_more_docs; d = it.next()) {
			if (d >= n || (!first && d <= prev)) { fail("next() out of order or range"); }
			(void)it.wdf();
			(void)pr.read(it);
			prev = d;
			first = false;
		}
	}
	for (const ordinal_t step : {1u, 61u, 700u}) {
		prospect::postings_iterator it(inv, info);
		ordinal_t prev = 0;
		for (ordinal_t t = 0; t < n; t += step) {
			const ordinal_t d = it.advance(t);
			if (d == strata::no_more_docs) { break; }
			if (d < t || d < prev || d >= n) { fail("advance() out of order or range"); }
			prev = d;
		}
	}
	{
		prospect::postings_iterator it(inv, info);
		for (ordinal_t t = 0; t < n; t += 7) {
			const std::size_t b = it.block_index_for(t);
			if (b < it.blocks() && (it.block_last(b) < t || (b && it.block_last(b - 1) >= t))) { fail("a block located outside its target"); }
			(void)it.block_impacts_at(b);
		}
	}
	prospect::term_scorer top(inv, info);
	(void)prospect::top_k(s, top, 10);
	prospect::term_scorer counted(inv, info);
	(void)prospect::count(counted);
}

}  // namespace

// The block decoder on raw bytes: a block they accept holds 128 ordinals strictly
// increasing within (base, last], ending at last.
void raw_blocks(const std::uint8_t* data, std::size_t size) {
	if (size < 9) { return; }
	std::uint32_t base, last, out[f::block];
	std::memcpy(&base, data, 4);
	std::memcpy(&last, data + 4, 4);
	const bool first = data[8] & 1;
	auto valid = [&] {
		for (std::size_t i = 0; i < f::block; ++i) {
			if ((i ? out[i] <= out[i - 1] : (!first && out[0] <= base)) || out[i] > last) { return false; }
		}
		return out[f::block - 1] == last;
	};
	if (f::decode_gaps(data + 9, size - 9, base, last, first, out) && !valid()) { fail("decode_gaps accepted a bad block"); }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	raw_blocks(data, size);
	reader in{data, size};
	const std::uint32_t docs = 1 + in.u16() % 6000;
	const std::uint8_t shape = in.byte();
	const bool wide = shape & 1, positions = shape & 2;
	// Two terms from the input's bytes: each byte is a gap (a run of consecutive documents
	// when its top bit is set), a wdf, and a position count.
	struct posting {
		ordinal_t ord;
		std::uint32_t wdf;
		std::vector<std::uint32_t> pos;
	};
	std::vector<posting> terms[2];
	for (auto& t : terms) {
		ordinal_t d = in.byte() % 8;
		const std::size_t budget = 1 + in.u16() % 4000;
		for (std::size_t k = 0; k < budget && d < docs; ++k) {
			const std::uint8_t g = in.byte();
			const std::uint32_t run = g & 0x80 ? 1 + (g & 0x3f) : 1;
			for (std::uint32_t r = 0; r < run && d < docs; ++r, ++d) {
				posting p{d, std::uint32_t(1 + (g >> 4 & 3)), {}};
				if (positions) {
					for (std::uint32_t i = 0, at = 0; i < 1 + (g & 3); ++i) { p.pos.push_back(at += 1 + i); }
				}
				t.push_back(std::move(p));
			}
			d += (g & 0x80) ? 0 : 1 + (g & 0x7f) * (g & 0x40 ? 97 : 1);
		}
	}
	prospect::segment_writer w;
	w.builder<prospect::inverted_plugin>().wide_skip_offsets = wide;
	std::size_t at[2] = {};
	for (ordinal_t d = 0; d < docs; ++d) {
		std::vector<prospect::posting_input> pin;
		const char* names[2] = {"a", "b"};
		for (int k = 0; k < 2; ++k) {
			if (at[k] < terms[k].size() && terms[k][at[k]].ord == d) {
				const auto& p = terms[k][at[k]++];
				pin.push_back({names[k], positions ? std::uint32_t(p.pos.size()) : p.wdf, p.pos});
			}
		}
		w.add(d + 1, pin, 1 + d % 97);
	}
	std::string file = w.flush({});
	{
		prospect::segment s(file);
		for (int k = 0; k < 2; ++k) {
			const auto info = s.get<prospect::inverted_plugin>().term(k ? "b" : "a");
			if (terms[k].empty() != !info) { fail("a term lost or invented"); }
			if (!info) { continue; }
			prospect::postings_iterator it(s.get<prospect::inverted_plugin>(), *info);
			prospect::positions_reader pr(s.get<prospect::inverted_plugin>(), *info);
			std::size_t i = 0;
			for (ordinal_t d = it.next(); d != strata::no_more_docs; d = it.next(), ++i) {
				if (i >= terms[k].size()) { fail("round trip length"); }
				const auto& p = terms[k][i];
				if (d != p.ord || it.wdf() != (positions ? p.pos.size() : p.wdf)) { fail("round trip"); }
				if (positions) {
					const auto got = pr.read(it);
					if (std::vector<std::uint32_t>(got.begin(), got.end()) != p.pos) { fail("positions round trip"); }
				}
			}
			if (i != terms[k].size()) { fail("round trip length"); }
			// Seeded top-k equals unseeded under both presets (F13).
			const auto& inv = s.get<prospect::inverted_plugin>();
			for (const auto& w : {prospect::weight::bm25(inv, info->df), prospect::weight::bm25_xapian(inv, info->df)}) {
				for (const std::size_t top : {10u, 100u, 1000u}) {
					prospect::term_scorer a(inv, *info, w), b(inv, *info, w);
					const double floor = a.seed(top);
					const auto x = prospect::top_k(s, a, top, nullptr, floor), y = prospect::top_k(s, b, top);
					bool same = x.size() == y.size();
					for (std::size_t h = 0; same && h < x.size(); ++h) { same = x[h].ord == y[h].ord && x[h].score == y[h].score; }
					if (!same) { fail("a seeded top-k differs"); }
				}
			}
			walk(s, *info);
		}
	}
	// Flips inside term a's postings, resealed.
	prospect::segment s0(file);
	const auto info = s0.get<prospect::inverted_plugin>().term("a");
	if (!info || (info->flags & f::single_posting) || !in.n) { return 0; }
	const std::size_t base = std::size_t(s0.get<prospect::inverted_plugin>().postings_bytes().data() - file.data()) + std::size_t(info->postings);
	for (std::size_t k = 0; k < 64 && in.n >= 3; ++k) {
		const std::uint32_t off = in.u16() % std::uint32_t(info->postings_length);
		file[base + off] ^= char(in.byte() | 1);
	}
	reseal(file);
	try {
		prospect::segment s(file);
		walk(s, *info);
		prospect::check_report r;
		strata::check_segment(s, nullptr, r);  // seeds, among the rest
	} catch (const strata::corrupt_segment&) {
	}
	return 0;
}
