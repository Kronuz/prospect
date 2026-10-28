// A reference oracle for the field dictionary (fielddict.hh): random (field, terms) sets, built
// into a dictionary, and every read path compared with a std::map: the cursor, find_run, scan,
// has_field, entry_of, add_plan and the deep check. The generator covers field ids around every
// varint width, term counts around the own threshold, the empty term, full-byte alphabets, and
// wide directories (17 to 76 own dictionaries between pooled fields: several directory samples).
// `against_truth` is for a valid dictionary; `agree` is the rule for a damaged one that
// check() accepted: its read paths agree with one another. Shared by fielddict_oracle_test and
// fuzz_fielddict.
#pragma once

#include "prospect/fielddict.hh"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace prospect::fielddict::oracle {

using truth_t = std::map<std::uint32_t, std::vector<std::string>>;

// Violation classes seen, with a count and the context of the first.
struct report {
	std::map<std::string, std::uint64_t> classes;
	std::string context;
	std::map<std::string, std::string> first;
	void violate(const std::string& what) {
		if (classes[what]++ == 0) { first[what] = context; }
	}
	bool ok() const noexcept { return classes.empty(); }
	void print() const {
		for (const auto& [k, n] : classes) { std::fprintf(stderr, "  %llu x %s [%s]\n", (unsigned long long)n, k.c_str(), first.at(k).c_str()); }
	}
};

inline format::term_info info_of(std::uint32_t n) {
	format::term_info t;
	t.flags = format::single_posting | format::no_wdf | format::no_positions;
	t.df = 1;
	t.cf = 0;
	t.postings = n % 50;
	return t;
}

inline builder::built build(const truth_t& truth) {
	planner plan;
	for (const auto& [f, ts] : truth) {
		for (const auto& t : ts) { plan.add(f, t); }
	}
	builder b(plan.finish());
	std::uint32_t n = 0;
	for (const auto& [f, ts] : truth) {
		for (const auto& t : ts) { b.add(f, t, info_of(n++)); }
	}
	return b.finish();
}

inline truth_t random_truth(std::mt19937_64& rng) {
	truth_t m;
	static const std::uint32_t edge[] = {0, 1, 2, 7, 238, 239, 240, 241, 254, 255, 256, 257, 4095, 65534, 65535, 65536, 65537, 0xFFFFFE, 0xFFFFFF, 0x1000000, 0x1000001, 0x7FFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF};
	static const std::size_t sizes[] = {1, 1, 2, 3, 5, 17, 100, 254, 255, 256, 257, 300, 700};
	if (rng() % 8 == 0) {  // wide: many own dictionaries (several directory samples), some pooled fields between
		const unsigned own = 17 + unsigned(rng() % 60);
		std::uint32_t id = std::uint32_t(rng() % 3);
		for (unsigned k = 0; k < own + own / 3; ++k) {
			id += 1 + std::uint32_t(rng() % (rng() % 4 == 0 ? 300 : 3));
			const bool big = k % 4 != 3;
			auto& dst = m[id];
			for (unsigned t = 0; t < (big ? 256 + rng() % 3 : 1 + rng() % 5); ++t) {
				char b[12];
				std::snprintf(b, sizeof b, "t%04u", t * 3 + unsigned(rng() % 3));
				dst.push_back(b);
			}
			std::sort(dst.begin(), dst.end());
			dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
		}
		return m;
	}
	const unsigned nf = rng() % 5 == 0 ? 40 + unsigned(rng() % 300) : 1 + unsigned(rng() % 14);
	const bool small_alpha = rng() % 2;
	for (unsigned k = 0; k < nf; ++k) {
		const std::uint32_t id = rng() % 3 == 0 ? edge[rng() % std::size(edge)] : rng() % 4 == 0 ? std::uint32_t(rng()) : std::uint32_t(rng() % (nf * 2));
		const std::size_t want = nf > 40 ? 1 + rng() % 4 : sizes[rng() % std::size(sizes)];
		std::set<std::string> terms;
		if (rng() % 3 == 0) { terms.insert(""); }
		const unsigned alpha = small_alpha || rng() % 2 ? 3 + unsigned(rng() % 5) : 256;
		for (unsigned tries = 0; terms.size() < want && tries < want * 40; ++tries) {
			std::string t(rng() % 10, 'a');
			for (auto& c : t) { c = alpha == 256 ? char(rng()) : char((rng() % 4 == 0 ? 0xF0 : 'a') + rng() % alpha); }
			terms.insert(t);
		}
		auto& dst = m[id];
		for (const auto& t : terms) { dst.push_back(t); }
		std::sort(dst.begin(), dst.end());
		dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
	}
	return m;
}

