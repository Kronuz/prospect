// PHRASE and NEAR over words that expand (include/prospect/phrase_expand.hh, the phrase-expansion
// quest's A1): prefix, wildcard and fuzzy words in phrases, with offsets and as NEAR, over the
// fixture's segments (own and pooled dictionaries, deleted documents, several segments) against
// the brute-force evaluation, position by position: each word's expansion, the documents, the
// counts, and each document's score under OR and SYNONYM slots. Queries past a bound must throw
// what the evaluation says (expansion_bounds_test pins the bounds themselves).
#include "phrase_expansion_fixture.hh"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 40) std::exit(1); } \
	} while (0)

namespace pf = phrase_fixture;
using namespace prospect;

namespace {

struct tally {
	std::uint64_t queries = 0, matched = 0, errors = 0, near_matched = 0, gapped_matched = 0, expanding = 0, multi_term_slots = 0, dead_literals = 0;
	std::uint64_t shapes[4] = {0, 0, 0, 0};
} t;

std::string describe(const pf::query& q) {
	std::string s = q.field + (q.window ? " NEAR " + std::to_string(q.window) : std::string(" PHRASE")) + ":";
	for (const auto& w : q.words) {
		static const char* const names[] = {"term", "prefix", "glob", "fuzzy"};
		s += std::string(" ") + names[w.how] + "(\"" + w.text + "\"";
		if (w.how == phrase_word::fuzzy) { s += "," + std::to_string(w.distance) + "," + std::to_string(w.fixed); }
		if (w.limit.max) { s += ",limit " + std::to_string(int(w.limit.type)) + "/" + std::to_string(w.limit.max); }
		s += ")";
	}
	return s + (q.max_expansions ? " max " + std::to_string(q.max_expansions) : std::string());
}

void check(const pf::world& w, const pf::query& q) {
	const auto want = pf::evaluate(w, q);
	const auto got = pf::run(w, q);
	++t.queries;
	if (want.error) {
		++t.errors;
		CHECK(got.error.has_value());
		if (got.error) {
			CHECK(got.error->word == want.word && got.error->per_query == want.per_query && got.error->max == want.max);
			if (got.error->word != want.word || got.error->per_query != want.per_query || got.error->max != want.max) { std::fprintf(stderr, "  %s\n", describe(q).c_str()); }
		}
		return;
	}
	CHECK(!got.error.has_value());
	if (got.error) {
		std::fprintf(stderr, "  threw %s on %s\n", got.error->what(), describe(q).c_str());
		return;
	}
	CHECK(got.slots.size() == want.slots.size());
	for (std::size_t k = 0; k < got.slots.size() && k < want.slots.size(); ++k) {
		if (want.exact_slot[k]) {
			CHECK(got.slots[k] == want.slots[k]);
		} else {
			++t.dead_literals;
		}
		t.multi_term_slots += got.slots[k].size() > 1;
	}
	if (got.hits.size() != want.hits.size() || got.count != want.hits.size()) {
		std::fprintf(stderr, "  %zu hits, %llu counted, want %zu: %s\n", got.hits.size(), (unsigned long long)got.count, want.hits.size(), describe(q).c_str());
	}
	CHECK(got.hits.size() == want.hits.size());
	CHECK(got.count == want.hits.size());
	for (const auto& h : got.hits) {
		const auto it = want.hits.find(h.docid);
		CHECK(it != want.hits.end());
		if (it != want.hits.end()) { CHECK(pf::close(h.score, it->second)); }
	}
	for (std::size_t i = 1; i < got.hits.size(); ++i) { CHECK(got.hits[i - 1].score > got.hits[i].score || (got.hits[i - 1].score == got.hits[i].score && got.hits[i - 1].docid < got.hits[i].docid)); }
	t.matched += !want.hits.empty();
	t.near_matched += q.window && !want.hits.empty();
	t.gapped_matched += !q.offsets.empty() && !want.hits.empty();
}

phrase_word random_word(std::mt19937_64& r, const std::vector<std::string>& pool, bool limited) {
	const std::string& base = pool[r() % pool.size()];
	phrase_word w;
	switch (r() % 5) {
	case 0: w = phrase_word::term(base); break;
	case 1: w = phrase_word::starts_with(base.substr(0, std::min<std::size_t>(base.size(), r() % 3))); break;
	case 2: {
		std::string pattern;
		for (const auto& c : automata::decode(base)) { pattern += r() % 3 == 0 ? (r() % 2 ? "*" : "?") : std::string(c.bytes); }
		w = phrase_word::wildcard(pattern, unsigned(r() % 4));
		break;
	}
	case 3: w = phrase_word::similar(base, r() % 3, r() % 3, r() % 2); break;
	default: w = phrase_word::starts_with(base.substr(0, 1)); break;
	}
	++t.shapes[w.how];
	if (limited && w.how != phrase_word::literal && r() % 3 == 0) {
		w.limit.type = expansion_limit::kind(r() % 3);
		w.limit.max = 1 + r() % 6;
	}
	return w;
}

pf::query random_query(std::mt19937_64& r, const pf::world& w, bool limited) {
	pf::query q;
	q.field = r() % 4 ? pf::text_name : pf::title_name;
	std::vector<std::string> pool = q.field == pf::text_name ? w.text_pool : w.title_pool;
	if (q.field == pf::text_name && r() % 5 == 0) { pool = w.tail_pool; }  // words of the long tail: few documents
	const std::size_t n = 1 + r() % 4;
	for (std::size_t i = 0; i < n; ++i) { q.words.push_back(random_word(r, pool, limited)); }
	q.scoring = r() % 2 ? slot_scoring::any : slot_scoring::synonym;
	if (r() % 3 == 0) {
		q.window = std::uint32_t(n + r() % 4);
	} else if (r() % 3 == 0) {
		std::uint32_t at = 0;
		for (std::size_t i = 0; i < n; ++i) {
			q.offsets.push_back(at);
			at += 1 + r() % 3;
		}
	}
	if (limited && r() % 3 == 0) { q.max_expansions = 1 + r() % 20; }
	return q;
}

}  // namespace

