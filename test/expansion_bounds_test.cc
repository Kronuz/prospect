// The bounds of a phrase's expansion (include/prospect/phrase_expand.hh, the phrase-expansion
// quest's A2): each word is bounded by its own limit and the query by max_expansions (literal
// words are not counted); past a bound the phrase throws phrase_expansion_error naming the word
// and the bound, and the walk stops where it is instead of running to the end of the dictionary.
//   1. Sizes taken from the brute-force evaluation: a limit at the size answers, one below throws,
//      for a word and for the query, at every word, as `error`, `first` and `most_frequent`.
//   2. The bound reported is the tighter, the word's on a tie.
//   3. A bounded walk visits fewer entries than an unbounded one; cancellation, the byte budget and
//      the key field's refusal reach the caller.
#include "phrase_expansion_fixture.hh"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 40) std::exit(1); } \
	} while (0)

namespace pf = phrase_fixture;
using namespace prospect;

namespace {

std::uint64_t answered = 0, refused = 0;

// The query's answer is the evaluation's: its error and its terms.
void agree(const pf::world& w, const pf::query& q) {
	const auto want = pf::evaluate(w, q);
	const auto got = pf::run(w, q);
	CHECK(got.error.has_value() == want.error);
	if (want.error) {
		++refused;
		if (got.error) { CHECK(got.error->word == want.word && got.error->per_query == want.per_query && got.error->max == want.max); }
	} else {
		++answered;
		CHECK(got.slots == want.slots || !got.error);
		for (std::size_t k = 0; k < want.slots.size() && k < got.slots.size(); ++k) {
			if (want.exact_slot[k]) { CHECK(got.slots[k] == want.slots[k]); }
		}
	}
}

// Boundary values of every bound of a random phrase: one under, at and one over the sizes.
void sweep(const pf::world& w, std::mt19937_64& r) {
	for (int n = 0; n < 150; ++n) {
		pf::query q;
		q.field = r() % 3 ? pf::text_name : pf::title_name;
		const auto& pool = q.field == pf::text_name ? w.text_pool : w.title_pool;
		const std::size_t words = 1 + r() % 4;
		std::vector<std::uint64_t> sizes;
		std::uint64_t total = 0;
		for (std::size_t i = 0; i < words; ++i) {
			const std::string& base = pool[r() % pool.size()];
			switch (r() % 4) {
			case 0: q.words.push_back(phrase_word::term(base)); break;
			case 1: q.words.push_back(phrase_word::starts_with(base.substr(0, r() % 2))); break;
			case 2: q.words.push_back(phrase_word::wildcard(base.substr(0, 1) + "*")); break;
			default: q.words.push_back(phrase_word::similar(base, r() % 3)); break;
			}
			const auto size = q.words.back().how == phrase_word::literal ? 0 : pf::live_matches(w, q.field, q.words.back()).size();
			sizes.push_back(size);
			total += size;
		}
		for (const std::int64_t delta : {-1, 0, 1}) {
			// The query's bound at the total, then each word's own limit at its size.
			if (total + delta > 0 || delta >= 0) {
				pf::query p = q;
				p.max_expansions = std::uint64_t(std::max<std::int64_t>(0, std::int64_t(total) + delta));
				agree(w, p);
			}
			for (std::size_t i = 0; i < words; ++i) {
				if (!sizes[i] || std::int64_t(sizes[i]) + delta < 1) { continue; }
				for (const auto type : {expansion_limit::error, expansion_limit::first, expansion_limit::most_frequent}) {
					pf::query p = q;
					p.words[i].limit = {type, std::uint64_t(std::int64_t(sizes[i]) + delta)};
					if (r() % 2) { p.max_expansions = std::uint64_t(std::max<std::int64_t>(1, std::int64_t(total) + delta)); }
					agree(w, p);
				}
			}
		}
	}
}

void fixed(const pf::world& w) {
	const std::string field = pf::text_name;
	const auto all = pf::live_matches(w, field, phrase_word::starts_with("")).size();
	const auto as = pf::live_matches(w, field, phrase_word::starts_with("a")).size();
	const auto bs = pf::live_matches(w, field, phrase_word::starts_with("b")).size();
	CHECK(all > 400 && as > 20 && bs > 20);
	auto query = [&](std::vector<phrase_word> words, std::uint64_t max = 0) {
		pf::query q;
		q.words = std::move(words);
		q.max_expansions = max;
		return q;
	};
	// A word's own bound: at its size answers, below throws, naming the word (index 1, after a literal).
	for (const std::uint64_t limit : {all, all - 1}) {
		auto w1 = phrase_word::starts_with("");
		w1.limit = {expansion_limit::error, limit};
		const auto got = pf::run(w, query({phrase_word::term("a"), w1}));
		CHECK(got.error.has_value() == (limit < all));
		if (got.error) {
			CHECK(got.error->word == 1 && !got.error->per_query && got.error->max == limit);
			CHECK(std::string(got.error->what()).find("phrase word 1") != std::string::npos);
		}
	}
	// The query's bound counts the words' terms together, and not the literal.
	{
		const auto words = std::vector<phrase_word>{phrase_word::starts_with("a"), phrase_word::term("b"), phrase_word::starts_with("b")};
		CHECK(!pf::run(w, query(words, as + bs)).error);
		const auto got = pf::run(w, query(words, as + bs - 1));
		CHECK(got.error && got.error->word == 2 && got.error->per_query && got.error->max == as + bs - 1);
		const auto first = pf::run(w, query(words, as - 1));
		CHECK(first.error && first.error->word == 0 && first.error->per_query);
	}
	// A tie names the word: own == what the query has left, one term too many.
	{
		auto w0 = phrase_word::starts_with("a");
		w0.limit = {expansion_limit::error, as - 1};
		const auto got = pf::run(w, query({w0}, as - 1));
		CHECK(got.error && !got.error->per_query && got.error->max == as - 1);
		w0.limit.max = as;  // the query's is tighter
		const auto tighter = pf::run(w, query({w0}, as - 1));
		CHECK(tighter.error && tighter.error->per_query && tighter.error->max == as - 1);
	}
	// A word that expands to nothing costs nothing; a query with nothing left refuses any term.
	{
		CHECK(!pf::run(w, query({phrase_word::starts_with("zzz"), phrase_word::starts_with("a")}, as)).error);
		const auto got = pf::run(w, query({phrase_word::starts_with("a"), phrase_word::starts_with("b")}, as));
		CHECK(got.error && got.error->word == 1 && got.error->per_query);
		CHECK(!pf::run(w, query({phrase_word::starts_with("a"), phrase_word::starts_with("zzz")}, as)).error);
	}
	// `first` and `most_frequent` cap a word under the query's bound; past it they throw.
	for (const auto type : {expansion_limit::first, expansion_limit::most_frequent}) {
		auto w0 = phrase_word::starts_with("a");
		w0.limit = {type, 5};
		auto capped = pf::run(w, query({w0}, 5));
		CHECK(!capped.error && capped.slots[0].size() == 5);
		auto over = pf::run(w, query({w0}, 4));
		CHECK(over.error && over.error->per_query && over.error->max == 4);
		w0.limit = {type, 0};  // no bound of its own: the query's applies
		CHECK(pf::run(w, query({w0}, as - 1)).error.has_value());
	}
	// An error stops the walk: the entries visited grow with the bound and stay below the unbounded
	// walk's (a segment's first entries may all be dead terms, so the first bound stops later than
	// its fourth term in the dictionary).
	{
		auto visited = [&](std::vector<phrase_word> words, std::uint64_t max) {
			std::uint64_t n = 0;
			walk_options o;
			o.check_every = 1;
			o.pause = [&] { return ++n, false; };
			phrase_options po;
			po.walk = o;
			po.max_expansions = max;
			term_table t(w.segs);
			try {
				(void)expanded_phrase(w.segs, field, words, t, po);
			} catch (const phrase_expansion_error&) {
				CHECK(max != 0);
			}
			return n;
		};
		const std::vector<phrase_word> everything = {phrase_word::starts_with("")};
		const auto none = visited(everything, 0), few = visited(everything, 3), some = visited(everything, all / 2);
		CHECK(few > 0 && few <= some && some < none && few * 2 < none);
		// A word met with nothing left in the query stops at its first term, as at its limit.
		const auto first = visited({phrase_word::starts_with("a")}, as);
		const auto both = visited({phrase_word::starts_with("a"), phrase_word::starts_with("")}, as);
		CHECK(both >= first && (both - first) * 2 < none);
	}
	// Cancellation, the byte budget and the key field reach the caller.
	{
		task::cancel_source src;
		const task::cancel_token token = src.token();
		src.request();
		walk_options o;
		o.check_every = 1;
		o.cancel = &token;
		bool cancelled = false;
		try {
			(void)pf::run(w, query({phrase_word::starts_with("")}), o);
		} catch (const task::cancelled&) {
			cancelled = true;
		}
		CHECK(cancelled);
		walk_options b;
		b.charge = [](std::uint64_t) { throw std::length_error("budget"); };
		bool refused_by_budget = false;
		try {
			(void)pf::run(w, query({phrase_word::starts_with("a")}), b);
		} catch (const std::length_error&) {
			refused_by_budget = true;
		}
		CHECK(refused_by_budget);
		bool invalid = false;
		try {
			pf::query q = query({phrase_word::starts_with("k")});
			q.field = pf::key_name;
			(void)pf::run(w, q);
		} catch (const std::invalid_argument&) {
			invalid = true;
		}
		CHECK(invalid);
		bool empty = false;
		try {
			(void)pf::run(w, query({}));
		} catch (const std::invalid_argument&) {
			empty = true;
		}
		CHECK(empty);
	}
	// A refused phrase leaves the snapshot as it was: the same phrase with room answers.
	{
		const auto words = std::vector<phrase_word>{phrase_word::starts_with("a"), phrase_word::starts_with("b")};
		CHECK(pf::run(w, query(words, as)).error.has_value());
		const auto got = pf::run(w, query(words, as + bs));
		CHECK(!got.error && got.slots[0].size() == as && got.slots[1].size() == bs);
	}
}

}  // namespace

int main() {
	for (std::uint64_t seed = 1; seed <= 3; ++seed) {
		const pf::world w(seed * 7919, 1500);
		CHECK(w.segs.size() > 1 && w.with_deletes() > 0 && w.own_dictionaries() > 0 && w.pooled_dictionaries() > 0);
		std::mt19937_64 r(seed);
		if (seed == 1) { fixed(w); }
		sweep(w, r);
	}
	CHECK(answered > 300 && refused > 300);
	std::printf("expansion_bounds: %llu bounded phrases answered, %llu refused past a bound, %d failures\n", (unsigned long long)answered, (unsigned long long)refused, failures);
	return failures ? 1 : 0;
}
