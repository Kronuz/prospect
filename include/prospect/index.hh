// The search layout's index: core/index.hh's basic_index over inverted, keys, and stored, with
// the inverter, the merge, the reordering, and the check report that layout brings.
#pragma once

#include "strata/core/index.hh"
#include "strata/search/check.hh"
#include "strata/search/inverter.hh"
#include "strata/search/merge.hh"
#include "strata/search/reorder.hh"
#include "strata/search/writer.hh"

namespace strata {

using operation = basic_operation<search_layout>;
using transaction = basic_transaction<search_layout>;
using index_options = basic_index_options<search_layout>;
using index = basic_index<search_layout>;

}  // namespace strata
