/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


// Ordered keys (Phase 4, D3a): typed values mapped to uint64_t so unsigned order is the
// values' order, as Lucene and Tantivy map them. NaN is rejected by the caller.

#pragma once

#include <bit>
#include <cstdint>

namespace prospect::ordered {

constexpr std::uint64_t from_i64(std::int64_t v) noexcept { return std::uint64_t(v) ^ (1ull << 63); }
constexpr std::int64_t to_i64(std::uint64_t k) noexcept { return std::int64_t(k ^ (1ull << 63)); }

// Negatives flip every bit, positives only the sign bit; -0.0 maps as +0.0.
constexpr std::uint64_t from_f64(double v) noexcept {
	if (v == 0.0) { v = 0.0; }
	const auto bits = std::bit_cast<std::uint64_t>(v);
	return bits >> 63 ? ~bits : bits | (1ull << 63);
}
constexpr double to_f64(std::uint64_t k) noexcept { return std::bit_cast<double>(k >> 63 ? k & ~(1ull << 63) : ~k); }

// The 8-byte big-endian form, for keys compared as bytes (a sort key's component on the wire).
inline void put_be(std::uint64_t k, unsigned char* out) noexcept {
	for (int i = 7; i >= 0; --i) { out[i] = static_cast<unsigned char>(k); k >>= 8; }
}

}  // namespace prospect::ordered
