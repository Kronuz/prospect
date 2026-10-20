// The search layout's check report: the core's counts, and the terms, postings, positions,
// stored bytes, and keys its plugins count.
#pragma once

#include "strata/core/check.hh"
#include "strata/search/reader.hh"

namespace strata {

using check_report = basic_check_report<search_layout>;

}  // namespace strata
