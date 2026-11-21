// Term expansion (docs/design/automata.md D4 to D8, the automata quest's A1): prefix, glob and
// Levenshtein automata over a keyed index's segments, through a field with its own dictionaries, a
// pooled field and the key field. Against brute force:
//   1. Each segment's intersect equals the field's dictionary filtered by the reference predicates
//      (automata's test/automata_reference.hh), each info equal to find_term's, and a paused walk
//      resumed from the key it returns gives the same.
//   2. expand under no limit equals the terms of the model's live documents that match: a term held
//      only by deleted documents is never selected; each column's df is the snapshot's.
//   3. error throws exactly past the limit, first keeps the first terms, most_frequent the highest
//      snapshot dfs with ties by term order; pauses, cancellation and charges.
//   4. A term info whose df exceeds the segment's documents is corrupt.
#include "text_field.hh"
#include "automata_reference.hh"
#include "prospect/expand.hh"
#include "prospect/index.hh"
#include "prospect/writer.hh"
#include "faultlab/faultfs.h"

#include <crc32c/crc32c.hh>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 40) std::exit(1); } \
	} while (0)

namespace {

using namespace prospect;
namespace faultlab = kronuz::faultlab;

const char* const tag_name = "tag";
const char* const key_name = "_id";

std::vector<strata::field_declaration> fields() {
	return {{text_name, strata::field_role::indexed, strata::field_postings::positions, false},
		{tag_name, strata::field_role::indexed, strata::field_postings::docs, false},
		{key_name, strata::field_role::key, strata::field_postings::docs, false}};
}

// Pieces of terms: ASCII, 2-, 3- and 4-byte characters, lone and aliasing bytes.
const std::vector<std::string> pieces = {"a", "b", "c", "o", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9D\x84\x9E", "\xE2", "\xC3\xA2", "\xC3"};

std::string random_term(std::mt19937_64& r, std::size_t max_pieces) {
	std::string s;
	for (std::size_t i = 0, n = 1 + r() % max_pieces; i < n; ++i) { s += pieces[r() % pieces.size()]; }
	return s;
}

struct doc {
	std::set<std::string> text, tag;
};
using model = std::map<std::string, doc>;  // live documents by key

// A query: the automaton kind and its parameters, and the reference predicate.
struct query {
	int kind = 0;  // 0 prefix, 1 glob, 2 levenshtein
	std::string pattern;
	unsigned flags = 3;
	std::uint64_t k = 0;
	std::size_t fixed = 0;
	bool swaps = true;
	bool accepts(std::string_view t) const {
		switch (kind) {
		case 0: return reference::prefix(pattern, t);
		case 1: return reference::glob(pattern, flags, t);
		default: return reference::levenshtein(pattern, k, fixed, swaps, t);
		}
	}
	template <class F>
	auto with(F&& f) const {
		if (kind == 0) { return f(automata::prefix(pattern)); }
		if (kind == 1) { return f(automata::glob(pattern, flags)); }
		return f(automata::levenshtein(pattern, k, fixed, swaps));
	}
	std::string what() const { return std::to_string(kind) + " \"" + pattern + "\" flags " + std::to_string(flags) + " k " + std::to_string(k) + " fixed " + std::to_string(fixed); }
};

query random_query(std::mt19937_64& r, const std::vector<std::string>& terms) {
	query q;
	q.kind = int(r() % 3);
	const std::string base = !terms.empty() && r() % 2 ? terms[r() % terms.size()] : random_term(r, 4);
	if (q.kind == 0) {
		q.pattern = base.substr(0, r() % (base.size() + 1));
	} else if (q.kind == 1) {
		for (const auto& c : automata::decode(base)) { q.pattern += r() % 4 == 0 ? (r() % 2 ? "*" : "?") : std::string(c.bytes); }
		q.flags = unsigned(r() % 4);
	} else {
		q.pattern = base;
		q.k = r() % 3;
		q.fixed = r() % 3;
		q.swaps = r() % 2;
	}
	return q;
}

bool same_info(const prospect::format::term_info& a, const prospect::format::term_info& b) {
	if (a.flags != b.flags || a.df != b.df || a.cf != b.cf || a.postings != b.postings || a.positions != b.positions || a.single_wdf != b.single_wdf ||
		a.postings_length != b.postings_length || a.positions_length != b.positions_length || a.impacts.size() != b.impacts.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.impacts.size(); ++i) {
		if (a.impacts[i].wdf != b.impacts[i].wdf || a.impacts[i].length != b.impacts[i].length) { return false; }
	}
	return true;
}

// Every term of `field` in one segment, deleted documents' included, from its dictionary.
std::vector<std::string> dictionary_terms(const segment_snapshot& s, std::string_view field) {
	std::vector<std::string> out;
	const auto id = s.field_id(field);
	if (!id) { return out; }
	if (s.key_field() == id) {
		s.seg->get<keys_plugin>().scan({}, [&](std::string_view k, ordinal_t) {
			out.emplace_back(k);
			return true;
		});
	} else {
		s.seg->get<inverted_plugin>().scan_dictionary(*id, {}, [&](std::string_view t, std::string_view) {
			out.emplace_back(t);
			return true;
		});
	}
	return out;
}

std::uint64_t checked_queries = 0, dead_terms_seen = 0, own_fields = 0, pooled_fields = 0;

void check_query(std::span<const segment_snapshot> segs, const model& live, std::string_view field, const query& q, std::mt19937_64& r) {
	// 1. Each segment's walk against its dictionary.
	std::set<std::string> in_dictionaries;
	for (const auto& s : segs) {
		std::vector<std::string> want;
		for (const auto& t : dictionary_terms(s, field)) {
			in_dictionaries.insert(t);
			if (q.accepts(t)) { want.push_back(t); }
		}
		std::vector<std::string> got;
		q.with([&](const auto& a) {
			return intersect(s, field, a, [&](std::string_view t, const prospect::format::term_info& info) {
				got.emplace_back(t);
				const auto expect = find_term(s, field, t);
				CHECK(expect && same_info(info, *expect));
				return true;
			});
		});
		if (got != want) { std::fprintf(stderr, "intersect %s on %s: %zu, want %zu\n", q.what().c_str(), std::string(field).c_str(), got.size(), want.size()); }
		CHECK(got == want);
		// Paused after every few entries and resumed from the key returned.
		std::vector<std::string> resumed;
		walk_options o;
		o.check_every = 1 + r() % 3;
		o.pause = [&] { return r() % 2 == 0; };
		std::size_t walks = 0;
		for (std::optional<std::string> from = std::string(); from && ++walks < 100000;) {
			from = q.with([&](const auto& a) {
				return intersect(s, field, a, [&](std::string_view t, const prospect::format::term_info&) {
					resumed.emplace_back(t);
					return true;
				}, o, *from);
			});
		}
		CHECK(resumed == want);
	}
	// 2. The expansion against the model's live documents.
	std::set<std::string> live_terms;
	for (const auto& [key, d] : live) {
		if (field == key_name) {
			live_terms.insert(key);
		} else {
			for (const auto& t : field == text_name ? d.text : d.tag) { live_terms.insert(t); }
		}
	}
	std::vector<std::string> want;
	for (const auto& t : in_dictionaries) {
		if (!q.accepts(t)) { continue; }
		if (live_terms.count(t)) {
			want.push_back(t);
		} else {
			++dead_terms_seen;
		}
	}
	term_table table(segs);
	const auto got = q.with([&](const auto& a) { return expand(segs, field, a, {}, table); });
	if (got != want) { std::fprintf(stderr, "expand %s on %s: %zu, want %zu\n", q.what().c_str(), std::string(field).c_str(), got.size(), want.size()); }
	CHECK(got == want);
	std::vector<std::uint64_t> dfs;
	for (const auto& t : want) {
		dfs.push_back(snapshot_df(segs, field, t));
		CHECK(table.df(field, t) == dfs.back());
		for (const auto& s : segs) {
			const auto e = find_term(s, field, t);
			CHECK(table.df(*s.seg, field, t) == (e ? e->df : 0));
		}
	}
	// 3. Limits.
	const std::uint64_t max = 1 + r() % (want.size() + 2);
	{
		term_table t(segs);
		bool threw = false;
		try {
			const auto e = q.with([&](const auto& a) { return expand(segs, field, a, {expansion_limit::error, max}, t); });
			CHECK(e == want);
		} catch (const expansion_error& e) {
			threw = true;
			CHECK(e.max == max);
		}
		CHECK(threw == (want.size() > max));
	}
	{
		term_table t(segs);
		const auto e = q.with([&](const auto& a) { return expand(segs, field, a, {expansion_limit::first, max}, t); });
		CHECK(e == std::vector<std::string>(want.begin(), want.begin() + std::ptrdiff_t(std::min<std::uint64_t>(max, want.size()))));
		for (const auto& term : e) { CHECK(t.df(field, term) == snapshot_df(segs, field, term)); }
	}
	{
		std::vector<std::size_t> order(want.size());
		for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
		std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return dfs[a] > dfs[b]; });
		std::vector<std::string> top;
		for (std::size_t i = 0; i < order.size() && i < max; ++i) { top.push_back(want[order[i]]); }
		std::sort(top.begin(), top.end());
		term_table t(segs);
		CHECK(q.with([&](const auto& a) { return expand(segs, field, a, {expansion_limit::most_frequent, max}, t); }) == top);
	}
	{  // paused walks resume inside expand
		term_table t(segs);
		walk_options o;
		o.check_every = 1 + r() % 4;
		o.pause = [&] { return r() % 3 == 0; };
		CHECK(q.with([&](const auto& a) { return expand(segs, field, a, {}, t, o); }) == want);
	}
	++checked_queries;
}

