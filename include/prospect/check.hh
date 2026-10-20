// The search layout's check report: the core's counts, and the terms, postings, positions,
// stored bytes, and keys its plugins count.
#pragma once

#include "prospect/reader.hh"
#include "prospect/strata.hh"
#include "strata/core/check.hh"

namespace prospect {

using check_report = basic_check_report<search_layout>;

}  // namespace prospect
