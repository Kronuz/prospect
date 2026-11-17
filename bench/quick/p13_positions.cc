// P13 offline: what each positions candidate costs in bytes over a real segment, and how fast
// its block decodes. Not built by CMake; compile with prospect_engine's flags and includes:
//   clang++ -std=gnu++20 -O3 -march=native -DNDEBUG $(includes) bench/quick/p13_positions.cc
//
//   p13_positions sizes SEGDIR     bytes per candidate over every position of SEGDIR/segment.sx
//   p13_positions decode SEGDIR N|FILE  ns per position decoding the N most frequent terms, or FILE's
//
// The stored stream is today's: per document the first position absolute, then gaps, cut into
// 128-value blocks across documents, with a LEB128 tail of fewer than 128 values per term.

#include "bitpack.hh"
#include "prospect/reader.hh"
#include "residency.hh"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr std::size_t B = 128;

u64 leb_bytes(u64 v) {
	u64 n = 1;
	while (v >= 128) { v >>= 7, ++n; }
	return n;
}

// Elias-Fano over the block's prefix sums S_i: [header u8: 0xC0 | l][zeros LEB128][lows packed
// at l, bitpack's layout][highs: 128 ones and `zeros` zeros, unary, ceil to bytes]. The low
// width l minimizing the size; zeros = S_127 >> l.
u64 ef_bytes(const u32* v, unsigned* lout = nullptr) {
	u64 s = 0;
	for (std::size_t i = 0; i < B; ++i) { s += v[i]; }
	u64 best = ~u64(0);
	for (unsigned l = 0; l <= 32; ++l) {
		const u64 zeros = s >> l, size = 1 + leb_bytes(zeros) + bitpack::packed_bytes(l) + (B + zeros + 7) / 8;
		if (size < best) { best = size, *(lout ? lout : &l) = l; }
		if (lout == nullptr && size > best) { break; }
	}
	return best;
}

struct ef_block {
	unsigned l = 0;
	u64 zeros = 0;
	u32 low[B];
	std::vector<u64> high;
};

std::size_t ef_encode(const u32* v, std::uint8_t* out) {
	unsigned l = 0;
	ef_bytes(v, &l);
	u32 low[B];
	u64 s = 0;
	std::vector<u64> high;
	const u32 mask = l == 32 ? ~0u : (1u << l) - 1;
	u64 sums[B];
	for (std::size_t i = 0; i < B; ++i) { sums[i] = s += v[i], low[i] = u32(sums[i]) & mask; }
	const u64 zeros = s >> l, bits = B + zeros;
	high.assign((bits + 63) / 64, 0);
	for (std::size_t i = 0; i < B; ++i) {
		const u64 at = (sums[i] >> l) + i;
		high[at / 64] |= u64(1) << (at % 64);
	}
	std::uint8_t* p = out;
	*p++ = std::uint8_t(0xC0 | l);
	for (u64 z = zeros; ; z >>= 7) {
		*p++ = std::uint8_t((z & 127) | (z >= 128 ? 128 : 0));
		if (z < 128) { break; }
	}
	bitpack::pack(low, l, p);
	p += bitpack::packed_bytes(l);
	std::memcpy(p, high.data(), (bits + 7) / 8);
	p += (bits + 7) / 8;
	return std::size_t(p - out);
}

// For each byte value, the indexes of its set bits (padded) and how many there are.
struct byte_bits {
	std::uint8_t at[256][8];
	std::uint8_t n[256];
	constexpr byte_bits() : at{}, n{} {
		for (unsigned v = 0; v < 256; ++v) {
			std::uint8_t k = 0;
			for (unsigned b = 0; b < 8; ++b) {
				if (v >> b & 1) { at[v][k++] = std::uint8_t(b); }
			}
			n[v] = k;
		}
	}
};
inline constexpr byte_bits bits_of{};