// The file with one section's bytes replaced, laid out again with every checksum good.
std::string with_section(const std::string& file, strata::format::section kind, const std::string& bytes) {
	auto f = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, f);
	std::string out;
	for (auto& e : toc) {
		while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
		const std::string b = e.kind == kind ? bytes : file.substr(e.offset, e.length);
		e = strata::format::describe(e.kind, out.size(), b, e.region_shift);
		out += b;
	}
	while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
	const std::string t = strata::format::encode_toc(toc);
	f.toc_offset = out.size();
	f.toc_length = t.size();
	f.toc_crc = ::kronuz::crc32c::crc32c(t.data(), t.size());
	return out + t + strata::format::encode_footer(f);
}

// D11: a segment whose term "aa" claims more documents than the segment holds, every checksum
// good. The walk decodes the info of a match it passes and refuses it as find_term does.
void check_corrupt_df() {
	segment_writer w;
	for (std::uint64_t d = 1; d <= 3; ++d) {
		static const std::uint32_t one[] = {1}, two[] = {2};
		const std::string b = "b" + std::to_string(d);
		const std::vector<posting_input> in{{"aa", 1, one}, {b, 1, two}};
		w.add(d, in, 2);
	}
	const std::string good = w.flush({});
	const segment seg(good);
	const auto& inv = seg.get<inverted_plugin>();
	fielddict::planner plan;
	std::vector<std::tuple<std::uint32_t, std::string, prospect::format::term_info>> all;
	for (fielddict::reader::cursor c(inv.dictionary()); c.next();) {
		plan.add(c.field(), c.term());
		all.emplace_back(c.field(), std::string(c.term()), c.info());
	}
	fielddict::builder b(plan.finish());
	for (auto& [field, term, info] : all) {
		if (term == "aa") { info.df = std::uint32_t(seg.core().doc_count() + 1); }
		b.add(field, term, info);
	}
	const auto built = b.finish();
	const auto f = strata::format::decode_footer(good);
	std::string term_index;
	for (const auto& e : strata::format::decode_toc(good, f)) {
		if (e.kind == strata::format::section::term_index) { term_index = good.substr(e.offset, inverted_plugin::statistics_size); }
	}
	term_index += built.index;
	const std::string bad = with_section(with_section(good, strata::format::section::terms, built.blocks), strata::format::section::term_index, term_index);
	const segment broken(bad);
	const segment_snapshot snap[] = {{&broken, nullptr, 0, &text_table()}};
	auto refused = [&](const auto& a) {
		try {
			(void)intersect(snap[0], text_name, a, [](std::string_view, const prospect::format::term_info&) { return true; });
		} catch (const strata::corrupt_segment& e) {
			return std::string(e.what()).find("document frequency") != std::string::npos;
		}
		return false;
	};
	CHECK(refused(automata::prefix("a")));
	CHECK(refused(automata::levenshtein("ab", 1)));
	// The df check is on the matches the walk returns: a glob that passes "aa" answers.
	std::vector<std::string> got;
	(void)intersect(snap[0], text_name, automata::glob("b?"), [&](std::string_view t, const prospect::format::term_info&) {
		got.emplace_back(t);
		return true;
	});
	CHECK((got == std::vector<std::string>{"b1", "b2", "b3"}));
}

