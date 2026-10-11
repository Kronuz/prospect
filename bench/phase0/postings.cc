// Inverts the search-benchmark-game corpus into per-term doc-id deltas and term
// frequencies, then measures how they encode:
//   - full 128-entry blocks with bitpack (FOR for doc deltas, PFOR for frequencies)
//   - tails (< 128 entries, most terms' whole list) with varint, LEB128, and StreamVByte
// It also writes the sorted vocabulary, one term per line, for the fst benchmark.
//
//   postings CORPUS_JSON VOCAB_OUT [MAX_DOCS]
#include "bitpack.hh"
#include "varint.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct postings {
	std::vector<std::uint32_t> docs, freqs;
};

void put_leb128(std::string& out, std::uint32_t v) {
	while (v >= 0x80) {
		out.push_back(char(v | 0x80));
		v >>= 7;
	}
	out.push_back(char(v));
}

std::uint32_t get_leb128(const unsigned char*& p) {
	std::uint32_t v = 0;
	for (unsigned shift = 0;; shift += 7) {
		unsigned char b = *p++;
		v |= std::uint32_t(b & 0x7f) << shift;
		if (!(b & 0x80)) { return v; }
	}
}

// StreamVByte: 2-bit length codes for four values per control byte, then the bytes.
void put_streamvbyte(std::string& out, const std::uint32_t* v, std::size_t n) {
	std::string control((n + 3) / 4, '\0'), data;
	for (std::size_t i = 0; i < n; ++i) {
		unsigned len = v[i] < (1u << 8) ? 1 : v[i] < (1u << 16) ? 2 : v[i] < (1u << 24) ? 3 : 4;
		control[i / 4] = char(control[i / 4] | ((len - 1) << (2 * (i % 4))));
		for (unsigned b = 0; b < len; ++b) { data.push_back(char(v[i] >> (8 * b))); }
	}
	out += control;
	out += data;
}

