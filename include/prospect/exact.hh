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

// Exact accumulators (Phase 4, D11): sums whose result does not depend on the order of
// additions or on how they are split and merged, so one shard and many answer alike.
// Integers sum in 128 bits. Doubles sum in a fixed-point superaccumulator spanning every
// double's bits, rounded once, to nearest even, when the result is read; a square enters
// as the error-free pair (x*x, fma(x, x, -x*x)).

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace prospect::exact {

// A signed integer sum that cannot overflow for fewer than 2^64 values.
struct integer_sum {
	__int128 value = 0;
	void add(std::int64_t v) noexcept { value += v; }
	void merge(const integer_sum& other) noexcept { value += other.value; }
};

// The exact sum of doubles. Bit k of the fixed point is 2^(k - 1074), the least
// significant bit of the smallest subnormal; 32-bit chunks held in int64 absorb 2^31
// additions between carry propagations.
class double_sum {
public:
	static constexpr int chunk_bits = 32;
	static constexpr int chunks = (2098 + 64) / chunk_bits + 2;  // every finite double's bits, and carries

	void add(double x) noexcept {
		if (!std::isfinite(x)) {
			if (std::isnan(x)) { ++nan_; } else if (x > 0) { ++positive_infinity_; } else { ++negative_infinity_; }
			return;
		}
		if (x == 0.0) { return; }
		const auto bits = std::bit_cast<std::uint64_t>(x);
		const bool negative = bits >> 63;
		const int exponent = int((bits >> 52) & 0x7ff);
		std::uint64_t mantissa = bits & ((1ull << 52) - 1);
		int shift;  // the mantissa's lowest bit, in fixed-point bits
		if (exponent == 0) { shift = 0; } else { mantissa |= 1ull << 52; shift = exponent - 1; }
		// The 53 bits land across at most three chunks.
		const int first = shift / chunk_bits, offset = shift % chunk_bits;
		const unsigned __int128 placed = static_cast<unsigned __int128>(mantissa) << offset;
		for (int i = 0; i < 3 && first + i < chunks; ++i) {
			const auto part = std::int64_t(std::uint64_t(placed >> (chunk_bits * i)) & 0xffffffffu);
			chunk_[first + i] += negative ? -part : part;
		}
		if (++pending_ == (1u << 30)) { normalize(); }
	}
	// x * x, exactly: the rounded product and its rounding error.
	void add_square(double x) noexcept {
		const double high = x * x;
		add(high);
		if (std::isfinite(high)) { add(std::fma(x, x, -high)); }
	}
	void merge(const double_sum& other) noexcept {
		normalize(); auto copy = other; copy.normalize();
		for (int i = 0; i < chunks; ++i) { chunk_[i] += copy.chunk_[i]; }
		nan_ += other.nan_; positive_infinity_ += other.positive_infinity_; negative_infinity_ += other.negative_infinity_;
		normalize();
	}
	// The sum, correctly rounded to nearest even; infinities as IEEE adds them.
	double value() const noexcept {
		if (nan_ || (positive_infinity_ && negative_infinity_)) { return std::numeric_limits<double>::quiet_NaN(); }
		if (positive_infinity_) { return std::numeric_limits<double>::infinity(); }
		if (negative_infinity_) { return -std::numeric_limits<double>::infinity(); }
		auto copy = *this; copy.normalize();
		// After normalize, chunks below the top are in [0, 2^32) and the top holds the sign.
		const bool negative = copy.chunk_[chunks - 1] < 0;
		if (negative) { copy.negate(); }
		int top = chunks - 1;
		while (top >= 0 && copy.chunk_[top] == 0) { --top; }
		if (top < 0) { return 0.0; }
		// The highest set bit, then the 53 bits below it with a guard and a sticky bit.
		const int high_bit = top * chunk_bits + (63 - std::countl_zero(std::uint64_t(copy.chunk_[top])));
		auto bit = [&](int k) -> std::uint64_t { return k < 0 ? 0 : (std::uint64_t(copy.chunk_[k / chunk_bits]) >> (k % chunk_bits)) & 1; };
		// Normal results keep 53 significant bits; subnormal ones keep bits down to 2^-1074.
		const int lowest_kept = std::max(high_bit - 52, 0);
		std::uint64_t mantissa = 0;
		for (int k = high_bit; k >= lowest_kept; --k) { mantissa = (mantissa << 1) | bit(k); }
		const std::uint64_t guard = bit(lowest_kept - 1);
		// Sticky: any bit below the guard.
		bool sticky = false;
		if (const int below = lowest_kept - 2; below >= 0) {
			const int c = below / chunk_bits, b = below % chunk_bits;
			sticky = (std::uint64_t(copy.chunk_[c]) & ((2ull << b) - 1)) != 0;
			for (int i = c - 1; i >= 0 && !sticky; --i) { sticky = copy.chunk_[i] != 0; }
		}
		if (guard && (sticky || (mantissa & 1))) { ++mantissa; }
		const double result = std::ldexp(double(mantissa), lowest_kept - 1074);
		return negative ? -result : result;
	}

private:
	// Carries move up so every chunk but the top lies in [0, 2^32).
	void normalize() noexcept {
		for (int i = 0; i + 1 < chunks; ++i) {
			const std::int64_t carry = chunk_[i] >> chunk_bits;  // floor division
			chunk_[i] -= carry << chunk_bits; chunk_[i + 1] += carry;
		}
		pending_ = 0;
	}
	void negate() noexcept {
		for (auto& c : chunk_) { c = -c; }
		normalize();
	}
	std::array<std::int64_t, chunks> chunk_{};
	std::uint32_t pending_ = 0;
	std::uint64_t nan_ = 0, positive_infinity_ = 0, negative_infinity_ = 0;
};

}  // namespace prospect::exact