// The charge budget bounds an expansion: `prefix("")` builds no matcher, so the per-term charge is
// its only bound; `*` is charged its matcher too. Each charges at least its terms' bytes, and a
// budget below that refuses.
void check_budgets(std::span<const segment_snapshot> segs) {
	auto bytes_of = [](const std::vector<std::string>& v) {
		std::uint64_t n = 0;
		for (const auto& t : v) { n += t.size(); }
		return n;
	};
	auto charges = [&](const auto& a, std::uint64_t budget) {  // the bytes charged and the terms' bytes; none when refused
		std::uint64_t charged = 0;
		walk_options o;
		o.charge = [&](std::uint64_t n) {
			charged += n;
			if (charged > budget) { throw std::length_error("budget"); }
		};
		term_table t(segs);
		try {
			const auto got = expand(segs, text_name, a, {}, t, o);
			return std::pair{std::optional<std::uint64_t>(charged), bytes_of(got)};
		} catch (const std::length_error&) {
			return std::pair{std::optional<std::uint64_t>(), std::uint64_t(0)};
		}
	};
	auto check = [&](const auto& a) {
		const auto [charged, bytes] = charges(a, ~std::uint64_t(0));
		CHECK(charged && bytes > 0 && *charged >= bytes);
		CHECK(!charges(a, bytes - 1).first);  // a budget below the terms' bytes refuses
	};
	check(automata::prefix(""));
	check(automata::glob("*"));
}

