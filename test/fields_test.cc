// Fields in the search layout (fields-and-keys D1, D3, D5, D9): a term is (field, bytes), so the
// same bytes in two fields are two terms, the empty term is a term, a field with at least 256 terms
// has a dictionary of its own and the rest share the pool, a document's length is the host's, and
// the segment a flush writes, a merge writes, and the buffer digest all agree.
#include "prospect/index.hh"
#include "prospect/searcher.hh"
#include "strata/core/faultfs.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace prospect;
namespace faultfs = strata::faultfs;

static const std::array<char, 16> uuid{'f', 'i', 'e', 'l', 'd', 's'};

using field_part = inverted_plugin::field_part;

static std::vector<strata::field_declaration> declarations() {
	return {{"title", strata::field_role::indexed, strata::field_postings::positions, false},
		{"body", strata::field_role::indexed, strata::field_postings::freqs, false},
		{"big", strata::field_role::indexed, strata::field_postings::freqs, false},
		{"_id", strata::field_role::key, strata::field_postings::docs, false}};
}
// The declarations' positions in `declarations()`, as the references a transaction uses.
static constexpr auto TITLE = strata::field_ref::declared(0), BODY = strata::field_ref::declared(1), BIG = strata::field_ref::declared(2),
					  KEY = strata::field_ref::declared(3);

static term_entry term(std::string t, std::uint32_t wdf = 1, std::vector<std::uint32_t> positions = {}) { return {std::move(t), wdf, std::move(positions)}; }

static operation add(const std::string& key, std::vector<field_part> parts, std::optional<std::uint32_t> length = std::nullopt) {
	operation o;
	o.type = operation::replace_key;
	o.key = key;
	o.doc.key = key;
	o.doc.fields = std::move(parts);
	o.doc.length = length;
	o.doc.stored = key;
	return o;
}
static field_part part(strata::field_ref f, std::vector<term_entry> terms, std::uint32_t length = 0) {
	field_part p;
	p.field = f;
	p.terms = std::move(terms);
	p.length = length;
	return p;
}
static transaction tx(std::vector<operation> ops) {
	transaction t;
	t.fields = declarations();
	t.ops = std::move(ops);
	return t;
}

static std::vector<segment_snapshot> snapshot(const prospect::index& ix) {
	std::vector<segment_snapshot> out;
	for (const auto& s : ix.segments()) { out.push_back({s.seg(), s.deleted, s.dead, &ix.fields()}); }
	return out;
}

static std::uint64_t df(const prospect::index& ix, const char* field, const std::string& t) { return snapshot_df(snapshot(ix), field, t); }

// Every (field name, term) with its document count over the live documents of the index's
// segments, read through the segments' dictionaries.
static std::map<std::pair<std::string, std::string>, std::uint64_t> dictionary_of(const prospect::index& ix) {
	std::map<std::pair<std::string, std::string>, std::uint64_t> out;
	for (const auto& s : ix.segments()) {
		s.seg()->get<inverted_plugin>().scan_terms([&](std::uint32_t field, std::string_view t, const prospect::format::term_info& info) {
			postings_iterator it(s.seg()->get<inverted_plugin>(), info);
			for (ordinal_t o = it.next(); o != no_more_docs; o = it.next()) {
				if (s.live(o)) { ++out[{ix.fields()[field].name, std::string(t)}]; }
			}
			return true;
		});
	}
	return out;
}

