// The field dictionaries (fields-and-keys D3, D10): the field id varint, the builder and reader
// against a model (own dictionaries, the pool, the empty term, gaps and large ids), the cursor's
// order, and a directory or index with any byte changed, which reads or throws corrupt_segment.
#include "prospect/fielddict.hh"

#include <algorithm>
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
using dict_model = std::map<std::pair<std::uint32_t, std::string>, prospect::format::term_info>;

static std::string field_bytes(std::uint32_t id) {
	std::string s;
	fielddict::put_field(s, id);
	return s;
}

static void varint_tests() {
	const std::uint32_t ids[] = {0, 1, 100, 239, 240, 241, 255, 256, 65535, 65536, (1u << 24) - 1, 1u << 24, 0xFFFFFFFFu};
	for (std::uint32_t id : ids) {
		const std::string s = field_bytes(id);
		char buf[5];  // the buffer overload (the tuple buffer's keys) writes the same bytes
		CHECK(std::string(buf, fielddict::put_field(buf, id)) == s);
		const char* p = s.data();
		std::uint32_t got = 0;
		CHECK(fielddict::get_field(p, s.data() + s.size(), got) && got == id && p == s.data() + s.size());
		CHECK(s.size() == (id < 240 ? 1u : id < 256 ? 2u : id < 65536 ? 3u : id < (1u << 24) ? 4u : 5u));
		for (std::uint32_t other : ids) {  // byte order is numeric order, and no encoding is a prefix of another
			const std::string o = field_bytes(other);
			CHECK((s < o) == (id < other));
			if (id != other) { CHECK(!o.starts_with(s)); }
		}
		for (std::size_t cut = 0; cut < s.size(); ++cut) {  // a truncated form is refused
			const char* q = s.data();
			std::uint32_t v;
			CHECK(!fielddict::get_field(q, s.data() + cut, v));
		}
	}
	const std::string bad[] = {
		std::string("\xF5\x01\x01\x01\x01\x01", 6), std::string("\xFF", 1), std::string("\xF0", 1),      // lead bytes 0xF5 to 0xFF; n of 0
		std::string("\xF1\x05", 2), std::string("\xF1\xEF", 2), std::string("\xF2\x00\xF0", 3),          // a long form below 240; a leading zero
		std::string("\xF3\x00\x01\x00", 4), std::string("\xF4\x00\xFF\xFF\xFF", 5)};
	for (const auto& s : bad) {
		const char* p = s.data();
		std::uint32_t v;
		CHECK(!fielddict::get_field(p, s.data() + s.size(), v));
	}
}

static prospect::format::term_info random_info(std::mt19937_64& rng) {
	prospect::format::term_info t;
	t.flags = prospect::format::no_positions;
	t.df = std::uint32_t(1 + rng() % (rng() % 8 == 0 ? 300 : 5));
	t.cf = t.df + rng() % 3;
	if (t.df == 1) {
		t.flags |= prospect::format::single_posting;
		t.postings = rng() % 1000;
		t.single_wdf = std::uint32_t(t.cf);
	} else {
		t.postings = rng() % 100000;
		t.postings_length = 1 + rng() % 400;
	}
	if (t.df >= prospect::format::block) { t.impacts.push_back({1, 3}); }
	return t;
}

static bool same(const prospect::format::term_info& a, const prospect::format::term_info& b) {
	return a.df == b.df && a.cf == b.cf && a.postings == b.postings && a.single_wdf == b.single_wdf && a.postings_length == b.postings_length &&
		a.impacts.size() == b.impacts.size() && (a.flags & prospect::format::single_posting) == (b.flags & prospect::format::single_posting);
}

struct built_dict {
	std::string terms, index;
};

static built_dict build(const dict_model& model) {
	fielddict::planner plan;
	for (const auto& [k, _] : model) { plan.add(k.first, k.second); }
	fielddict::builder b(plan.finish());
	for (const auto& [k, info] : model) { b.add(k.first, k.second, info); }
	auto built = b.finish();
	return {std::move(built.blocks), std::move(built.index)};
}

static std::optional<prospect::format::term_info> find(const fielddict::reader& r, std::uint32_t field, const std::string& term) {
	prospect::format::term_run run, before;
	std::string_view last;
	const auto v = r.find_run(field, term, [&](std::string_view value) {
		before = run;
		(void)prospect::format::decode_term_info(value, run, false);
		last = value;
	});
	if (!v) { return std::nullopt; }
	return prospect::format::decode_term_info(last, before);
}

