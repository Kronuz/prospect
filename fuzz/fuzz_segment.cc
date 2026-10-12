// libFuzzer target: corrupt segments must fence (corrupt_segment, termdict::corrupt,
// invalid_argument) or answer, never crash, read out of bounds, or allocate past the
// RSS limit. Base segments are built once, large enough for several skip groups, bitset
// and FOR blocks, positions whose counts differ from wdf, positioned wdf-0 terms, and
// stored records past one zstd frame. The input picks a base and byte changes inside its
// sections; most inputs then recompute every checksum, so the changes reach the decoders
// instead of stopping at verification.
#include "strata/reader.hh"
#include "strata/search.hh"
#include "strata/writer.hh"

#include <cstdlib>
#include <random>

namespace {

const char* const vocab[] = {"the", "of", "a", "b", "c", "rare", "bool", "weighted", "nopos"};

std::string build(std::uint64_t seed, std::uint32_t docs) {
	std::mt19937_64 rng(seed);
	strata::segment_writer w;
	std::vector<std::vector<std::uint32_t>> pos(std::size(vocab));
	for (std::uint32_t d = 0; d < docs; ++d) {
		std::vector<strata::posting_input> in;
		std::uint32_t at = 0, length = 0;
		for (std::size_t t = 0; t < std::size(vocab); ++t) {
			const std::uint32_t density = t < 2 ? 95 : t < 5 ? 40 : t == 5 ? 2 : 30;
			pos[t].clear();
			if (rng() % 100 >= density) { continue; }
			const std::uint32_t n = 1 + std::uint32_t(rng() % (t == 0 ? 6 : 3));
			for (std::uint32_t k = 0; k < n; ++k) { pos[t].push_back(at += 1 + std::uint32_t(rng() % 3)); }
			const std::uint32_t wdf = vocab[t] == std::string_view("bool") ? 0 : vocab[t] == std::string_view("weighted") ? n * 3 : n;
			if (vocab[t] == std::string_view("nopos")) { pos[t].clear(); }  // indexed without positions
			in.push_back({vocab[t], wdf, pos[t]});
			length += wdf;
		}
		std::string rec(d % 997 == 0 ? 70000 : d % 40, char('a' + d % 26));
		w.add(d + 1, in, length, rec);
	}
	return w.flush({});
}

const std::string& base(unsigned which) {
	static const std::string segments[] = {build(1, 300), build(2, 9000), build(3, 20000)};
	return segments[which % std::size(segments)];
}

// Recomputes every section's checksum and region checksums, then the table of contents'
// and the footer's, so the file passes verification with whatever its sections hold.
void refresh_checksums(std::string& file) {
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

// Recomputes only the table of contents' and the footer's checksums, so a changed table
// (duplicate, overlapping, or out-of-range sections) reaches the reader's table checks.
void refresh_toc_checksum(std::string& file) {
	auto f = strata::format::decode_footer(file);
	f.toc_crc = hashes::crc32c(file.data() + f.toc_offset, f.toc_length);
	const std::string footer_bytes = strata::format::encode_footer(f);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

void exercise(const strata::segment& seg) {
	for (strata::ordinal_t d = 0; d < seg.doc_count(); d += 1 + d / 64) { (void)seg.stored(d); }
	std::vector<strata::format::term_info> infos;
	for (const char* t : vocab) {
		auto info = seg.term(t);
		if (!info) { continue; }
		infos.push_back(*info);
		strata::postings_iterator it(seg, *info);
		strata::positions_reader pr(seg, *info);
		std::vector<std::uint32_t> got;
		for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) { pr.read(it, got); }
		strata::term_scorer s(seg, *info);
		strata::top_k(s, 10);
		strata::term_scorer c(seg, *info);
		std::uint64_t n = 0;
		strata::top_k(c, 100, &n);
	}
	(void)seg.term("absent");
	if (infos.size() < 3) { return; }
	auto term = [&](std::size_t i) { return std::make_unique<strata::term_scorer>(seg, infos[i % infos.size()]); };
	auto pair = [&](std::size_t i, std::size_t j) {
		std::vector<strata::scorer_ptr> v;
		v.push_back(term(i));
		v.push_back(term(j));
		return v;
	};
	strata::conjunction_scorer conj(pair(0, 1));
	strata::top_k(conj, 10);
	strata::conjunction_scorer conj_count(pair(0, 2));
	(void)strata::count(conj_count);
	strata::disjunction_scorer disj(pair(1, 2));
	strata::top_k(disj, 10);
	std::vector<strata::scorer_ptr> three;
	for (std::size_t i = 0; i < 3; ++i) { three.push_back(term(i)); }
	strata::disjunction_scorer disj_count(std::move(three));
	(void)strata::count(disj_count);
	strata::exclusion_scorer excl(term(0), term(2));
	(void)strata::count(excl);
	strata::required_optional_scorer req(term(1), term(2));
	strata::top_k(req, 10);
	strata::phrase_scorer phrase(seg, {infos[0], infos[1 % infos.size()], infos[0]});
	strata::top_k(phrase, 10);
	strata::phrase_scorer phrase_count(seg, {infos[2 % infos.size()], infos.back()});
	std::uint64_t n = 0;
	strata::top_k(phrase_count, 10, &n);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 2) { return 0; }
	std::string file = base(data[0]);
	const auto sections = strata::format::decode_toc(file, strata::format::decode_footer(file));
	// Byte changes: (section, position, xor) quintuples, positions scaled to the section.
	for (std::size_t i = 2; data[1] % 8 != 1 && i + 4 < size; i += 5) {
		const auto& e = sections[data[i] % sections.size()];
		if (!e.length) { continue; }
		const std::uint32_t at = std::uint32_t(data[i + 1]) | std::uint32_t(data[i + 2]) << 8 | std::uint32_t(data[i + 3]) << 16;
		file[e.offset + at % e.length] ^= char(data[i + 4] | 1);
	}
	if (data[1] % 8 == 1) {
		// Change the table of contents itself instead, keeping its checksum valid.
		const auto f = strata::format::decode_footer(file);
		for (std::size_t i = 2; i + 2 < size; i += 3) { file[f.toc_offset + (data[i] * 256u + data[i + 1]) % f.toc_length] ^= char(data[i + 2] | 1); }
		refresh_toc_checksum(file);
	} else if (data[1] % 8 != 0) {
		refresh_checksums(file);  // one input in eight checks detection instead
	}
	try {
		strata::segment seg(file);
		exercise(seg);
	} catch (const strata::corrupt_segment&) {
	} catch (const termdict::corrupt&) {
	} catch (const std::invalid_argument&) {
	}
	return 0;
}
