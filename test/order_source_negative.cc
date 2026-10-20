// Built by the order_source_negative ctest entry, which expects the build to fail: this unit names
// the search layout's index options without search/reorder.hh, so it would see the primary
// order_source (no reordering, a smaller options struct) while other units see the search layout's.
#include "strata/core/index.hh"
#include "strata/search/check.hh"
#include "strata/search/inverter.hh"
#include "strata/search/merge.hh"
#include "strata/search/writer.hh"

unsigned long options_size() { return sizeof(strata::basic_index_options<strata::search_layout>); }
