// libFuzzer target: corrupt segments must fence (corrupt_segment, termdict::corrupt,
// unsupported_segment) or answer, never crash, read out of bounds, or allocate past the
// RSS limit. Base segments are built once, large enough for several skip groups, bitset
// and FOR blocks, positions whose counts differ from wdf, positioned wdf-0 terms, and
// stored records past one zstd frame; one base has monotone docids, versions, and keys,
// and one the same in a permuted order (docids kind 2, a reordering merge's output).
// The input picks a base and byte changes inside its sections; most inputs then
// recompute every checksum, so the changes reach the decoders instead of stopping at
// verification. One base uses 64-bit skip offsets. Run with a -timeout of a few seconds
// so a hang counts as a finding.
#include "prospect/check.hh"
#include "prospect/reader.hh"
#include "prospect/search.hh"
#include "prospect/writer.hh"

#include <crc32c/crc32c.hh>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

const char* const vocab[] = {"the", "of", "a", "b", "c", "rare", "bool", "weighted", "nopos"};

// keyed: docids with gaps, versions, and a key per document (the docids, versions, keys,
// and key_index sections); permuted: the same docids in a shuffled order.
std::string build(std::uint64_t seed, std::uint32_t docs, bool wide = false, bool keyed = false, bool permuted = false) {
	std::mt19937_64 rng(seed);
	prospect::segment_writer w;
	w.builder<prospect::inverted_plugin>().wide_skip_offsets = wide;
	w.permuted = permuted;
	// The permuted base's docids come from their own generator, so the other bases keep their bytes.
	std::vector<std::uint64_t> ids;
	if (permuted) {
		std::mt19937_64 id_rng(seed * 7919);
		ids.resize(docs);
		for (std::uint64_t d = 1000; auto& id : ids) { id = d += 1 + (id_rng() % 8 == 0 ? id_rng() % 100000 : id_rng() % 3); }
		std::shuffle(ids.begin(), ids.end(), id_rng);
	}
	std::vector<std::vector<std::uint32_t>> pos(std::size(vocab));
	std::uint64_t docid = 1000;
	for (std::uint32_t d = 0; d < docs; ++d) {
		std::vector<prospect::posting_input> in;
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
		if (!keyed) {
			w.add(d + 1, in, length, rec);
			continue;
		}
		docid = permuted ? ids[d] : docid + 1 + (rng() % 8 == 0 ? rng() % 100000 : rng() % 3);
		const std::string key = "Q" + std::to_string(docid);
		w.add(docid, in, length, rec, rng() % 4 == 0 ? rng() : rng() % 40, key);
	}
	return w.flush({});
}

// First bytes 250-255 pick the permuted base; the others keep their earlier mapping, so
// the checked-in corpus still reaches the bases it was grown on.
const std::string& base(unsigned which) {
	static const std::string segments[] = {build(1, 300), build(2, 9000), build(3, 20000), build(4, 9000, true), build(5, 3000, false, true)};
	static const std::string permuted = build(6, 3000, false, true, true);
	return which >= 250 ? permuted : segments[which % std::size(segments)];
}

// Recomputes the prefix's CRC, every section's checksum and region checksums, then the
// table of contents' and the footer's, so the file passes verification with whatever its
// sections hold.
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

