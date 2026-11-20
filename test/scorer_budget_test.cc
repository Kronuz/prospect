// The byte budget against what a phrase's scorers allocate (docs/mechanisms/phrase-expansion.md,
// Bounds): the bytes `scorer()` offers to the budget, summed over the segments, are within 10% of
// the heap the scorers hold, for one segment and for sixteen, under both slot scorings.
#include "phrase_expansion_fixture.hh"

#include <cstdio>
#include <cstdlib>
#include <new>
#if defined(__APPLE__)
#include <malloc/malloc.h>
static std::size_t usable(void* p) { return malloc_size(p); }
#else
#include <malloc.h>
static std::size_t usable(void* p) { return malloc_usable_size(p); }
#endif

static long long g_live = 0;
void* operator new(std::size_t n) {
	void* p = std::malloc(n ? n : 1);
	if (!p) { throw std::bad_alloc(); }
	g_live += (long long)usable(p);
	return p;
}
void operator delete(void* p) noexcept {
	if (p) {
		g_live -= (long long)usable(p);
		std::free(p);
	}
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

static int failures = 0;
#define CHECK(c)                                                    \
	do {                                                            \
		if (!(c)) {                                                 \
			std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
			++failures;                                             \
		}                                                           \
	} while (0)

namespace pf = phrase_fixture;
using namespace prospect;

// Charged and allocated bytes of the scorers of [*, *] in every segment.
static void check(const pf::world& w, slot_scoring how, const char* what) {
	const std::vector<phrase_word> words{phrase_word::starts_with(""), phrase_word::starts_with("")};
	std::uint64_t charged = 0;
	phrase_options o;
	o.scoring = how;
	o.walk.charge = [&](std::uint64_t n) { charged += n; };
	term_table table(w.segs);
	const expanded_phrase ph(w.segs, pf::text_name, words, table, o);
	const std::uint64_t built = charged;
	std::vector<scorer_ptr> held;
	const long long before = g_live;
	for (const auto& s : w.segs) { held.push_back(ph.scorer(*s.seg, 0)); }
	const long long allocated = g_live - before;
	const std::uint64_t offered = charged - built;
	const double ratio = double(offered) / double(allocated);
	std::printf("%s, %zu segments, %s: charged %llu, allocated %lld (%.3fx)\n", what, w.segs.size(), how == slot_scoring::any ? "any" : "synonym", (unsigned long long)offered, allocated, ratio);
	CHECK(allocated > 0 && ratio > 0.9 && ratio < 1.1);
}

int main() {
	const pf::world one(7919, 1200, 36, 10, 6000, std::size_t(1) << 40);
	const pf::world many(7919, 4000, 36, 10, 6000);
	CHECK(one.segs.size() == 1);
	CHECK(many.segs.size() >= 16);
	for (const auto how : {slot_scoring::synonym, slot_scoring::any}) {
		check(one, how, "one");
		check(many, how, "many");
	}
	std::printf("scorer_budget: %d failures\n", failures);
	return failures ? 1 : 0;
}