std::string_view text_field(std::string_view line) {
	auto at = line.find("\"text\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 9;
	return line.substr(at, line.find('"', at) - at);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: postings CORPUS_JSON VOCAB_OUT [MAX_DOCS]\n");
		return 2;
	}
	std::uint64_t max_docs = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : ~0ull;
	std::ifstream in(argv[1]);
	std::unordered_map<std::string, std::uint32_t> ids;
	std::vector<postings> lists;
	std::unordered_map<std::uint32_t, std::uint32_t> doc_freqs;
	std::string line;
	std::uint32_t doc = 0;
	auto t0 = std::chrono::steady_clock::now();
	while (doc < max_docs && std::getline(in, line)) {
		if (line.empty()) { continue; }
		doc_freqs.clear();
		std::string_view text = text_field(line);
		for (std::size_t i = 0; i < text.size();) {
			std::size_t j = text.find(' ', i);
			if (j == std::string_view::npos) { j = text.size(); }
			if (j > i) {
				auto [it, fresh] = ids.try_emplace(std::string(text.substr(i, j - i)), std::uint32_t(lists.size()));
				if (fresh) { lists.emplace_back(); }
				++doc_freqs[it->second];
			}
			i = j + 1;
		}
		for (auto [term, freq] : doc_freqs) {
			lists[term].docs.push_back(doc);
			lists[term].freqs.push_back(freq);
		}
		++doc;
	}
	double invert_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

	{
		std::vector<std::string_view> vocab;
		vocab.reserve(ids.size());
		for (const auto& [term, id] : ids) { vocab.push_back(term); }
		std::sort(vocab.begin(), vocab.end());
		std::ofstream out(argv[2]);
		for (auto term : vocab) { out << term << '\n'; }
	}

	std::uint64_t entries = 0, tail_entries = 0, block_entries = 0;
	std::uint64_t block_doc_bytes = 0, block_freq_bytes = 0;
	std::string tails_varint, tails_leb, tails_svb;
	std::vector<std::uint32_t> tail_values;  // doc deltas then freqs, for decode timing
	std::uint8_t buf[bitpack::max_encoded_bytes];
	std::array<std::uint32_t, 128> deltas{};
	for (const auto& p : lists) {
		entries += p.docs.size();
		std::size_t full = p.docs.size() / 128 * 128;
		std::uint32_t base = 0;
		for (std::size_t b = 0; b < full; b += 128) {
			bitpack::delta_encode(base, p.docs.data() + b, deltas.data());
			if (b == 0) { deltas[0] = p.docs[0]; }  // first doc is absolute
			block_doc_bytes += bitpack::encode_for(deltas.data(), buf);
			block_freq_bytes += bitpack::encode_pfor(p.freqs.data() + b, buf);
			base = p.docs[b + 127];
		}
		block_entries += full;
		std::size_t n = p.docs.size() - full;
		tail_entries += n;
		std::vector<std::uint32_t> tail;
		for (std::size_t i = full; i < p.docs.size(); ++i) {
			tail.push_back(p.docs[i] - (i == 0 ? 0 : p.docs[i - 1]));
		}
		for (std::size_t i = full; i < p.docs.size(); ++i) { tail.push_back(p.freqs[i] - 1); }  // freqs >= 1
		for (auto v : tail) {
			char b[varint::max_length_bytes];
			tails_varint.append(b, varint::serialise_length(b, v));
			put_leb128(tails_leb, v);
		}
		put_streamvbyte(tails_svb, tail.data(), tail.size());
		if (tail_values.size() < 50'000'000) { tail_values.insert(tail_values.end(), tail.begin(), tail.end()); }
	}

	// Decode speed over the collected tail values (one stream, sequential).
	std::string leb_sample, varint_sample;
	for (auto v : tail_values) {
		put_leb128(leb_sample, v);
		char b[varint::max_length_bytes];
		varint_sample.append(b, varint::serialise_length(b, v));
	}
	volatile std::uint64_t sink = 0;
	auto time_ns = [&](auto&& f) {
		double best = 1e300;
		for (int t = 0; t < 5; ++t) {
			auto s = std::chrono::steady_clock::now();
			f();
			best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - s).count());
		}
		return best / double(tail_values.size());
	};
	double leb_ns = time_ns([&] {
		const auto* p = reinterpret_cast<const unsigned char*>(leb_sample.data());
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < tail_values.size(); ++i) { s += get_leb128(p); }
		sink = sink + s;
	});
	double varint_ns = time_ns([&] {
		const char* p = varint_sample.data();
		const char* end = p + varint_sample.size();
		std::uint64_t s = 0, v = 0;
		for (std::size_t i = 0; i < tail_values.size(); ++i) {
			varint::unserialise_length(&p, end, v);
			s += v;
		}
		sink = sink + s;
	});

	std::printf("docs %u, terms %zu, postings %llu, inversion %.1f s\n", doc, lists.size(), (unsigned long long)entries, invert_s);
	std::printf("block postings: %llu entries (%.1f%%), doc deltas %llu B (%.3f B/entry), freqs %llu B (%.3f B/entry)\n",
		(unsigned long long)block_entries, 100.0 * double(block_entries) / double(entries), (unsigned long long)block_doc_bytes,
		double(block_doc_bytes) / double(block_entries), (unsigned long long)block_freq_bytes, double(block_freq_bytes) / double(block_entries));
	std::printf("tail postings: %llu entries (%.1f%%)\n", (unsigned long long)tail_entries, 100.0 * double(tail_entries) / double(entries));
	std::printf("  %-12s %14s %12s %14s\n", "codec", "bytes", "B/value", "decode ns/value");
	std::printf("  %-12s %14zu %12.3f %14.2f\n", "varint", tails_varint.size(), double(tails_varint.size()) / double(2 * tail_entries), varint_ns);
	std::printf("  %-12s %14zu %12.3f %14.2f\n", "LEB128", tails_leb.size(), double(tails_leb.size()) / double(2 * tail_entries), leb_ns);
	std::printf("  %-12s %14zu %12.3f %14s\n", "StreamVByte", tails_svb.size(), double(tails_svb.size()) / double(2 * tail_entries), "-");
	std::printf("total doc+freq bytes with varint tails: %llu\n", (unsigned long long)(block_doc_bytes + block_freq_bytes + tails_varint.size()));
}