static void basic_tests() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	prospect::index::options o;
	o.key_prefix = "Q";
	auto ix = prospect::index::create(io, o, uuid);
	// The same bytes in two fields are two terms; the empty term is a term; a document's length
	// is the host's, not a count of its terms.
	const auto v = ix.apply(tx({add("Q1", {part(TITLE, {term("alpha", 2, {1, 2}), term(""), term("beta", 1, {3})}, 4), part(BODY, {term("alpha"), term("gamma", 3)}, 4)}, 9),
								add("Q2", {part(TITLE, {term("gamma", 1, {1})}, 1)}, 2)}),
		{});
	CHECK(v.result == strata::verdict::accepted);
	CHECK(ix.fields().size() == 4 && ix.fields().find("body") == 1u && ix.fields().key_field() == 3u);
	ix.commit();
	CHECK(df(ix, "title", "alpha") == 1 && df(ix, "body", "alpha") == 1 && df(ix, "title", "gamma") == 1 && df(ix, "body", "gamma") == 1);
	CHECK(df(ix, "title", "") == 1 && df(ix, "body", "") == 0);
	CHECK(df(ix, "body", "beta") == 0 && df(ix, "nope", "alpha") == 0);
	const auto segs = snapshot(ix);
	CHECK(segs.size() == 1);
	const auto& inv = segs[0].seg->get<inverted_plugin>();
	CHECK(inv.length(0) == 9 && inv.length(1) == 2 && inv.total_length() == 11);  // the host's lengths, by ordinal
	const auto title_alpha = inv.find_term(0, "alpha");
	CHECK(title_alpha && title_alpha->df == 1 && title_alpha->cf == 2);
	CHECK(inv.find_term(1, "gamma")->cf == 3);
	CHECK(!inv.find_term(0, "delta") && !inv.find_term(7, "alpha"));
	// The key field is the keys reader's: its terms are key leaves, never in the dictionary.
	CHECK(df(ix, "_id", "Q1") == 1 && df(ix, "_id", "Q2") == 1 && df(ix, "_id", "Q3") == 0);
	CHECK(!inv.has_field(3));
	const auto want = std::map<std::pair<std::string, std::string>, std::uint64_t>{
		{{"title", "alpha"}, 1}, {{"title", ""}, 1}, {{"title", "beta"}, 1}, {{"title", "gamma"}, 1}, {{"body", "alpha"}, 1}, {{"body", "gamma"}, 1}};
	CHECK(dictionary_of(ix) == want);
	CHECK(ix.check().documents == 2);
	ix.close();
}

static void rejection_tests() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	prospect::index::options o;
	o.key_prefix = "Q";
	auto ix = prospect::index::create(io, o, uuid);
	auto verdict_of = [&](std::vector<operation> ops, bool declare = true) {
		transaction t = tx(std::move(ops));
		if (!declare) { t.fields.clear(); }
		return ix.apply(t, {}).result;
	};
	CHECK(verdict_of({add("Q1", {part(TITLE, {term("a")})})}) == strata::verdict::accepted);
	CHECK(verdict_of({add("Q2", {part(KEY, {term("a")})})}) == strata::verdict::invalid);                           // postings on the key field
	CHECK(verdict_of({add("Q3", {part(TITLE, {term("a")}), part(TITLE, {term("b")})})}) == strata::verdict::invalid);  // a field twice
	CHECK(verdict_of({add("Q4", {part(strata::field_ref::declared(9), {term("a")})})}) == strata::verdict::invalid);   // no such declaration
	CHECK(verdict_of({add("Q5", {part(strata::field_ref::id(0), {term("a")})})}) == strata::verdict::invalid);         // an id without a mark
	CHECK(verdict_of({add("Q6", {part(TITLE, {term("a"), term("a")})})}) == strata::verdict::invalid);                 // a term twice in a field
	CHECK(verdict_of({add("Q7", {part(TITLE, {term("a")}), part(BODY, {term("a")})})}) == strata::verdict::accepted);  // once in each field
	CHECK(verdict_of({add("Q8", {part(TITLE, {term(std::string(1000, 'x'), 1, {2, 2})})})}) == strata::verdict::invalid);  // positions must increase
	ix.close();
}

// A field with 256 terms has a dictionary of its own; with 255 it is in the pool. Either way the
// segment answers the same, and a merge decides again from what survives.
static void dictionary_tests() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	prospect::index::options o;
	o.key_prefix = "Q";
	o.auto_merge = false;
	auto ix = prospect::index::create(io, o, uuid);
	std::vector<operation> ops;
	std::mt19937_64 rng(5);
	std::map<std::string, std::vector<std::string>> docs_terms;
	for (int d = 0; d < 300; ++d) {
		std::vector<term_entry> big;
		big.push_back(term("b" + std::to_string(d)));
		big.push_back(term("shared"));
		ops.push_back(add("Q" + std::to_string(d), {part(BIG, std::move(big), 2), part(TITLE, {term("t" + std::to_string(d % 7))}, 1)}));
	}
	CHECK(ix.apply(tx(std::move(ops)), {}).result == strata::verdict::accepted);
	ix.commit();
	{
		const auto& inv = ix.segments()[0].seg()->get<inverted_plugin>();
		CHECK(inv.dictionary().entry_count() == 1);  // "big" has 301 terms; "title" has 7 and is pooled
		CHECK(inv.dictionary().has_pool());
		CHECK(inv.find_term(2, "b17") && inv.find_term(2, "shared")->df == 300 && inv.find_term(0, "t3") && !inv.find_term(0, "b17"));
		inv.dictionary().check();
	}
	// Delete most documents and merge: the big field keeps 5 terms and goes to the pool or stays by the plan's bound.
	std::vector<operation> del;
	for (int d = 5; d < 300; ++d) {
		operation x;
		x.type = operation::delete_key;
		x.key = "Q" + std::to_string(d);
		del.push_back(x);
	}
	CHECK(ix.apply(tx(std::move(del)), {}).result == strata::verdict::accepted);
	ix.commit();
	const auto before = dictionary_of(ix);
	ix.force_merge();
	CHECK(dictionary_of(ix) == before);
	CHECK(df(ix, "big", "shared") == 5 && df(ix, "big", "b4") == 1 && df(ix, "big", "b5") == 0);
	CHECK(ix.check().documents == 5);
	ix.close();
}