int main() {
	for (std::uint64_t seed = 1; seed <= 4; ++seed) {
		const pf::world w(seed * 977, 1200 + seed * 300);
		CHECK(w.segs.size() > 1 && w.with_deletes() > 0);
		CHECK(w.own_dictionaries() > 0 && w.pooled_dictionaries() > 0);
		std::mt19937_64 r(seed);
		// Fixed shapes: a word that expands to everything, to nothing, and a one-word phrase.
		for (const char* field : {pf::text_name, pf::title_name}) {
			for (const auto& word : {phrase_word::starts_with(""), phrase_word::wildcard("*"), phrase_word::wildcard("zzz*"), phrase_word::similar("a", 1), phrase_word::term("a")}) {
				pf::query q;
				q.field = field;
				q.words = {word};
				check(w, q);
				q.words = {word, phrase_word::starts_with("")};
				q.window = 3;
				check(w, q);
			}
		}
		// Unlimited queries (every one answers), then bounded ones (some throw).
		for (int i = 0; i < 250; ++i) { check(w, random_query(r, w, false)); }
		for (int i = 0; i < 250; ++i) { check(w, random_query(r, w, true)); }
	}
	CHECK(t.matched > 400 && t.near_matched > 60 && t.gapped_matched > 40 && t.errors > 50 && t.multi_term_slots > 400);
	CHECK(t.shapes[0] > 100 && t.shapes[1] > 100 && t.shapes[2] > 100 && t.shapes[3] > 100);
	std::printf("phrase_expansion: %llu queries (%llu matching, %llu NEAR, %llu with gaps, %llu refused past a bound), %llu multi-term slots, %d failures\n",
		(unsigned long long)t.queries, (unsigned long long)t.matched, (unsigned long long)t.near_matched, (unsigned long long)t.gapped_matched, (unsigned long long)t.errors,
		(unsigned long long)t.multi_term_slots, failures);
	return failures ? 1 : 0;
}
