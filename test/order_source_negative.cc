// Built by the order_source_negative ctest entry, which expects the build to fail: this unit names
// the search layout's index options without prospect/reorder.hh, so it would see the primary
// order_source (no reordering, a smaller options struct) while other units see the search layout's.
#include "prospect/check.hh"
#include "prospect/inverter.hh"
#include "prospect/merge.hh"
#include "prospect/writer.hh"
#include "strata/core/index.hh"

unsigned long options_size() { return sizeof(strata::basic_index_options<prospect::search_layout>); }
