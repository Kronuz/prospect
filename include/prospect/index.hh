// The search layout's index: strata/core/index.hh's basic_index over inverted, keys, and stored,
// with the inverter, the merge, the reordering, and the check report that layout brings.
#pragma once

#include "prospect/check.hh"
#include "prospect/inverter.hh"
#include "prospect/merge.hh"
#include "prospect/reorder.hh"
#include "prospect/strata.hh"
#include "prospect/writer.hh"
#include "strata/core/index.hh"

namespace prospect {

using operation = basic_operation<search_layout>;
using transaction = basic_transaction<search_layout>;
using index_options = basic_index_options<search_layout>;
using index = basic_index<search_layout>;

}  // namespace prospect