// Everything the cursor says, capped.
struct listing {
	std::vector<std::tuple<std::uint32_t, std::string, std::string>> items;  // field, term, raw value
	bool truncated = false;
};
inline listing list(const reader& r, std::size_t cap = 200000) {
	listing l;
	reader::cursor c(r);
	while (c.next()) {
		if (l.items.size() >= cap) {
			l.truncated = true;
			break;
		}
		l.items.emplace_back(c.field(), std::string(c.term()), std::string(c.value()));
	}
	return l;
}

// A valid dictionary against `truth`: every read path. Throws what the reader throws.
inline void against_truth(const reader& r, const truth_t& truth, std::mt19937_64& rng, report& rep) {
	const listing l = list(r);
	std::vector<std::pair<std::uint32_t, std::string>> want;
	for (const auto& [f, ts] : truth) {
		for (const auto& t : ts) { want.emplace_back(f, t); }
	}
	if (l.items.size() != want.size()) { rep.violate("cursor: count differs from the reference"); }
	for (std::size_t i = 0; i < std::min(l.items.size(), want.size()); ++i) {
		if (std::get<0>(l.items[i]) != want[i].first) { rep.violate("cursor: term filed under the wrong field"); }
		else if (std::get<1>(l.items[i]) != want[i].second) { rep.violate("cursor: wrong term"); }
	}
	std::map<std::pair<std::uint32_t, std::string>, std::string> raw;
	for (const auto& [f, t, v] : l.items) { raw[{f, t}] = v; }
	for (const auto& [f, ts] : truth) {
		const auto e = r.entry_of(f);
		if (e.has_value() != (ts.size() >= own_threshold)) { rep.violate("entry_of: own/pool decision differs from the threshold"); }
		if (e && e->terms != ts.size()) { rep.violate("entry_of: term count"); }
		if (!r.has_field(f)) { rep.violate("has_field false for a field with terms"); }
		for (const auto& t : ts) {
			const auto v = r.find_run(f, t, [](std::string_view) {});
			if (!v) { rep.violate("find_run: present term not found"); continue; }
			if (std::string(*v) != raw[{f, t}]) { rep.violate("find_run: value differs from the cursor's"); }
		}
		for (int k = 0; k < 3; ++k) {  // scan from a term, from just past one, and from the start
			std::string from = ts[rng() % ts.size()];
			if (k == 1 && !from.empty()) { from.back() = char(from.back() + 1); }
			if (k == 2) { from.clear(); }
			std::vector<std::string> got;
			r.scan(f, from, [&](std::string_view term, std::string_view) {
				got.emplace_back(term);
				return got.size() < 100000;
			});
			const std::vector<std::string> exp(std::lower_bound(ts.begin(), ts.end(), from), ts.end());
			if (got != exp) { rep.violate("scan: terms from a point differ from the reference (field leak or loss)"); }
		}
	}
	// Absent lookups: neighbours of every field, and every field of the reference with another's term.
	std::set<std::uint32_t> probes;
	for (const auto& [f, ts] : truth) {
		probes.insert(f);
		probes.insert(f + 1);
		probes.insert(f - 1);
	}
	for (int k = 0; k < 6; ++k) { probes.insert(std::uint32_t(rng())); }
	for (const std::uint32_t f : probes) {
		const auto it = truth.find(f);
		const bool has = it != truth.end();
		if (!has) {
			if (r.has_field(f)) { rep.violate("has_field true for an absent field"); }
			if (r.entry_of(f)) { rep.violate("entry_of for an absent field"); }
			std::size_t n = 0;
			r.scan(f, {}, [&](std::string_view, std::string_view) { return ++n < 10; });
			if (n) { rep.violate("scan yields terms of an absent field"); }
		}
		for (const auto& [g, ts] : truth) {
			if (g == f) { continue; }
			const std::string& t = ts[rng() % ts.size()];
			const bool member = has && std::binary_search(it->second.begin(), it->second.end(), t);
			if (r.find_run(f, t, [](std::string_view) {}).has_value() != member) { rep.violate("find_run: another field's term found or lost wrongly"); }
		}
		if (has) {
			const std::string t = it->second[rng() % it->second.size()] + "\x01";
			if (!std::binary_search(it->second.begin(), it->second.end(), t) && r.find_run(f, t, [](std::string_view) {})) { rep.violate("find_run: absent term found"); }
		}
	}
	std::map<std::uint32_t, field_plan> plan;
	r.add_plan(plan);
	if (plan.size() != truth.size()) { rep.violate("add_plan: field count"); }
	for (const auto& [f, p] : plan) {
		const auto it = truth.find(f);
		if (it == truth.end()) { rep.violate("add_plan: invented field"); continue; }
		if (p.terms != it->second.size()) { rep.violate("add_plan: term count"); }
		byte_set a;
		for (const auto& t : it->second) { a.add(t); }
		for (unsigned c = 0; c < 256; ++c) {
			if (a.has(std::uint8_t(c)) && !p.alphabet.has(std::uint8_t(c))) { rep.violate("add_plan: alphabet misses a byte"); }
		}
	}
	r.check();
}

