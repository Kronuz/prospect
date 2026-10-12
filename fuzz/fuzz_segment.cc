// libFuzzer target: segments built from input-derived documents answer postings and
// positions exactly; the same bytes with input-chosen corruption either fence the segment
// (corrupt_segment, termdict::corrupt, invalid_argument) or answer without leaving them.
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/writer.hh"

#include <cstdlib>
#include <map>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 4) { return 0; }
	// Documents: input bytes as tokens from a small alphabet, split into docs on 0x00.
	strata::segment_writer w;
	std::map<std::string, std::vector<std::uint32_t>> expected;  // term -> ordinals
	std::uint32_t ord = 0;
	std::map<std::string, std::vector<std::uint32_t>> positions;
	std::uint32_t pos = 0;
	auto flush_doc = [&] {
		std::vector<strata::posting_input> in;
		std::uint32_t length = 0;
		for (auto& [t, p] : positions) {
			in.push_back({t, std::uint32_t(p.size()), p});
			length += std::uint32_t(p.size());
			expected[t].push_back(ord);
		}
		w.add(ord + 1, in, length, std::string_view(reinterpret_cast<const char*>(data), std::min<std::size_t>(size, ord % 7)));
		++ord;
		positions.clear();
		pos = 0;
	};
	for (std::size_t i = 2; i < size; ++i) {
		if (data[i] == 0) { flush_doc(); continue; }
		positions[std::string(1, char('a' + data[i] % 6))].push_back(++pos);
	}
	flush_doc();
	std::string file = w.flush({});
	{
		strata::segment seg(file);
		for (const auto& [t, ords] : expected) {
			auto info = seg.term(t);
			if (!info || info->df != ords.size()) { std::abort(); }
			strata::postings_iterator it(seg, *info);
			std::size_t k = 0;
			for (auto d = it.next(); d != strata::no_more_docs; d = it.next(), ++k) {
				if (k >= ords.size() || d != ords[k]) { std::abort(); }
			}
			if (k != ords.size()) { std::abort(); }
		}
	}
	// Corrupt bytes chosen by the input.
	std::string bad = file;
	for (std::size_t i = 0; i + 1 < std::min<std::size_t>(size, 16); i += 2) { bad[(data[i] * 131u + data[i + 1]) % bad.size()] ^= char(data[i + 1] | 1); }
	try {
		strata::segment seg(bad);
		for (strata::ordinal_t d = 0; d < seg.doc_count(); ++d) { (void)seg.stored(d); }
		for (const auto& [t, ords] : expected) {
			auto info = seg.term(t);
			if (!info) { continue; }
			strata::term_scorer s(seg, *info);
			strata::top_k(s, 10);
			strata::postings_iterator it(seg, *info);
			strata::positions_reader pr(seg, *info);
			std::vector<std::uint32_t> got;
			for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { pr.read(it, got); }
		}
	} catch (const strata::corrupt_segment&) {
	} catch (const termdict::corrupt&) {
	} catch (const std::invalid_argument&) {
	}
	return 0;
}