// Recomputes only the table of contents' and the footer's checksums, so a changed table
// (duplicate, overlapping, or out-of-range sections) reaches the reader's table checks.
void refresh_toc_checksum(std::string& file) {
	auto f = strata::format::decode_footer(file);
	f.toc_crc = ::kronuz::crc32c::crc32c(file.data() + f.toc_offset, f.toc_length);
	const std::string footer_bytes = strata::format::encode_footer(f);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

void exercise(const prospect::segment& seg) {
	// Every third ordinal deleted, for find's live bits.
	strata::deleted_bits deleted((seg.core().doc_count() + 63) / 64);
	for (strata::ordinal_t d = 0; d < seg.core().doc_count(); d += 3) { deleted.set(d); }
	const strata::live_bits live{deleted.data()};
	for (strata::ordinal_t d = 0; d < seg.core().doc_count(); d += 1 + d / 64) {
		(void)seg.get<strata::stored_plugin>().stored(d);
		const std::uint64_t id = seg.core().docid(d);
		(void)seg.core().ordinal_of(id);
		(void)seg.core().ordinal_of(id + 1);
		(void)seg.get<strata::keys_plugin>().version(d);
		(void)seg.get<prospect::inverted_plugin>().length(d);
		(void)seg.get<strata::keys_plugin>().may_hold("Q" + std::to_string(id));
		(void)seg.get<strata::keys_plugin>().find("Q" + std::to_string(id), {});  // decodes a key block
		(void)seg.get<strata::keys_plugin>().find("Q" + std::to_string(id), live);
		(void)seg.get<strata::keys_plugin>().find("Q" + std::to_string(id) + "x", live);  // a miss inside the range
	}
	// The keys section's blocks: a scan from a few bounds and a full walk (F13-R37).
	{
		const auto& keys = seg.get<strata::keys_plugin>();
		for (const char* from : {"", "Q", "Q5", "Q9\xff"}) {
			std::size_t n = 0;
			keys.scan(from, [&](std::string_view, strata::ordinal_t) { return ++n < 64; });
		}
		for (auto c = keys.cursor(); c.next();) {}
	}
	try {
		prospect::check_report r;
		strata::check_segment(seg, &deleted, r, "Q");  // every byte, every key against its range and filter (CR2)
	} catch (const strata::corrupt_segment&) {
		// damage found; the query paths below still run over what opened
	}
	std::vector<prospect::format::term_info> infos;
	for (const char* t : vocab) {
		auto info = seg.get<prospect::inverted_plugin>().term(t);
		if (!info) { continue; }
		infos.push_back(*info);
		prospect::postings_iterator it(seg.get<prospect::inverted_plugin>(), *info);
		prospect::positions_reader pr(seg.get<prospect::inverted_plugin>(), *info);
		std::vector<std::uint32_t> got;
		// Fence or answer (F10): every ordinal increases and stays below the count.
		strata::ordinal_t prev = 0;
		bool first = true;
		for (auto d = it.next(); d != strata::no_more_docs; d = it.next()) {
			if (d >= seg.core().doc_count() || (!first && d <= prev)) { std::abort(); }
			prev = d;
			first = false;
			pr.read(it, got);
		}
		prospect::term_scorer s(seg.get<prospect::inverted_plugin>(), *info);
		prospect::top_k(seg, s, 10);
		prospect::term_scorer c(seg.get<prospect::inverted_plugin>(), *info);
		std::uint64_t n = 0;
		prospect::top_k(seg, c, 100, &n);
	}
	(void)seg.get<prospect::inverted_plugin>().term("absent");
	if (infos.size() < 3) { return; }
	auto term = [&](std::size_t i) { return std::make_unique<prospect::term_scorer>(seg.get<prospect::inverted_plugin>(), infos[i % infos.size()]); };
	auto pair = [&](std::size_t i, std::size_t j) {
		std::vector<prospect::scorer_ptr> v;
		v.push_back(term(i));
		v.push_back(term(j));
		return v;
	};
	prospect::conjunction_scorer conj(pair(0, 1));
	prospect::top_k(seg, conj, 10);
	prospect::conjunction_scorer conj_count(pair(0, 2));
	(void)prospect::count(conj_count);
	prospect::disjunction_scorer disj(pair(1, 2));
	prospect::top_k(seg, disj, 10);
	std::vector<prospect::scorer_ptr> three;
	for (std::size_t i = 0; i < 3; ++i) { three.push_back(term(i)); }
	prospect::disjunction_scorer disj_count(std::move(three));
	(void)prospect::count(disj_count);
	prospect::exclusion_scorer excl(term(0), term(2));
	(void)prospect::count(excl);
	prospect::required_optional_scorer req(term(1), term(2));
	prospect::top_k(seg, req, 10);
	prospect::phrase_scorer phrase(seg.get<prospect::inverted_plugin>(), {infos[0], infos[1 % infos.size()], infos[0]});
	prospect::top_k(seg, phrase, 10);
	prospect::phrase_scorer phrase_count(seg.get<prospect::inverted_plugin>(), {infos[2 % infos.size()], infos.back()});
	std::uint64_t n = 0;
	prospect::top_k(seg, phrase_count, 10, &n);
	// Xapian-weighted phrase and NEAR, and a disjunction under pruning parents.
	const std::vector<prospect::format::term_info> two{infos[0], infos[1]};
	const std::vector<prospect::weight> weights{prospect::weight::bm25(seg.get<prospect::inverted_plugin>(), infos[0].df), prospect::weight::bm25(seg.get<prospect::inverted_plugin>(), infos[1].df)};
	prospect::phrase_scorer xapian(seg.get<prospect::inverted_plugin>(), two, weights);
	prospect::top_k(seg, xapian, 10);
	prospect::phrase_scorer near(seg.get<prospect::inverted_plugin>(), two, weights, 4);
	prospect::top_k(seg, near, 10, &n);
	auto either = [&] { return std::make_unique<prospect::disjunction_scorer>(pair(0, 1)); };
	prospect::exclusion_scorer either_not(either(), term(2));
	prospect::top_k(seg, either_not, 10);
	prospect::required_optional_scorer either_maybe(either(), term(2));
	prospect::top_k(seg, either_maybe, 10);
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
	// PROSPECT_FUZZ_VERDICTS prints each input's verdict, to compare two builds' outcomes.
	static const bool verdicts = std::getenv("PROSPECT_FUZZ_VERDICTS") != nullptr;
	const char* verdict = "accepted";
	try {
		prospect::segment seg(file);
		exercise(seg);
	} catch (const strata::corrupt_segment&) {
		verdict = "corrupt_segment";
	} catch (const strata::unsupported_segment&) {
		verdict = "unsupported_segment";  // a resealed version or structure bit this binary lacks: refused (F2, F4)
	} catch (const termdict::corrupt&) {
		verdict = "termdict::corrupt";
	}
	if (verdicts) { std::printf("%s\n", verdict); }
	return 0;
}