// The rule for a dictionary check() accepted, whatever its bytes: the cursor ends, reads in
// order, and scan, find_run, entry_of, has_field and add_plan agree with it.
inline void agree(const reader& r, std::mt19937_64& rng, report& rep) {
	const listing l = list(r, 20000);
	if (l.truncated) { rep.violate("check accepted, cursor never ends"); }
	std::map<std::uint32_t, std::vector<std::string>> by_field;
	std::map<std::pair<std::uint32_t, std::string>, std::string> raw;
	for (std::size_t i = 0; i < l.items.size(); ++i) {
		const auto& [f, t, v] = l.items[i];
		if (i) {
			const auto& [pf, pt, pv] = l.items[i - 1];
			if (f < pf || (f == pf && t <= pt)) { rep.violate("check accepted, cursor out of order"); }
		}
		by_field[f].push_back(t);
		raw[{f, t}] = v;
	}
	if (l.truncated) { return; }
	for (const auto& [f, ts] : by_field) {
		const auto e = r.entry_of(f);
		if (e && e->terms != ts.size()) { rep.violate("check accepted, an entry's term count differs from the cursor's"); }
		if (!r.has_field(f)) { rep.violate("check accepted, has_field false for a cursor field"); }
		std::vector<std::string> got;
		r.scan(f, {}, [&](std::string_view t, std::string_view) {
			got.emplace_back(t);
			return true;
		});
		if (got != ts) { rep.violate("check accepted, scan(field) differs from the cursor's terms of that field"); }
		for (std::size_t k = 0; k < std::min<std::size_t>(ts.size(), 30); ++k) {
			const auto& t = ts[rng() % ts.size()];
			const auto v = r.find_run(f, t, [](std::string_view) {});
			if (!v || std::string(*v) != raw[{f, t}]) { rep.violate("check accepted, find_run differs from the cursor"); }
		}
	}
	std::map<std::uint32_t, field_plan> plan;
	r.add_plan(plan);
	if (plan.size() != by_field.size()) { rep.violate("check accepted, add_plan's field set differs from the cursor's"); }
	for (const auto& [f, p] : plan) {
		if (p.terms != by_field[f].size()) { rep.violate("check accepted, add_plan's term count differs from the cursor's"); }
	}
}

// Where the termdict's own bytes (alphabet and FST of the own dictionaries and the pool) begin in
// a built index: before it lie the directory's header, samples and entries.
inline std::size_t termdict_bytes_start(const std::string& index) {
	const char* p = index.data();
	const char* end = p + index.size();
	(void)format::get_leb(&p, end);
	const std::uint64_t entries = format::get_leb(&p, end), entry_bytes = format::get_leb(&p, end);
	(void)format::get_leb(&p, end);
	return std::size_t(p - index.data()) + std::size_t((entries + sample_every - 1) / sample_every) * sample_bytes + std::size_t(entry_bytes);
}

}  // namespace prospect::fielddict::oracle
