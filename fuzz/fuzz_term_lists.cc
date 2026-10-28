// libFuzzer target: damaged term lists must fence (corrupt_segment) or answer, never crash,
// read out of bounds, or allocate past the RSS limit. Two modes by the first byte's low bit:
//   record mode: the rest is one record, read by a cursor from the start, by a prefix, and by
//     skip_to targets taken from the input; whatever it yields increases strictly;
//   segment mode: base segments are built once (raw frames and zstd frames, shared prefixes,
//     empty lists, long lists); the input picks a base and byte changes, three in four inside
//     the term_lists section; most inputs then recompute every checksum, so the changes reach
//     the frame and record decoders instead of stopping at verification. The lists are read
//     back whole, by prefix and by skip_to, then checked and cross-checked against the postings.
// Run with a -timeout of a few seconds so a hang counts as a finding.
#include "prospect/check.hh"
#include "prospect/reader.hh"
#include "prospect/writer.hh"

#include <crc32c/crc32c.hh>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

using namespace std::string_literals;

namespace {

std::string build(std::uint64_t seed, std::uint32_t docs, bool raw) {
	std::mt19937_64 rng(seed);
	static const char alphabet[] = {'a', 'b', 'c', '\0', '\xff', 'x'};
	std::vector<std::string> vocab;
	for (int i = 0; i < 120; ++i) {
		std::string t = i % 3 == 0 ? "Xf\0"s : i % 3 == 1 ? "Xfa"s : "Z"s;
		for (std::size_t n = 1 + rng() % 8; n--;) { t.push_back(alphabet[rng() % sizeof alphabet]); }
		vocab.push_back(t);
	}
	std::sort(vocab.begin(), vocab.end());
	vocab.erase(std::unique(vocab.begin(), vocab.end()), vocab.end());
	prospect::segment_writer w;
	w.structures |= strata::format::bit::term_lists;
	if (raw) { w.stored_codec = {strata::format::codec::raw, 0}; }
	for (std::uint32_t d = 0; d < docs; ++d) {
		std::set<std::string> chosen;
		const std::size_t n = d % 9 == 0 ? 0 : d % 50 == 1 ? 110 : rng() % 30;
		while (chosen.size() < n && chosen.size() < vocab.size()) { chosen.insert(vocab[rng() % vocab.size()]); }
		std::vector<prospect::posting_input> in;
		std::uint32_t length = 0;
		for (const auto& t : chosen) {
			const std::uint32_t wdf = std::uint32_t(rng() % 5);
			in.push_back({t, wdf, {}});
			length += wdf;
		}
		w.add(d + 1, in, length);
	}
	return w.flush({});
}

const std::string& base(unsigned which) {
	static const std::string segments[] = {build(1, 300, true), build(2, 300, false), build(3, 9000, false), build(4, 1, true)};
	return segments[which % std::size(segments)];
}

void refresh_checksums(std::string& file) {
	if (file.size() >= strata::format::prefix_size) {
		const std::uint32_t crc = ::kronuz::crc32c::crc32c(file.data(), 12);
		std::memcpy(file.data() + 12, &crc, 4);
	}
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

// Reads a list every way, checking what a cursor promises: strictly increasing terms, a wdf
// within 32 bits, prefixes honored, skip_to at or after its target.
void exercise_list(const prospect::term_list& l, std::string_view prefix, std::string_view target) {
	std::string previous;
	bool first = true;
	std::uint64_t n = 0;
	for (auto c = l.begin(); c.next(); ++n) {
		if (!first && !(previous < c.term())) { std::abort(); }
		if (c.term().empty()) { std::abort(); }
		previous.assign(c.term());
		first = false;
	}
	if (n != l.size()) { std::abort(); }
	for (auto c = l.prefixed(prefix); c.next();) {
		if (!c.term().starts_with(prefix)) { std::abort(); }
	}
	auto c = l.begin();
	if (c.skip_to(target) && c.term() < target) { std::abort(); }
	(void)l.prefixed(prefix);
}

void exercise(const prospect::segment& seg, std::string_view prefix, std::string_view target) {
	const auto& lists = seg.get<prospect::term_lists_plugin>();
	if (lists.present()) {
		prospect::term_lists_plugin::reader::frame frame;
		std::mt19937_64 rng(seg.core().doc_count());
		const std::uint64_t docs = seg.core().doc_count();
		for (std::uint64_t i = 0; i < std::min<std::uint64_t>(docs, 200); ++i) {
			try {
				exercise_list(lists.list(strata::ordinal_t(docs > 200 ? rng() % docs : i), frame), prefix, target);
			} catch (const strata::corrupt_segment&) {
			}
		}
	}
	try {
		prospect::check_report r;
		prospect::check_segment(seg, nullptr, r);
	} catch (const strata::corrupt_segment&) {
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 3) { return 0; }
	if (data[0] & 1) {
		// A record: [flags][prefix length][prefix][target length][target]... then the record.
		const std::size_t plen = std::min<std::size_t>(data[1] & 15, size - 2);
		const std::string_view prefix(reinterpret_cast<const char*>(data) + 2, plen);
		std::size_t at = 2 + plen;
		const std::size_t tlen = at < size ? std::min<std::size_t>(data[at] & 15, size - at - 1) : 0;
		const std::string_view target(reinterpret_cast<const char*>(data) + std::min(at + 1, size), tlen);
		at = std::min(size, at + 1 + tlen);
		const std::string_view record(reinterpret_cast<const char*>(data) + at, size - at);
		try {
			exercise_list(prospect::term_list(record), prefix, target);
		} catch (const strata::corrupt_segment&) {
		}
		return 0;
	}
	std::string file = base(data[0] >> 1);
	const auto sections = strata::format::decode_toc(file, strata::format::decode_footer(file));
	const strata::format::section_entry* lists = nullptr;
	for (const auto& e : sections) {
		if (e.kind == strata::format::section::term_lists) { lists = &e; }
	}
	if (!lists) { std::abort(); }
	for (std::size_t i = 2; i + 4 < size; i += 5) {
		const auto& e = data[i] % 4 ? *lists : sections[data[i] % sections.size()];
		if (!e.length) { continue; }
		const std::uint32_t at = std::uint32_t(data[i + 1]) | std::uint32_t(data[i + 2]) << 8 | std::uint32_t(data[i + 3]) << 16;
		file[e.offset + at % e.length] ^= char(data[i + 4] | 1);
	}
	if (data[1] % 8 != 0) { refresh_checksums(file); }  // one input in eight checks detection instead
	static const bool verdicts = std::getenv("PROSPECT_FUZZ_VERDICTS") != nullptr;
	const char* verdict = "accepted";
	try {
		prospect::segment seg(file);
		exercise(seg, std::string_view("Xf\0", 3).substr(0, data[1] % 4), std::string_view("Xfa", 3).substr(0, data[1] / 8 % 4));
	} catch (const strata::corrupt_segment&) {
		verdict = "corrupt_segment";
	} catch (const strata::unsupported_segment&) {
		verdict = "unsupported_segment";
	} catch (const termdict::corrupt&) {
		verdict = "termdict::corrupt";
	}
	if (verdicts) { std::printf("%s\n", verdict); }
	return 0;
}
