// Merge sources over an index's segment views: merge_source takes plain word vectors, so each
// view's deleted_bits are copied into storage the sources point at.
#pragma once

#include "prospect/merge.hh"

#include <cstdint>
#include <vector>

struct view_sources {
	std::vector<prospect::merge_source> sources;
	std::vector<std::vector<std::uint64_t>> bits;

	template <class Views>
	explicit view_sources(const Views& views) {
		bits.reserve(views.size());
		for (const auto& v : views) {
			const std::vector<std::uint64_t>* deleted = nullptr;
			if (v.deleted) {
				bits.emplace_back(v.deleted->begin(), v.deleted->end());
				deleted = &bits.back();
			}
			sources.push_back({v.seg(), deleted});
		}
	}
	view_sources(view_sources&&) = default;
	view_sources(const view_sources&) = delete;
};