// Decodes ef_encode's block back to the stream values (each S_i - S_{i-1}): the lows by
// bitpack's SIMD unpack, the highs' set bits a byte at a time from a table (eight stores, no
// branch per bit), then S_i = (at_i - i) << l | low_i and the differences.
std::size_t ef_decode(const std::uint8_t* in, u32* out) {
	const std::uint8_t* p = in;
	const unsigned l = *p++ & 0x3f;
	u64 zeros = 0;
	for (unsigned sh = 0; ; sh += 7) {
		const std::uint8_t b = *p++;
		zeros |= u64(b & 127) << sh;
		if (b < 128) { break; }
	}
	u32 low[B], at[B + 8];
	bitpack::unpack(p, l, low);
	p += bitpack::packed_bytes(l);
	const u64 bits = B + zeros, bytes = (bits + 7) / 8;
	u32 i = 0;
	for (u64 k = 0; k < bytes && i < B; ++k) {
		const std::uint8_t v = p[k];
		const u32 base = u32(8 * k);
		for (unsigned j = 0; j < 8; ++j) { at[i + j] = base + bits_of.at[v][j]; }
		i += bits_of.n[v];
	}
	u32 sum[B + 1];
	sum[0] = 0;
	for (u32 j = 0; j < B; ++j) { sum[j + 1] = (at[j] - j) << l | low[j]; }
	for (u32 j = 0; j < B; ++j) { out[j] = sum[j + 1] - sum[j]; }
	return std::size_t(p - in) + bytes;
}

// Binary interpolative coding of a sorted set in [lo, hi]: the bits of the minimal binary code of
// each middle element over its feasible range.
u64 bic_bits(const u32* a, std::size_t n, u64 lo, u64 hi) {
	if (n == 0) { return 0; }
	const std::size_t m = n / 2;
	const u64 range = (hi - lo + 1) - (n - 1);  // values a[m] can take
	const u64 bits = range <= 1 ? 0 : u64(std::bit_width(range - 1));
	return bits + bic_bits(a, m, lo, a[m] - 1) + bic_bits(a + m + 1, n - m - 1, u64(a[m]) + 1, hi);
}

