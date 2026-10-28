// libFuzzer target: a damaged values section must fence (corrupt_segment) or answer, never
// crash, read out of bounds, or allocate past the RSS limit. Base segments are built once: one
// small with every kind of slot (dense, sparse, one document, a long arbitrary value), one
// large enough for several skip groups, one with hundreds of slots. The input picks a base and
// byte changes, three in four inside the values section; most inputs then recompute every
// checksum, so the changes reach the directory and column decoders instead of stopping at
// verification. Run with a -timeout of a few seconds so a hang counts as a finding.
#include "prospect/check.hh"
#include "prospect/reader.hh"
#include "prospect/writer.hh"

#include <crc32c/crc32c.hh>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

std::string build(std::uint64_t seed, std::uint32_t docs, std::uint32_t slots) {
	std::mt19937_64 rng(seed);
	prospect::segment_writer w;
	for (std::uint32_t d = 0; d < docs; ++d) {
		const auto ord = w.add(d + 1, {}, 0, d % 7 ? std::string_view() : std::string_view("record"));
		for (std::uint32_t s = 0; s < slots; ++s) {
			const std::uint32_t slot = s * 3 + (s == slots - 1 ? 0x7FFFFF00u : 0);
			const std::uint32_t density = s == 0 ? 100 : s % 5 == 1 ? 1 : s % 3 == 0 ? 60 : 20;
			if (rng() % 100 >= density) { continue; }
			std::string v;
			switch (s % 4) {
			case 0: v = std::to_string(rng() % 9); break;
			case 1: v.assign(1 + rng() % 300, char(rng())); break;
			case 2: v = std::string(rng() % 3, 'x'); break;
			default: v = "value " + std::to_string(rng() % 100000); break;
			}
			w.builder<prospect::values_plugin>().add_value(ord, slot, v);
		}
	}
	return w.flush({});
}

const std::string& base(unsigned which) {
	static const std::string segments[] = {build(1, 400, 6), build(2, 5000, 4), build(3, 300, 120)};
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

void exercise(const prospect::segment& seg) {
	const auto& rd = seg.get<prospect::values_plugin>();
	const std::uint64_t docs = seg.core().doc_count();
	std::mt19937_64 rng(docs + rd.slot_count());
	std::size_t slots = 0;
	try {
		slots = rd.slot_count();
	} catch (const strata::corrupt_segment&) {
	}
	for (std::size_t i = 0; i < slots; i += 1 + i / 16) {  // the first sixteen, then sparser
		try {
			const auto& c = rd.at(i);
			(void)c.documents();
			if (c.distinct()) { (void)c.first(); (void)c.last(); }
			std::uint64_t seen = 0;
			strata::ordinal_t prev = 0;
			auto it = c.begin();
			for (auto o = it.next(); o != strata::no_more_docs && seen < 20000; o = it.next(), ++seen) {
				if (o >= docs || (seen && o <= prev)) { std::abort(); }  // fence or answer (F10)
				prev = o;
				(void)it.value();
			}
			for (int k = 0; k < 8; ++k) {
				auto j = c.begin();
				const auto target = strata::ordinal_t(rng() % (docs + 2));
				const auto o = j.advance(target);
				if (o != strata::no_more_docs && (o < target || o >= docs)) { std::abort(); }
				if (o != strata::no_more_docs) { (void)j.value(); (void)j.next(); }
			}
			for (int k = 0; k < 16 && docs; ++k) { (void)c.value(strata::ordinal_t(rng() % docs)); }
		} catch (const strata::corrupt_segment&) {
		}
	}
	for (int k = 0; k < 8 && docs; ++k) {
		try {
			rd.each_value(strata::ordinal_t(rng() % docs), [](std::uint32_t, std::string_view) {});
			(void)rd.value(strata::ordinal_t(rng() % docs), std::uint32_t(rng()));
		} catch (const strata::corrupt_segment&) {
		}
	}
	if (rng() % 4 == 0) {  // the whole-segment check reads every value: one input in four
		try {
			prospect::check_report r;
			strata::check_segment(seg, nullptr, r);
		} catch (const strata::corrupt_segment&) {
		}
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 2) { return 0; }
	std::string file = base(data[0]);
	const auto sections = strata::format::decode_toc(file, strata::format::decode_footer(file));
	const strata::format::section_entry* values = nullptr;
	for (const auto& e : sections) {
		if (e.kind == strata::format::section::values) { values = &e; }
	}
	if (!values) { std::abort(); }
	for (std::size_t i = 2; i + 4 < size; i += 5) {
		const auto& e = data[i] % 4 ? *values : sections[data[i] % sections.size()];
		if (!e.length) { continue; }
		const std::uint32_t at = std::uint32_t(data[i + 1]) | std::uint32_t(data[i + 2]) << 8 | std::uint32_t(data[i + 3]) << 16;
		file[e.offset + at % e.length] ^= char(data[i + 4] | 1);
	}
	if (data[1] % 8 != 0) { refresh_checksums(file); }  // one input in eight checks detection instead
	static const bool verdicts = std::getenv("PROSPECT_FUZZ_VERDICTS") != nullptr;
	const char* verdict = "accepted";
	try {
		prospect::segment seg(file);
		exercise(seg);
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