// What the flush writes, what a merge writes and what the buffer digests agree.
static void agreement_tests() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	prospect::index::options o;
	o.key_prefix = "Q";
	o.auto_merge = false;
	auto ix = prospect::index::create(io, o, uuid);
	std::mt19937_64 rng(9);
	auto batch = [&](int base) {
		std::vector<operation> ops;
		for (int d = 0; d < 40; ++d) {
			std::set<std::string> a, b;
			for (int i = 0; i < 6; ++i) { a.insert("w" + std::to_string(rng() % 30)), b.insert("w" + std::to_string(rng() % 30)); }
			std::vector<term_entry> ta, tb;
			for (const auto& t : a) { ta.push_back(term(t)); }
			for (const auto& t : b) { tb.push_back(term(t, 1 + std::uint32_t(rng() % 3))); }
			ops.push_back(add("Q" + std::to_string(base + d), {part(TITLE, std::move(ta), 6), part(BODY, std::move(tb), 8)}));
		}
		return ops;
	};
	CHECK(ix.apply(tx(batch(0)), {}).result == strata::verdict::accepted);
	const auto buffered_digest = ix.digest();
	ix.commit();
	CHECK(ix.digest() == buffered_digest);  // the same records whether buffered or flushed
	CHECK(ix.apply(tx(batch(1000)), {}).result == strata::verdict::accepted);
	ix.commit();
	const auto before = dictionary_of(ix);
	const auto digest = ix.digest();
	CHECK(ix.segments().size() == 2);
	ix.force_merge();
	CHECK(ix.segments().size() == 1);
	CHECK(dictionary_of(ix) == before);
	CHECK(ix.digest() == digest);
	CHECK(ix.check().documents == 80);
	ix.close();
}

// A transaction names fields by its own declarations, in any order: a part's reference resolves to
// the table's id for that name, never to the reference's number (D2).
static void reference_tests() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	prospect::index::options o;
	o.key_prefix = "Q";
	auto ix = prospect::index::create(io, o, uuid);
	CHECK(ix.apply(tx({add("Q1", {part(TITLE, {term("alpha")}), part(BODY, {term("beta")})})}), {}).result == strata::verdict::accepted);
	transaction t;
	const auto all = declarations();
	t.fields = {all[1], all[0], all[2]};  // body, title, big: declared(0) is "body"
	t.ops = {add("Q2", {part(strata::field_ref::declared(0), {term("gamma")}), part(strata::field_ref::declared(1), {term("delta")})}),
		add("Q3", {part(strata::field_ref::declared(2), {term("epsilon")})})};
	CHECK(ix.apply(t, {}).result == strata::verdict::accepted);
	ix.commit();
	CHECK(ix.fields().find("title") == 0u && ix.fields().find("body") == 1u && ix.fields().find("big") == 2u);
	CHECK(df(ix, "body", "gamma") == 1 && df(ix, "title", "gamma") == 0 && df(ix, "title", "delta") == 1 && df(ix, "body", "delta") == 0);
	CHECK(df(ix, "big", "epsilon") == 1 && df(ix, "title", "epsilon") == 0 && df(ix, "body", "epsilon") == 0);
	// A snapshot of a segment without its index's field table cannot resolve a name: it throws, never answers empty.
	bool threw = false;
	try {
		const segment_snapshot bare{ix.segments()[0].seg()};
		(void)snapshot_df(std::span<const segment_snapshot>(&bare, 1), "title", "alpha");
	} catch (const std::logic_error&) {
		threw = true;
	}
	CHECK(threw);
	ix.close();
}

int main() {
	reference_tests();
	basic_tests();
	rejection_tests();
	dictionary_tests();
	agreement_tests();
	if (failures) { return 1; }
	std::puts("fields_test: ok");
	return 0;
}
