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

// Offline checking (Lucene's CheckIndex): every byte against its checksum and every
// structure decoded, rather than only what queries happen to read. The core walks the
// documents; each plugin checks its own sections (`P::check`) and counts into its `P::report`.
#pragma once

#include "strata/core/reader.hh"

#include <cstdint>
#include <string_view>
#include <vector>

namespace strata {

namespace detail {
template <class E>
struct plugin_report {
	using type = empty_part<E>;
};
template <class E>
	requires requires { typename E::report; }
struct plugin_report<E> {
	using type = typename E::report;
};
template <class E>
using plugin_report_t = typename plugin_report<E>::type;
}  // namespace detail

template <class Layout>
struct basic_check_report;
// What a check saw: the core's counts, and each plugin's through its base.
template <class... Entries>
struct basic_check_report<layout<Entries...>> : detail::plugin_report_t<Entries>... {
	std::uint64_t segments = 0, documents = 0, deleted = 0;
};

// Checks one segment: verifies every byte, reads every document's docid, then each plugin
// checks its sections (the term dictionary and postings, the keys and versions, the stored
// records). Throws corrupt_segment at the first problem; adds what it saw to r.
template <class... Entries>
void check_segment(const basic_segment<layout<Entries...>>& seg, const std::vector<std::uint64_t>* deleted, basic_check_report<layout<Entries...>>& r,
	std::string_view key_prefix = {}) {
	seg.verify_all();
	++r.segments;
	r.documents += seg.core().doc_count();
	for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
		if (deleted && ((*deleted)[o >> 6] >> (o & 63)) & 1) { ++r.deleted; }
		(void)seg.core().docid(o);
	}
	([&] {
		if constexpr (requires { typename Entries::report; }) {
			Entries::check(seg.template get<Entries>(), seg.core(), static_cast<typename Entries::report&>(r), key_prefix);
		}
	}(), ...);
}

}  // namespace strata