struct segment_view {
	residency::file file;
	prospect::segment seg;
	explicit segment_view(const std::string& dir)
		: file(residency::file::open(dir + "/segment.sx")), seg([&] {
			  auto bytes = file.view({0, file.size()});
			  return prospect::segment(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		  }()) {}
};

// Every positional term's stored stream, with each document's (length, count).
template <class F>
void each_term(const prospect::inverted_plugin::reader& r, F&& f) {
	std::vector<u32> stream, lengths, counts, pos;
	r.scan_terms([&](u32, std::string_view term, const prospect::format::term_info& info) {
		if (info.flags & prospect::format::no_positions) { return true; }
		stream.clear(), lengths.clear(), counts.clear();
		prospect::postings_iterator it(r, info);
		prospect::positions_reader pr(r, info);
		for (auto o = it.next(); o != prospect::no_more_docs; o = it.next()) {
			pr.read(it, pos);
			lengths.push_back(r.length(o));
			counts.push_back(u32(pos.size()));
			u32 prev = 0;
			for (u32 p : pos) { stream.push_back(p - prev), prev = p; }
		}
		f(term, info, stream, lengths, counts);
		return true;
	});
}

int sizes(const std::string& dir) {
	segment_view sv(dir);
	const auto& r = sv.seg.get<prospect::inverted_plugin>();
	std::uint8_t buf[bitpack::max_encoded_bytes + 64];
	u64 positions = 0, full = 0, tail_leb = 0, pfor = 0, forb = 0, ef = 0, best = 0, ef_blocks = 0, blocks = 0, exc_blocks = 0;
	u64 firsts = 0, bic = 0, outside = 0, docs = 0, firsts_bw = 0;
	// Per stream of the split (0 firsts, 1 gaps): PFOR blocks, min(PFOR, EF) blocks, the LEB128 tail.
	u64 sp_pfor[2] = {}, sp_best[2] = {}, sp_tail[2] = {};
	// Rice codes with the parameter from the document: k = bit_width(length / (count + 1)) - 1 + delta.
	u64 rice_f[3] = {}, rice_g[3] = {};
	u64 counts_hist[5] = {};
	// min(PFOR, EF) with EF taken only when it saves at least margins[m] percent of the block.
	constexpr unsigned margins[4] = {0, 5, 10, 20};
	u64 marg_bytes[4] = {}, marg_ef[4] = {};
	u64 m1_pfor = 0, m1_best = 0, m1_tail = 0;  // gaps stored minus one (firsts as they are)
	std::vector<u32> m1;
	std::vector<u32> split_v[2], pos;
	each_term(r, [&](std::string_view, const prospect::format::term_info&, const std::vector<u32>& s, const std::vector<u32>& lengths, const std::vector<u32>& counts) {
		positions += s.size();
		const std::size_t nf = s.size() / B * B;
		for (std::size_t b = 0; b < nf; b += B) {
			const u64 f = bitpack::encode_for(s.data() + b, buf), e = ef_bytes(s.data() + b), p = bitpack::encode_pfor(s.data() + b, buf);
			pfor += p, forb += f, ef += e, best += std::min(p, e), ef_blocks += e < p, ++blocks;
			for (int m = 0; m < 4; ++m) {
				const bool take = e * 100 < p * (100 - margins[m]);
				marg_bytes[m] += take ? e : p, marg_ef[m] += take;
			}
			exc_blocks += (buf[0] & bitpack::exceptions_flag) != 0;
		}
		full += nf;
		for (std::size_t i = nf; i < s.size(); ++i) { tail_leb += leb_bytes(s[i]); }
		m1.assign(s.begin(), s.end());
		{
			std::size_t i = 0;
			for (u32 c : counts) {
				for (u32 j = 1; j < c; ++j) { m1[i + j] -= 1; }
				i += c;
			}
			for (std::size_t b = 0; b < nf; b += B) {
				const u64 p = bitpack::encode_pfor(m1.data() + b, buf);
				m1_pfor += p, m1_best += std::min<u64>(p, ef_bytes(m1.data() + b));
			}
			for (std::size_t j = nf; j < m1.size(); ++j) { m1_tail += leb_bytes(m1[j]); }
		}
		split_v[0].clear(), split_v[1].clear();
		std::size_t at = 0;
		for (std::size_t d = 0; d < counts.size(); ++d) {
			const u32 c = counts[d], L = lengths[d];
			if (c == 0) { continue; }
			++docs, ++counts_hist[std::min<u32>(c, 4)];
			split_v[0].push_back(s[at]);
			for (u32 k = 1; k < c; ++k) { split_v[1].push_back(s[at + k]); }
			pos.clear();
			u32 q = 0;
			for (u32 k = 0; k < c; ++k) { pos.push_back(q += s[at + k]); }
			if (pos.front() >= 1 && pos.back() <= L) {
				firsts_bw += u64(std::bit_width(L));
				bic += bic_bits(pos.data(), pos.size(), 1, L);
				const int k0 = int(std::bit_width(L / (c + 1))) - 1;
				for (int dl = 0; dl < 3; ++dl) {
					const unsigned k = unsigned(std::max(0, k0 - 1 + dl));
					rice_f[dl] += ((s[at] - 1) >> k) + 1 + k;
					for (u32 j = 1; j < c; ++j) { rice_g[dl] += ((s[at + j] - 1) >> k) + 1 + k; }
				}
			} else {
				++outside;
				firsts_bw += 32, bic += 32 * u64(c);
				for (int dl = 0; dl < 3; ++dl) { rice_f[dl] += 32, rice_g[dl] += 32 * u64(c - 1); }
			}
			at += c;
		}
		firsts += split_v[0].size();
		for (int k = 0; k < 2; ++k) {
			const auto& v = split_v[k];
			const std::size_t n = v.size() / B * B;
			for (std::size_t b = 0; b < n; b += B) {
				const u64 p = bitpack::encode_pfor(v.data() + b, buf);
				sp_pfor[k] += p, sp_best[k] += std::min<u64>(p, ef_bytes(v.data() + b));
			}
			for (std::size_t i = n; i < v.size(); ++i) { sp_tail[k] += leb_bytes(v[i]); }
		}
	});
	const double mb = 1e6;
	std::printf("positions %llu (full blocks %llu, %llu blocks, %.1f%% with exceptions), documents %llu, firsts %.1f%% of positions\n",
		(unsigned long long)positions, (unsigned long long)full, (unsigned long long)blocks, 100.0 * double(exc_blocks) / double(blocks), (unsigned long long)docs,
		100.0 * double(firsts) / double(positions));
	std::printf("counts per document: 1 %.1f%%, 2 %.1f%%, 3 %.1f%%, 4+ %.1f%%; outside [1, length] %llu\n", 100.0 * double(counts_hist[1]) / double(docs),
		100.0 * double(counts_hist[2]) / double(docs), 100.0 * double(counts_hist[3]) / double(docs), 100.0 * double(counts_hist[4]) / double(docs), (unsigned long long)outside);
	std::printf("%-62s %10s %10s\n", "candidate (stream bytes, without headers and counts)", "MB", "vs PFOR");
	const double base = double(pfor + tail_leb);
	auto row = [&](const char* name, double bytes) { std::printf("%-62s %10.2f %+9.2f%%\n", name, bytes / mb, 100.0 * (bytes - base) / base); };
	const double F_pfor = double(sp_pfor[0] + sp_tail[0]), G_pfor = double(sp_pfor[1] + sp_tail[1]);
	const double F_best = double(sp_best[0] + sp_tail[0]), G_best = double(sp_best[1] + sp_tail[1]);
	row("PFOR per block + LEB tail (today)", base);
	row("FOR per block", double(forb + tail_leb));
	row("Elias-Fano per block", double(ef + tail_leb));
	std::printf("  (EF smaller on %.1f%% of blocks)\n", 100.0 * double(ef_blocks) / double(blocks));
	row("min(PFOR, EF) per block (FOR inside PFOR's choice)", double(best + tail_leb));
	for (int m = 1; m < 4; ++m) {
		char name[96];
		std::snprintf(name, sizeof name, "min(PFOR, EF), EF when it saves >= %u%% (%.1f%% of blocks)", margins[m], 100.0 * double(marg_ef[m]) / double(blocks));
		row(name, double(marg_bytes[m] + tail_leb));
	}
	row("gaps minus one, PFOR per block", double(m1_pfor + m1_tail));
	row("gaps minus one, min(PFOR, EF) per block", double(m1_best + m1_tail));
	row("split streams: firsts PFOR, gaps PFOR", F_pfor + G_pfor);
	row("split streams: each min(PFOR, EF)", F_best + G_best);
	row("firsts at bit_width(length), gaps min(PFOR, EF)", double(firsts_bw) / 8 + G_best);
	for (int dl = 0; dl < 3; ++dl) {
		char name[96];
		std::snprintf(name, sizeof name, "Rice from length/(count+1), k%+d: firsts, gaps min(PFOR, EF)", dl - 1);
		row(name, double(rice_f[dl]) / 8 + G_best);
		std::snprintf(name, sizeof name, "Rice from length/(count+1), k%+d: every position", dl - 1);
		row(name, double(rice_f[dl] + rice_g[dl]) / 8);
	}
	row("interpolative per document in [1, length] (bound)", double(bic) / 8);
	std::printf("firsts: PFOR %.2f MB, min(PFOR, EF) %.2f, bit_width(length) %.2f, Rice k-1/k/k+1 %.2f/%.2f/%.2f; gaps: PFOR %.2f, min %.2f, Rice %.2f/%.2f/%.2f\n",
		F_pfor / mb, F_best / mb, double(firsts_bw) / 8 / mb, double(rice_f[0]) / 8 / mb, double(rice_f[1]) / 8 / mb, double(rice_f[2]) / 8 / mb, G_pfor / mb, G_best / mb,
		double(rice_g[0]) / 8 / mb, double(rice_g[1]) / 8 / mb, double(rice_g[2]) / 8 / mb);
	return 0;
}

// Rice code with the parameter from the document: k = max(0, bit_width(length / (count + 1)) - 1);
// each value v >= 1 as (v - 1) >> k zeros, a one, then the low k bits.
unsigned rice_k(u32 L, u32 c) { return unsigned(std::max(0, int(std::bit_width(L / (c + 1))) - 1)); }

struct bit_writer {
	std::vector<u64> words;
	u64 at = 0;
	void put(u64 v, unsigned w) {  // w <= 57
		if (w == 0) { return; }
		if ((at + w + 63) / 64 > words.size()) { words.resize((at + w + 63) / 64 + 1, 0); }
		words[at / 64] |= v << (at % 64);
		if (at % 64 + w > 64) { words[at / 64 + 1] |= v >> (64 - at % 64); }
		at += w;
	}
	void unary(u64 q) {
		for (; q >= 32; q -= 32) { put(0, 32); }
		put(u64(1) << q, unsigned(q) + 1);
	}
};

// Positions of the terms in the files' queries (or the `top` most frequent), each term's full
// blocks decoded whole: today's PFOR, Elias-Fano, min(PFOR, EF), and the Rice code per document.
int decode(const std::string& dir, const std::string& which_terms) {
	segment_view sv(dir);
	const auto& r = sv.seg.get<prospect::inverted_plugin>();
	struct term { u64 n; std::vector<u32> s, lengths, counts; };
	std::vector<term> terms;
	std::vector<std::string> wanted;
	std::size_t top = 0;
	if (!which_terms.empty() && std::isdigit(static_cast<unsigned char>(which_terms[0]))) {
		top = std::stoul(which_terms);
	} else {
		FILE* f = std::fopen(which_terms.c_str(), "r");
		char line[256];
		while (f && std::fgets(line, sizeof line, f)) {
			std::string w(line);
			while (!w.empty() && (w.back() == '\n' || w.back() == '\r')) { w.pop_back(); }
			if (!w.empty()) { wanted.push_back(w); }
		}
		if (f) { std::fclose(f); }
		std::sort(wanted.begin(), wanted.end());
	}
	each_term(r, [&](std::string_view t, const prospect::format::term_info&, const std::vector<u32>& s, const std::vector<u32>& lengths, const std::vector<u32>& counts) {
		if (s.size() < B) { return; }
		if (top == 0 && !std::binary_search(wanted.begin(), wanted.end(), std::string(t))) { return; }
		terms.push_back({s.size(), s, lengths, counts});
		if (top) {
			std::sort(terms.begin(), terms.end(), [](const term& a, const term& b) { return a.n > b.n; });
			if (terms.size() > top) { terms.pop_back(); }
		}
	});
	std::vector<std::uint8_t> pfor_bytes, ef_bytes_v, min_bytes;
	std::uint8_t buf[bitpack::max_encoded_bytes + 64];
	u64 n = 0, efn = 0, all = 0;
	const char* m_env = std::getenv("P13_MARGIN");
	const u64 margin = m_env ? std::stoul(m_env) : 0;  // percent EF must save to be chosen
	bit_writer rice;
	for (auto& t : terms) {
		const std::size_t nf = t.s.size() / B * B;
		for (std::size_t b = 0; b < nf; b += B) {
			const std::size_t p = bitpack::encode_pfor(t.s.data() + b, buf);
			pfor_bytes.insert(pfor_bytes.end(), buf, buf + p);
			std::uint8_t ebuf[2048];
			const std::size_t e = ef_encode(t.s.data() + b, ebuf);
			ef_bytes_v.insert(ef_bytes_v.end(), ebuf, ebuf + e);
			if (e * 100 < p * (100 - margin)) {
				min_bytes.insert(min_bytes.end(), ebuf, ebuf + e), ++efn;
			} else {
				min_bytes.insert(min_bytes.end(), buf, buf + p);
			}
			n += B;
			u32 check[B];
			ef_decode(ebuf, check);
			if (std::memcmp(check, t.s.data() + b, sizeof check) != 0) { std::fprintf(stderr, "ef round trip failed\n"); return 1; }
		}
		std::size_t at = 0;
		for (std::size_t d = 0; d < t.counts.size(); ++d) {
			const unsigned k = rice_k(t.lengths[d], t.counts[d]);
			for (u32 j = 0; j < t.counts[d]; ++j, ++at) {
				const u64 v = t.s[at] - 1;
				rice.unary(v >> k);
				rice.put(v & ((u64(1) << k) - 1), k);
			}
		}
		all += t.s.size();
	}
	rice.words.resize(rice.words.size() + 2, 0);
	for (int i = 0; i < 16; ++i) { pfor_bytes.push_back(0), ef_bytes_v.push_back(0), min_bytes.push_back(0); }
	u32 out[B];
	volatile u32 sink = 0;
	auto timed = [&](auto&& body) {
		double best = 1e30;
		for (int rep = 0; rep < 7; ++rep) {
			const auto t0 = std::chrono::steady_clock::now();
			body();
			best = std::min(best, double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
		}
		return best;
	};
	auto bench = [&](const char* name, const std::vector<std::uint8_t>& bytes, int which) {
		const double ns = timed([&] {
			const std::uint8_t* p = bytes.data();
			for (u64 k = 0; k < n / B; ++k) {
				std::size_t used;
				if (which == 1 || (which == 2 && (*p & 0xC0) == 0xC0)) {
					used = ef_decode(p, out);
				} else {
					used = bitpack::decode(p, bytes.size() - std::size_t(p - bytes.data()), out);
				}
				u32 acc = 0;  // the reader's prefix sum within a block
				for (std::size_t i = 0; i < B; ++i) { acc += out[i]; }
				sink = sink + acc;
				p += used;
			}
		});
		std::printf("%-22s %8.3f ns/position %10.2f MB (full blocks)\n", name, ns / double(n), double(bytes.size() - 16) / 1e6);
	};
	std::printf("%zu terms, %llu positions (%llu in full blocks), EF chosen on %.1f%% of blocks\n", terms.size(), (unsigned long long)all, (unsigned long long)n,
		100.0 * double(efn) * B / double(n));
	bench("PFOR (today)", pfor_bytes, 0);
	bench("Elias-Fano", ef_bytes_v, 1);
	bench("min(PFOR, EF)", min_bytes, 2);
	// Rice: every position of each term in order, the parameter from each document's length and count.
	u64 bad = 0;
	const double rns = timed([&] {
		const u64* w = rice.words.data();
		u64 bit = 0;
		for (const auto& t : terms) {
			std::size_t at = 0;
			for (std::size_t d = 0; d < t.counts.size(); ++d) {
				const unsigned k = rice_k(t.lengths[d], t.counts[d]);
				const u64 mask = (u64(1) << k) - 1;
				u32 acc = 0;
				for (u32 j = 0; j < t.counts[d]; ++j, ++at) {
					// A 64-bit window at `bit` (two words, shifted); codes longer than 64 bits take the slow path.
					const u64 lo_word = w[bit / 64], hi_word = w[bit / 64 + 1];
					const unsigned sh = unsigned(bit % 64);
					u64 win = sh ? (lo_word >> sh) | (hi_word << (64 - sh)) : lo_word;
					u64 q = 0;
					if (win == 0) [[unlikely]] {
						while (win == 0) { q += 64, bit += 64, win = (w[bit / 64] >> (bit % 64)) | (bit % 64 ? w[bit / 64 + 1] << (64 - bit % 64) : 0); }
					}
					const unsigned z = unsigned(std::countr_zero(win));
					q += z, bit += z + 1;
					u64 lo = 0;
					if (z + 1 + k <= 64) {
						lo = (win >> (z + 1)) & mask;
					} else {
						lo = (w[bit / 64] >> (bit % 64)) | (bit % 64 ? w[bit / 64 + 1] << (64 - bit % 64) : 0);
						lo &= mask;
					}
					bit += k;
					acc += u32((q << k | lo) + 1);
				}
				sink = sink + acc;
			}
		}
	});
	{  // correctness, untimed
		const u64* w = rice.words.data();
		u64 bit = 0;
		for (const auto& t : terms) {
			std::size_t at = 0;
			for (std::size_t d = 0; d < t.counts.size(); ++d) {
				const unsigned k = rice_k(t.lengths[d], t.counts[d]);
				for (u32 j = 0; j < t.counts[d]; ++j, ++at) {
					u64 q = 0;
					while (!((w[bit / 64] >> (bit % 64)) & 1)) { ++q, ++bit; }
					++bit;
					u64 lo = 0;
					for (unsigned b = 0; b < k; ++b, ++bit) { lo |= ((w[bit / 64] >> (bit % 64)) & 1) << b; }
					bad += u32((q << k | lo) + 1) != t.s[at];
				}
			}
		}
	}
	std::printf("%-22s %8.3f ns/position %10.2f MB (every position), %llu mismatches\n", "Rice from length", rns / double(all), double(rice.at) / 8 / 1e6, (unsigned long long)bad);
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "sizes" && argc == 3) { return sizes(argv[2]); }
	if (mode == "decode" && argc == 4) { return decode(argv[2], argv[3]); }
	std::fprintf(stderr, "usage: p13_positions sizes SEGDIR | decode SEGDIR TOP|TERMSFILE\n");
	return 2;
}
