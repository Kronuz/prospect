// libFuzzer target: a damaged field dictionary (the `terms` and `term_index` sections: the
// sampled directory, the own dictionaries and the pool) must fence (corrupt_segment) or answer,
// never crash, read out of bounds, loop, or allocate past the RSS limit. Two modes by the first
// byte: the reader over the raw section bytes of a dictionary built directly (no checksums, so
// every byte reaches the decoders), and a whole segment whose two sections changed, most inputs
// with every checksum recomputed. Base dictionaries have fields of many sizes (own and pooled),
// the empty term, ids on both sides of every varint width, and enough fields for several
// directory samples. Run with a -timeout of a few seconds so a hang counts as a finding.
#include "prospect/check.hh"
#include "prospect/fielddict.hh"
#include "prospect/reader.hh"
#include "prospect/writer.hh"

#include <algorithm>
#include <crc32c/crc32c.hh>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>

namespace {

namespace fd = prospect::fielddict;

// (field, term) pairs in order: sizes on both sides of the own threshold, the empty term, and
// ids 0, 1, 239, 240, 255, 256, 65535, 65536, 70000 and a run of small ones.
const std::map<std::uint32_t, std::vector<std::string>>& base_terms(unsigned which) {
	static const std::map<std::uint32_t, std::vector<std::string>> bases[] = {
		[] {
			std::map<std::uint32_t, std::vector<std::string>> m;
			std::mt19937_64 rng(1);
			const std::uint32_t ids[] = {0, 1, 7, 239, 240, 255, 256, 65535, 65536, 70000, 0x01000000, 0xFFFFFFFFu};
			const std::size_t sizes[] = {400, 5, 255, 256, 3, 1, 300, 2, 40, 1, 2, 4};
			for (std::size_t k = 0; k < std::size(ids); ++k) {
				std::set<std::string> terms;
				if (k % 3 == 1) { terms.insert(""); }
				while (terms.size() < sizes[k]) {
					std::string t(rng() % 12, 'a');
					for (auto& c : t) { c = char(k % 4 == 3 ? rng() : 'a' + rng() % 26); }
					terms.insert(t);
				}
				m[ids[k]].assign(terms.begin(), terms.end());
			}
			return m;
		}(),
		[] {
			std::map<std::uint32_t, std::vector<std::string>> m;  // many small fields: several samples, one pool
			for (std::uint32_t f = 0; f < 700; f += 1 + f % 3) {
				for (std::uint32_t t = 0; t < 1 + f % 4; ++t) { m[f].push_back(std::string(1, char('a' + t)) + std::to_string(f)); }
				std::sort(m[f].begin(), m[f].end());
			}
			for (std::uint32_t f = 700; f < 740; ++f) {  // and fields of their own among them
				for (std::uint32_t t = 0; t < 300; ++t) { m[f * 7].push_back("w" + std::to_string(1000 + t)); }
			}
			return m;
		}(),
	};
	return bases[which % std::size(bases)];
}

prospect::format::term_info info_of(std::uint32_t n) {
	prospect::format::term_info t;
	t.flags = prospect::format::single_posting | prospect::format::no_wdf | prospect::format::no_positions;
	t.df = 1;
	t.cf = 0;
	t.postings = n % 50;
	return t;
}

fd::builder::built build_dictionary(const std::map<std::uint32_t, std::vector<std::string>>& terms) {
	fd::planner plan;
	for (const auto& [f, ts] : terms) {
		for (const auto& t : ts) { plan.add(f, t); }
	}
	fd::builder b(plan.finish());
	std::uint32_t n = 0;
	for (const auto& [f, ts] : terms) {
		for (const auto& t : ts) { b.add(f, t, info_of(n++)); }
	}
	return b.finish();
}

void exercise(const fd::reader& r, std::mt19937_64& rng) {
	try {
		std::uint64_t seen = 0;
		fd::reader::cursor c(r);
		std::uint32_t pf = 0;
		std::string pt;
		while (c.next() && seen < 100000) {
			if (seen && (c.field() < pf || (c.field() == pf && c.term() <= pt))) { std::abort(); }  // fence or answer: terms ascend
			pf = c.field();
			pt = std::string(c.term());
			++seen;
		}
	} catch (const strata::corrupt_segment&) {
	}
	for (int k = 0; k < 24; ++k) {
		const std::uint32_t field = k % 3 == 0 ? std::uint32_t(rng()) : k % 3 == 1 ? std::uint32_t(rng() % 800) : std::uint32_t(rng() % 3 ? 0 : 70000);
		try {
			(void)r.entry_of(field);
			(void)r.has_field(field);
			(void)r.find_run(field, "", [](std::string_view) {});
			std::string term(rng() % 6, 'w');
			for (auto& ch : term) { ch = char(rng()); }
			(void)r.find_run(field, term, [](std::string_view) {});
			std::size_t n = 0;
			r.scan(field, term, [&](std::string_view, std::string_view) { return ++n < 50; });
		} catch (const strata::corrupt_segment&) {
		}
	}
	try {
		r.check();
		std::map<std::uint32_t, fd::field_plan> plan;
		r.add_plan(plan);
	} catch (const strata::corrupt_segment&) {
	}
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

// A segment whose documents carry the base's terms: one document per term, each in its field.
const std::string& base_segment(unsigned which) {
	static const std::string segments[] = {
		[] {
			prospect::segment_writer w;
			std::uint64_t docid = 0;
			for (const auto& [f, ts] : base_terms(0)) {
				for (const auto& t : ts) {
					const prospect::posting_input in{t, 1, {}, f};
					prospect::detail::writer_access::add_texts(w, ++docid, std::span<const prospect::posting_input>(&in, 1), {}, 1, {}, 0, {});
				}
			}
			return w.flush({});
		}(),
	};
	return segments[which % std::size(segments)];
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 3) { return 0; }
	static const bool verdicts = std::getenv("PROSPECT_FUZZ_VERDICTS") != nullptr;
	const char* verdict = "accepted";
	std::mt19937_64 rng(size * 131 + data[2]);
	try {
		if (data[0] & 0x80) {
			auto built = build_dictionary(base_terms(data[0]));
			std::string* both[] = {&built.blocks, &built.index};
			for (std::size_t i = 3; i + 4 < size; i += 5) {
				std::string& s = *both[data[i] & 1];
				if (s.empty()) { continue; }
				const std::uint32_t at = std::uint32_t(data[i + 1]) | std::uint32_t(data[i + 2]) << 8 | std::uint32_t(data[i + 3]) << 16;
				s[at % s.size()] ^= char(data[i + 4] | 1);
			}
			if (data[1] % 11 == 0 && !built.index.empty()) { built.index.resize(std::size_t(data[2]) * built.index.size() / 256); }  // truncated
			const fd::reader r(built.blocks, built.index);
			exercise(r, rng);
		} else {
			std::string file = base_segment(data[0]);
			const auto sections = strata::format::decode_toc(file, strata::format::decode_footer(file));
			const strata::format::section_entry* picked[2] = {nullptr, nullptr};
			for (const auto& e : sections) {
				if (e.kind == strata::format::section::terms) { picked[0] = &e; }
				if (e.kind == strata::format::section::term_index) { picked[1] = &e; }
			}
			if (!picked[0] || !picked[1]) { std::abort(); }
			for (std::size_t i = 3; i + 4 < size; i += 5) {
				const auto& e = *picked[data[i] & 1];
				if (!e.length) { continue; }
				const std::uint32_t at = std::uint32_t(data[i + 1]) | std::uint32_t(data[i + 2]) << 8 | std::uint32_t(data[i + 3]) << 16;
				file[e.offset + at % e.length] ^= char(data[i + 4] | 1);
			}
			if (data[1] % 8 != 0) { refresh_checksums(file); }
			prospect::segment seg(file);
			const auto& inv = seg.get<prospect::inverted_plugin>();
			exercise(inv.dictionary(), rng);
			if (rng() % 4 == 0) {
				prospect::check_report rep;
				strata::check_segment(seg, nullptr, rep);
			}
		}
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
