// The one field the single-field tests index into: a transaction declares it and its documents'
// terms are parts naming `declared(0)` (the default part of a document), so it takes id 0.
#pragma once

#include "prospect/index.hh"

#include <vector>

inline const char* const text_name = "text";

inline std::vector<strata::field_declaration> text_fields() {
	return {{text_name, strata::field_role::indexed, strata::field_postings::positions, false}};
}

// The same with the key field `_id`, for a keyed index: its terms are the keys (D7).
inline std::vector<strata::field_declaration> keyed_fields() {
	auto f = text_fields();
	f.push_back({"_id", strata::field_role::key, strata::field_postings::docs, false});
	return f;
}

// The table of an index that declared only `text_fields()`, for snapshots of bare segments.
inline const strata::field_table& text_table() {
	static const strata::field_table table = [] {
		strata::field_table t;
		for (const auto& d : text_fields()) { t.append(d); }
		return t;
	}();
	return table;
}