// A random model: fields of every size around the threshold, large ids, the empty term.
static dict_model random_model(std::mt19937_64& rng, unsigned fields) {
	dict_model m;
	std::uint32_t id = std::uint32_t(rng() % 3);
	for (unsigned f = 0; f < fields; ++f) {
		id += 1 + std::uint32_t(rng() % (rng() % 5 == 0 ? 100000 : 3));
		const unsigned sizes[] = {1, 2, 20, 255, 256, 257, 700};
		const unsigned n = sizes[rng() % 7];
		const std::string alphabet = rng() % 2 ? "abcdefgh" : std::string("\x00\x01\xFE\xFF", 4) + "xyz";
		std::set<std::string> terms;
		if (rng() % 4 == 0) { terms.insert(""); }
		for (unsigned tries = 0; terms.size() < n && tries < 20 * n; ++tries) {
			std::string t;
			for (unsigned i = 1 + rng() % 6; i > 0; --i) { t.push_back(alphabet[rng() % alphabet.size()]); }
			terms.insert(t);
		}
		for (const auto& t : terms) { m[{id, t}] = random_info(rng); }
	}
	return m;
}

static void dict_modelests() {
	std::mt19937_64 rng(7);
	for (unsigned round = 0; round < 40; ++round) {
		const dict_model m = random_model(rng, 1 + round % 25);
		const built_dict d = build(m);
		fielddict::reader r(d.terms, d.index);
		r.check();
		std::map<std::uint32_t, std::size_t> sizes;
		for (const auto& [k, _] : m) { ++sizes[k.first]; }
		std::size_t own = 0;
		for (const auto& [f, n] : sizes) {
			CHECK(r.entry_of(f).has_value() == (n >= fielddict::own_threshold));
			own += n >= fielddict::own_threshold;
			CHECK(r.has_field(f));
		}
		CHECK(r.entry_count() == own);
		CHECK(r.has_pool() == (own != sizes.size()));
		for (const auto& [k, info] : m) {
			const auto got = find(r, k.first, k.second);
			CHECK(got && same(*got, info));
			CHECK(find(r, k.first, k.second + "zz").has_value() == (m.count({k.first, k.second + "zz"}) != 0));
		}
		CHECK(!r.has_field(sizes.rbegin()->first + 1) && !r.has_field(0xFFFFFFFFu));
		// A field's scan is its terms, in order, from a lower bound, and nothing of its neighbors.
		for (const auto& [f, n] : sizes) {
			std::vector<std::string> got, want;
			for (auto it = m.lower_bound({f, ""}); it != m.end() && it->first.first == f; ++it) { want.push_back(it->first.second); }
			r.scan(f, "", [&](std::string_view t, std::string_view) { return got.push_back(std::string(t)), true; });
			CHECK(got == want);
			got.clear();
			const std::string from = want[want.size() / 2];
			r.scan(f, from, [&](std::string_view t, std::string_view) { return got.push_back(std::string(t)), true; });
			CHECK(got == std::vector<std::string>(std::lower_bound(want.begin(), want.end(), from), want.end()));
		}
		// The cursor visits every (field, term) in order with its info.
		fielddict::reader::cursor c(r);
		auto it = m.begin();
		while (c.next()) {
			CHECK(it != m.end() && c.field() == it->first.first && c.term() == it->first.second && same(c.info(), it->second));
			if (it != m.end()) { ++it; }
		}
		CHECK(it == m.end());
	}
}

static void threshold_tests() {
	std::mt19937_64 rng(3);
	for (unsigned n : {255u, 256u}) {  // the segment's count decides: 255 pools, 256 has a dictionary of its own
		dict_model m;
		for (unsigned i = 0; i < n; ++i) {
			char t[8];
			std::snprintf(t, sizeof t, "t%04u", i);
			m[{9, t}] = random_info(rng);
		}
		m[{12, ""}] = random_info(rng);  // the empty term, pooled
		const built_dict d = build(m);
		fielddict::reader r(d.terms, d.index);
		CHECK(r.entry_of(9).has_value() == (n == 256));
		CHECK(!r.entry_of(12));
		CHECK(find(r, 12, "") && same(*find(r, 12, ""), m[{12, ""}]));
		CHECK(!find(r, 9, "") && !find(r, 11, "t0001"));
	}
	dict_model m;  // the empty term in a dictionary of its own
	for (unsigned i = 0; i < 300; ++i) { m[{4, i ? std::to_string(1000 + i) : std::string()}] = random_info(rng); }
	const built_dict d = build(m);
	fielddict::reader r(d.terms, d.index);
	CHECK(r.entry_of(4) && find(r, 4, "") && same(*find(r, 4, ""), m[{4, ""}]));
	fielddict::reader::cursor c(r);
	CHECK(c.next() && c.field() == 4 && c.term().empty());
}

