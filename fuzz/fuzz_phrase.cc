// libFuzzer target: a phrase of expanding words, decoded from the input (words of every shape,
// limits, window, offsets, scoring, max_expansions), must answer what the brute-force evaluation
// of test/phrase_expansion_fixture.hh answers over a fixed world (several segments, deletes, own
// and pooled dictionaries): the same terms, documents, scores, or the same refusal. A mismatch
// aborts. Words take arbitrary bytes, so lone continuation bytes and invalid UTF-8 reach the
// automata.
#include "../test/phrase_expansion_fixture.hh"

#include <cstdlib>
#include <cstring>

namespace pf = phrase_fixture;
using namespace prospect;

namespace {

const pf::world& world() {
	static const pf::world w(20261120, 700, 24, 8, 600, 150);
	return w;
}

struct reader {
	const std::uint8_t* p;
	std::size_t n;
	std::uint8_t byte() { return n ? (--n, *p++) : 0; }
	std::string bytes(std::size_t k) {
		std::string s;
		while (k-- && n) { s.push_back(char(byte())); }
		return s;
	}
};

[[noreturn]] void mismatch(const char* what) {
	std::fprintf(stderr, "fuzz_phrase: %s\n", what);
	std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	const pf::world& w = world();
	reader in{data, size};
	pf::query q;
	const std::uint8_t head = in.byte();
	q.field = head & 1 ? pf::title_name : pf::text_name;
	q.scoring = head & 2 ? slot_scoring::any : slot_scoring::synonym;
	const auto& pool = q.field == pf::text_name ? w.text_pool : w.title_pool;
	const std::size_t words = 1 + (head >> 2) % 4;
	for (std::size_t i = 0; i < words; ++i) {
		const std::uint8_t shape = in.byte(), text = in.byte();
		std::string t = text & 1 ? pool[(text >> 1) % pool.size()] : in.bytes(text >> 5);
		if (text & 1) { t = t.substr(0, (text >> 6) % (t.size() + 1)); }
		phrase_word word;
		switch (shape % 4) {
		case 0: word = phrase_word::term(t); break;
		case 1: word = phrase_word::starts_with(t); break;
		case 2: word = phrase_word::wildcard(t, (shape >> 2) % 4); break;
		default: word = phrase_word::similar(t, (shape >> 2) % 4, (shape >> 4) % 3, (shape >> 6) & 1); break;
		}
		const std::uint8_t lim = in.byte();
		if (word.how != phrase_word::literal && lim & 0x80) { word.limit = {expansion_limit::kind((lim >> 5) % 3), std::uint64_t(lim & 0x1f)}; }
		q.words.push_back(std::move(word));
	}
	const std::uint8_t shape = in.byte(), extra = in.byte();
	if (shape & 1) {
		q.window = std::uint32_t(words + (shape >> 1) % 5);
	} else if (shape & 2) {
		std::uint32_t at = 0;
		for (std::size_t i = 0; i < words; ++i) {
			q.offsets.push_back(at);
			at += 1 + ((shape >> 2) >> (2 * (i % 3))) % 3;
		}
	}
	q.max_expansions = extra & 0x3f ? std::uint64_t(extra & 0x3f) : 0;
	if (extra & 0x80) { q.max_expansions = 0; }

	const auto want = pf::evaluate(w, q);
	const auto got = pf::run(w, q);
	if (got.error.has_value() != want.error) { mismatch("refusal differs"); }
	if (want.error) {
		if (got.error->word != want.word || got.error->per_query != want.per_query || got.error->max != want.max) { mismatch("refusal names another bound"); }
		return 0;
	}
	if (got.slots.size() != want.slots.size()) { mismatch("slot count"); }
	for (std::size_t k = 0; k < got.slots.size(); ++k) {
		if (want.exact_slot[k] && got.slots[k] != want.slots[k]) { mismatch("a word's terms"); }
	}
	if (got.hits.size() != want.hits.size() || got.count != want.hits.size()) { mismatch("hit count"); }
	for (const auto& h : got.hits) {
		const auto it = want.hits.find(h.docid);
		if (it == want.hits.end() || !pf::close(h.score, it->second)) { mismatch("a hit or its score"); }
	}
	return 0;
}
