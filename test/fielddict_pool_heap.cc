// The heap one open index keeps in its field dictionaries (docs/perf/fielddict-pool-reader.md):
//   fielddict_pool_heap FIELDS SEGMENTS ARM [ROUNDS]
// SEGMENTS readers over one dictionary of FIELDS fields (one own, the rest pooled), opened one
// after another as an index's open does; ARM is `open` (the reader only), `check` (then
// check_fields, as Xapiand's open path) or `query` (then one pooled lookup, which caches the pool
// reader). Prints the live heap per segment (bytes malloc hands out, deterministic) and the
// median nanoseconds to open one segment over ROUNDS (default 15) repeats. Not a ctest: built by
// fielddict_pool_heap.sh, once per side of an A/B.
#include "prospect/fielddict.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <malloc/malloc.h>
static std::size_t usable(void* p) { return malloc_size(p); }
#else
#include <malloc.h>
static std::size_t usable(void* p) { return malloc_usable_size(p); }
#endif

static std::size_t live_bytes = 0;
void* operator new(std::size_t n) {
	if (void* p = std::malloc(n ? n : 1)) {
		live_bytes += usable(p);
		return p;
	}
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
	if (p) { live_bytes -= usable(p); }
	std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

using namespace prospect;

int main(int argc, char** argv) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: fielddict_pool_heap FIELDS SEGMENTS open|check|query [ROUNDS]\n");
		return 2;
	}
	const unsigned fields = unsigned(std::atoi(argv[1])), segments = unsigned(std::atoi(argv[2]));
	const std::string arm = argv[3];
	const unsigned rounds = argc > 4 ? unsigned(std::atoi(argv[4])) : 15;
	if (fields < 2 || segments == 0 || (arm != "open" && arm != "check" && arm != "query")) { return 2; }

	prospect::format::term_info info;
	info.flags = prospect::format::single_posting | prospect::format::no_wdf | prospect::format::no_positions;
	info.df = 1;
	info.cf = 0;
	fielddict::planner plan;
	std::vector<std::pair<std::uint32_t, std::string>> terms;
	for (std::uint32_t f = 1; f < fields; ++f) {
		for (unsigned i = 0; i < 6; ++i) { terms.emplace_back(f, "t" + std::to_string(i)); }  // pooled
	}
	for (unsigned i = 0; i < 400; ++i) { terms.emplace_back(fields, std::to_string(100000 + i)); }  // own
	for (const auto& [f, t] : terms) { plan.add(f, t); }
	fielddict::builder b(plan.finish());
	for (const auto& [f, t] : terms) {
		info.postings = f;
		b.add(f, t, info);
	}
	const auto built = b.finish();

	std::vector<double> ns;
	std::size_t heap = 0;
	for (unsigned r = 0; r < rounds; ++r) {
		std::vector<std::unique_ptr<fielddict::reader>> open;
		open.reserve(segments);
		const std::size_t before = live_bytes;
		const auto t0 = std::chrono::steady_clock::now();
		for (unsigned s = 0; s < segments; ++s) {
			open.push_back(std::make_unique<fielddict::reader>(built.blocks, built.index));
			if (arm != "open") { open.back()->check_fields(fields + 1, std::nullopt); }
			if (arm == "query") {
				const auto v = open.back()->find_run(1, "t3", [](std::string_view) {});
				if (!v) { return 1; }
			}
		}
		const auto t1 = std::chrono::steady_clock::now();
		heap = live_bytes - before;
		ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / segments);
	}
	std::sort(ns.begin(), ns.end());
	std::printf("fields=%u segments=%u arm=%s heap_per_segment=%.1f open_ns_median=%.0f\n", fields, segments, arm.c_str(), double(heap) / segments, ns[ns.size() / 2]);
	return 0;
}