// Cancellation requested mid-walk stops it within check_every entries.
void check_cancel_bound(std::span<const segment_snapshot> segs) {
	std::size_t best = 0, most = 0;
	for (std::size_t i = 0; i < segs.size(); ++i) {
		const auto n = dictionary_terms(segs[i], text_name).size();
		if (n > most) {
			most = n;
			best = i;
		}
	}
	const std::size_t cancel_at = 5, every = 16;
	CHECK(most > cancel_at + 2 * every);  // the walk would run on without the cancel
	task::cancel_source src;
	const task::cancel_token token = src.token();
	walk_options o;
	o.check_every = every;
	o.cancel = &token;
	std::size_t seen = 0;
	bool cancelled = false;
	try {
		(void)intersect(segs[best], text_name, automata::glob("*"), [&](std::string_view, const prospect::format::term_info&) {
			if (++seen == cancel_at) { src.request(); }
			return true;
		}, o);
	} catch (const task::cancelled&) {
		cancelled = true;
	}
	CHECK(cancelled);
	CHECK(seen > cancel_at && seen <= cancel_at + every);
}

}  // namespace

int main() {
	check_corrupt_df();
	std::mt19937_64 r(20261120);
	for (int round = 0; round < 6; ++round) {
		faultlab::filesystem fs;
		faultlab::io io(fs);
		index::options opts;
		opts.keyed = true;
		auto ix = index::create(io, opts, {});
		model live;
		// Pools of terms: the text field's many (own dictionaries), the tag field's few (pooled).
		std::vector<std::string> text_pool, tag_pool, keys;
		for (int i = 0; i < 700; ++i) { text_pool.push_back(random_term(r, 5)); }
		for (int i = 0; i < 30; ++i) { tag_pool.push_back(random_term(r, 3)); }
		for (int i = 0; i < 900; ++i) { keys.push_back("k" + random_term(r, 4)); }
		auto apply = [&](operation op) {
			transaction t;
			t.fields = fields();
			t.ops.push_back(std::move(op));
			CHECK(ix.apply(t, {}).result == strata::verdict::accepted);
		};
		const std::size_t docs = 1500 + r() % 1500;
		for (std::size_t d = 0; d < docs; ++d) {
			const std::string& key = keys[r() % keys.size()];
			doc m;
			operation op;
			op.type = operation::replace_key;
			op.key = key;
			op.doc.key = key;
			for (std::size_t i = 0, n = 1 + r() % 6; i < n; ++i) { m.text.insert(text_pool[r() % text_pool.size()]); }
			if (r() % 2) { m.tag.insert(tag_pool[r() % tag_pool.size()]); }
			std::uint32_t pos = 0;
			for (const auto& t : m.text) { op.doc.field(strata::field_ref::declared(0)).terms.push_back({t, 1, {++pos}}); }
			for (const auto& t : m.tag) { op.doc.field(strata::field_ref::declared(1)).terms.push_back({t, 1, {}}); }
			op.doc.length = pos;
			apply(std::move(op));
			live[key] = std::move(m);
			if (r() % 400 == 0) { ix.refresh(); }
			// Deletes, some of documents holding terms nobody else holds: dead dictionary entries.
			if (r() % 7 == 0 && !live.empty()) {
				auto it = live.begin();
				std::advance(it, std::ptrdiff_t(r() % live.size()));
				operation del;
				del.type = operation::delete_key;
				del.key = it->first;
				apply(std::move(del));
				live.erase(it);
			}
			if (r() % 300 == 0) {
				const std::string key2 = "k" + random_term(r, 4) + "dead";
				operation op2;
				op2.type = operation::replace_key;
				op2.key = key2;
				op2.doc.key = key2;
				op2.doc.field(strata::field_ref::declared(0)).terms.push_back({"dead" + random_term(r, 3), 1, {1}});
				op2.doc.length = 1;
				apply(std::move(op2));
				operation del;
				del.type = operation::delete_key;
				del.key = key2;
				apply(std::move(del));
			}
		}
		ix.refresh();
		std::vector<segment_snapshot> segs;
		for (const auto& v : ix.segments()) { segs.push_back({v.seg(), v.deleted, v.dead, &ix.fields()}); }
		CHECK(!segs.empty());
		for (const auto& s : segs) {
			const auto id = s.field_id(text_name), tid = s.field_id(tag_name);
			if (id && s.seg->get<inverted_plugin>().dictionary().entry_of(*id)) { ++own_fields; }
			if (tid && s.seg->get<inverted_plugin>().has_field(*tid) && !s.seg->get<inverted_plugin>().dictionary().entry_of(*tid)) { ++pooled_fields; }
		}
		std::vector<std::string> sample(text_pool.begin(), text_pool.end());
		sample.insert(sample.end(), tag_pool.begin(), tag_pool.end());
		sample.insert(sample.end(), keys.begin(), keys.end());
		for (int qn = 0; qn < 60; ++qn) {
			const query q = random_query(r, sample);
			for (const char* field : {text_name, tag_name, key_name}) { check_query(segs, live, field, q, r); }
		}
		// Fixed patterns that walk everything or nothing.
		for (const char* p : {"", "*", "?", "*a", "dead*", "k*"}) {
			query q;
			q.kind = 1;
			q.pattern = p;
			for (const char* field : {text_name, tag_name, key_name}) { check_query(segs, live, field, q, r); }
		}
		check_budgets(segs);
		check_cancel_bound(segs);
		// Cancellation stops a walk at its next check, charges are offered and may refuse.
		{
			task::cancel_source src;
			const task::cancel_token token = src.token();
			src.request();
			walk_options o;
			o.check_every = 1;
			o.cancel = &token;
			term_table t(segs);
			bool cancelled = false;
			try {
				(void)expand(segs, text_name, automata::glob("*"), {}, t, o);
			} catch (const task::cancelled&) {
				cancelled = true;
			}
			CHECK(cancelled);
			std::uint64_t charged = 0;
			walk_options c;
			c.charge = [&](std::uint64_t n) { charged += n; };
			term_table u(segs);
			const auto all = expand(segs, text_name, automata::levenshtein("abc", 2), {}, u, c);
			CHECK(charged > 0);
			walk_options refuse;
			refuse.charge = [](std::uint64_t) { throw std::length_error("budget"); };
			bool refused = false;
			try {
				term_table v(segs);
				(void)expand(segs, text_name, automata::levenshtein("abc", 2), {}, v, refuse);
			} catch (const std::length_error&) {
				refused = true;
			}
			CHECK(refused);
		}
	}
	CHECK(dead_terms_seen > 0);  // the liveness rule was exercised
	CHECK(own_fields > 0 && pooled_fields > 0);
	std::printf("expand: %llu queries, %llu matched terms with no live document skipped, %llu own and %llu pooled field dictionaries, %d failures\n",
		(unsigned long long)checked_queries, (unsigned long long)dead_terms_seen, (unsigned long long)own_fields, (unsigned long long)pooled_fields, failures);
	return failures ? 1 : 0;
}