static void builder_misuse_tests() {
	std::mt19937_64 rng(5);
	const prospect::format::term_info info = random_info(rng);
	auto throws = [](auto&& f) {
		try {
			f();
		} catch (const std::invalid_argument&) {
			return true;
		}
		return false;
	};
	CHECK(throws([&] { fielddict::builder b({{1, 2, {}}, {1, 2, {}}}); }));  // fields must increase
	CHECK(throws([&] {
		fielddict::builder b({{1, 1, {}}});
		b.add(2, "a", info);
	}));
	CHECK(throws([&] {
		fielddict::byte_set s;
		s.add("ab");
		fielddict::builder b({{1, 1, s}});
		b.add(1, "a", info);
		b.add(1, "b", info);  // more terms than planned
		(void)b.finish();
	}));
	{  // a plan is an upper bound: a planned field may get fewer terms or none
		fielddict::byte_set s;
		s.add("abc");
		fielddict::builder b({{1, 2, s}, {2, 5, s}, {3, 9, s}});
		b.add(1, "a", info);
		b.add(3, "b", info);
		const auto built = b.finish();
		fielddict::reader r(built.blocks, built.index);
		r.check();
		CHECK(find(r, 1, "a") && !find(r, 2, "a") && find(r, 3, "b"));
	}
	CHECK(throws([&] {
		fielddict::byte_set s;
		s.add("a");
		fielddict::builder b({{1, 1, s}});
		b.add(1, "b", info);  // a byte outside the alphabet
	}));
}

// Every byte of the directory and of the section changed to every other value in turn: reading
// either works or throws corrupt_segment, never crashes (run under ASan and UBSan).
static void damage_tests() {
	std::mt19937_64 rng(11);
	dict_model m = random_model(rng, 6);
	for (unsigned i = 0; i < 300; ++i) { m[{3, std::to_string(100000 + i)}] = random_info(rng); }
	const built_dict d = build(m);
	auto read_all = [&](const std::string& terms, const std::string& index) {
		try {
			fielddict::reader r(terms, index);
			r.check();
			for (const auto& [k, _] : m) { (void)find(r, k.first, k.second); }
			fielddict::reader::cursor c(r);
			while (c.next()) {}
			for (std::uint32_t f = 0; f < 6; ++f) { r.scan(f, "", [](std::string_view, std::string_view) { return true; }); }
		} catch (const strata::corrupt_segment&) {
		}
	};
	for (std::size_t at = 0; at < d.index.size(); at += 1 + (at > 200 ? 7 : 0)) {
		std::string index = d.index;
		for (unsigned v : {0u, 1u, 0x7Fu, 0x80u, 0xFFu, unsigned(std::uint8_t(index[at])) ^ 0x55u}) {
			index[at] = char(v);
			read_all(d.terms, index);
		}
	}
	for (std::size_t cut = 0; cut < d.index.size(); cut += 1 + (cut > 200 ? 13 : 0)) { read_all(d.terms, d.index.substr(0, cut)); }
	for (std::size_t at = 0; at < d.terms.size(); at += 1 + d.terms.size() / 400) {
		std::string terms = d.terms;
		terms[at] = char(terms[at] ^ 0xA5);
		read_all(terms, d.index);
	}
	for (std::size_t cut = 0; cut < d.terms.size(); cut += 1 + d.terms.size() / 200) { read_all(d.terms.substr(0, cut), d.index); }
	// A verifier that refuses every range surfaces as corrupt_segment.
	fielddict::reader r(d.terms, d.index, [](void*, std::uint64_t, std::uint64_t) { return false; }, nullptr);
	bool threw = false;
	try {
		(void)find(r, 3, "100001");
	} catch (const strata::corrupt_segment&) {
		threw = true;
	}
	CHECK(threw);
}

// A directory entry that claims another term count than its dictionary holds is refused by check(),
// which reads the terms; lookups alone cannot tell.
static void count_tests() {
	std::mt19937_64 rng(13);
	dict_model m;
	for (unsigned i = 0; i < 300; ++i) { m[{3, std::to_string(100000 + i)}] = random_info(rng); }
	m[{9, "x"}] = random_info(rng);
	const built_dict d = build(m);
	{
		fielddict::reader r(d.terms, d.index);
		r.check();
	}
	const char* p = d.index.data();
	const char* const end = p + d.index.size();
	const std::uint64_t own_blocks = prospect::format::get_leb(&p, end), entries = prospect::format::get_leb(&p, end);
	(void)prospect::format::get_leb(&p, end), (void)prospect::format::get_leb(&p, end);
	CHECK(own_blocks > 0 && entries == 1);
	p += ((entries + fielddict::sample_every - 1) / fielddict::sample_every) * fielddict::sample_bytes;
	(void)prospect::format::get_leb(&p, end);  // the entry: its field, then flags, then its term count (300: two bytes)
	++p;
	std::string index = d.index;
	char& low = index[std::size_t(p - d.index.data())];
	CHECK((std::uint8_t(low) & 0x7F) == (300 & 0x7F) && (std::uint8_t(low) & 0x80));
	low = char(std::uint8_t(low) + 1);
	bool threw = false;
	try {
		fielddict::reader r(d.terms, index);
		for (const auto& [k, _] : m) { CHECK(find(r, k.first, k.second)); }  // every lookup still answers
		r.check();
	} catch (const strata::corrupt_segment&) {
		threw = true;
	}
	CHECK(threw);
}

int main() {
	count_tests();
	varint_tests();
	dict_modelests();
	threshold_tests();
	builder_misuse_tests();
	damage_tests();
	if (failures) { return 1; }
	std::puts("fielddict_test: ok");
	return 0;
}
