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

// The scorer contract: a doc iterator that scores its current doc and bounds the
// scores ahead of it, which is what block-max pruning needs.

#pragma once

#include "docset.hh"

#include <concepts>

namespace strata {

using docset::doc_t;
using docset::no_more_docs;

// A scorer iterates docs like docset::doc_iterator and adds:
//   score()           the current doc's score (valid after next/advance returned a doc)
//   block_end(t)      the last doc of the block that holds t, without decoding the block
//                     (no_more_docs past the end); doc() is unchanged
//   max_score(f, u)   an upper bound on score() for docs in [f, u], from skip data alone,
//                     so a conjunction can bound children it has not moved yet
// Scores are doubles so Xapian-compatible weighting keeps its precision.
template <typename S>
concept scorer = docset::doc_iterator<S> && requires(S s, const S cs, doc_t target, doc_t upto) {
	{ s.score() } -> std::same_as<double>;
	{ s.block_end(target) } -> std::same_as<doc_t>;
	{ s.max_score(target, upto) } -> std::same_as<double>;
};

// A filter that also weights its matches (ranges, geo distance): the same contract.
template <typename S>
concept scored_filter = scorer<S>;

}  // namespace strata
