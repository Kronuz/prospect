// expand_bench: the term expansion's classes (docs/design/automata.md, "Numbers") over a vocabulary
// in one prospect segment, per arm and query, interleaved with the arm order rotating per run.
//   expand_bench VOCAB RUNS [PER_CLASS]
// VOCAB's first column is the terms. Arms:
//   scan     every term of the literal range tested by the brute-force predicates (the shape of
//            Xapiand's expansion, not its code)
//   raw      the automaton walked over a bare termdict of the same terms: no term infos
//   walk     prospect::intersect: the field's cursor, infos decoded in the walk (D4, D5)
//   rescan   the same matcher over restarted scan_dictionary calls, find_term per match (D4's and
//            D5's alternatives)
//   expand   prospect::expand into a term_table (no limit)
// Every arm must return the same terms for every query before timing; exit 1 otherwise.
#include "automata_reference.hh"
#include "prospect/expand.hh"
#include "prospect/writer.hh"
#include "text_field.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace prospect;
using clock_type = std::chrono::steady_clock;

struct query {
	std::string cls, str;
	int kind = 0;  // 0 prefix, 1 glob, 2 levenshtein
	std::uint64_t k = 0;
	std::size_t fixed = 0;
	template <class F>
	auto with(F&& f) const {
		if (kind == 0) { return f(automata::prefix(str)); }
		if (kind == 1) { return f(automata::glob(str)); }
		return f(automata::levenshtein(str, k, fixed, true));
	}
	bool accepts(std::string_view t) const {
		if (kind == 0) { return reference::prefix(str, t); }
		if (kind == 1) { return reference::glob(str, 3, t); }
		return reference::levenshtein(str, k, fixed, true, t);
	}
};

std::vector<query> queries(const std::vector<std::string>& terms, std::size_t per_class) {
	std::mt19937_64 rng(5);
	auto draw = [&](std::size_t lo, std::size_t hi) {
		for (;;) {
			const auto& t = terms[rng() % terms.size()];
			if (t.size() >= lo && t.size() <= hi) { return t; }
		}
	};
	std::vector<query> qs;
	auto add = [&](std::string cls, std::string s, int kind, std::uint64_t k, std::size_t p) { qs.push_back({std::move(cls), std::move(s), kind, k, p}); };
	add("prefix", "obam", 0, 0, 0);
	add("glob ?", "ob?ma", 1, 0, 0);
	add("glob *x", "*ma", 1, 0, 0);
	for (unsigned k = 1; k <= 2; ++k) {
		for (std::size_t p : {0, 2}) { add("fuzzy k" + std::to_string(k) + " p" + std::to_string(p), "obama", 2, k, p); }
	}
	for (std::size_t i = 1; i < per_class; ++i) {
		add("prefix", draw(4, 12).substr(0, 3), 0, 0, 0);
		std::string g = draw(5, 10);
		g[2] = '?';
		add("glob ?", g, 1, 0, 0);
		add("glob *x", "*" + draw(4, 10).substr(1), 1, 0, 0);
		for (unsigned k = 1; k <= 2; ++k) {
			for (std::size_t p : {0, 2}) { add("fuzzy k" + std::to_string(k) + " p" + std::to_string(p), draw(5, 10), 2, k, p); }
		}
	}
	return qs;
}

// The literal range's bytes, as the automata compute it.
std::string literal(const query& q) {
	return q.with([](const auto& a) { return std::string(a.literal()); });
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: expand_bench VOCAB RUNS [PER_CLASS]\n");
		return 2;
	}
	const unsigned runs = unsigned(std::strtoul(argv[2], nullptr, 10));
	const std::size_t per_class = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 10;
	const auto t0 = clock_type::now();
	std::vector<std::string> terms;
	{
		std::ifstream in(argv[1]);
		std::string line;
		while (std::getline(in, line)) { terms.push_back(line.substr(0, line.find('\t'))); }
	}
	std::sort(terms.begin(), terms.end());
	terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
	terms.erase(std::remove_if(terms.begin(), terms.end(), [](const std::string& t) { return t.empty() || t.size() > strata::format::max_term_bytes; }), terms.end());
	// One segment: term i in document i / 64, so infos are single postings and short lists.
	segment_writer w;
	{
		std::array<std::uint32_t, 64> positions;
		for (std::uint32_t p = 0; p < 64; ++p) { positions[p] = p + 1; }
		std::vector<posting_input> in;
		std::uint64_t doc = 0;
		std::uint32_t pos = 0;
		for (std::size_t i = 0; i < terms.size(); ++i) {
			in.push_back({terms[i], 1, std::span<const std::uint32_t>(&positions[pos++], 1)});
			if (in.size() == 64 || i + 1 == terms.size()) {
				w.add(++doc, in, pos);
				in.clear();
				pos = 0;
			}
		}
	}
	const std::string file = w.flush({});
	const segment seg(file);
	const strata::deleted_bits none((seg.core().doc_count() + 63) / 64);
	const segment_snapshot snap[] = {{&seg, &none, 0, &text_table()}};
	termdict::builder tb;
	for (const auto& t : terms) { tb.add(t, {}); }
	const auto tables = tb.finish();
	const termdict::reader raw(tables.blocks, tables.index);
	const auto& inv = seg.get<inverted_plugin>();
	const auto qs = queries(terms, per_class);
	const auto t1 = clock_type::now();
	std::printf("config: vocab %s, %zu terms, segment %zu bytes, %zu queries, %u runs, NDEBUG %d, setup %.1f s\n", argv[1], terms.size(), file.size(), qs.size(), runs,
#ifdef NDEBUG
		1,
#else
		0,
#endif
		std::chrono::duration<double>(t1 - t0).count());
	if (terms.size() < 1000000 || qs.empty()) { return 2; }

	constexpr int arms = 5;
	const char* names[arms] = {"scan", "raw", "walk", "rescan", "expand"};
	auto run = [&](int arm, const query& q) {
		std::vector<std::string> out;
		switch (arm) {
		case 0: {
			const std::string lit = literal(q);
			inv.scan_dictionary(0, lit, [&](std::string_view t, std::string_view) {
				if (!t.starts_with(lit)) { return false; }
				if (q.accepts(t)) { out.emplace_back(t); }
				return true;
			});
			break;
		}
		case 1:
			q.with([&](const auto& a) {
				termdict::reader::cursor c(raw);
				return automata::walk(c, a, [&](std::string_view t) {
					out.emplace_back(t);
					return true;
				});
			});
			break;
		case 2:
			q.with([&](const auto& a) {
				return intersect(snap[0], text_name, a, [&](std::string_view t, const format::term_info& info) {
					if (info.df) { out.emplace_back(t); }
					return true;
				});
			});
			break;
		case 3:
			q.with([&](const auto& a) {
				// The matcher over scans restarted at each seek target, then a lookup per match.
				using A = std::decay_t<decltype(a)>;
				const std::string lit(a.literal());
				const auto hi = automata::successor(lit);
				std::optional<automata::matcher<A>> m;
				if constexpr (!A::byte_range) { m.emplace(a); }
				for (std::optional<std::string> from = lit; from;) {
					std::optional<std::string> next;
					inv.scan_dictionary(0, *from, [&](std::string_view key, std::string_view) {
						if (hi && key >= *hi) { return false; }
						if constexpr (A::byte_range) {
							if (inv.find_term(0, key)) { out.emplace_back(key); }
							return true;
						} else {
							const auto res = (*m)(key);
							if (res.match && inv.find_term(0, key)) { out.emplace_back(key); }
							if (!res.died) { return true; }
							auto t = m->seek();
							if (!t) { return false; }
							if (t->size() == key.size() + 1 && t->back() == '\0' && t->starts_with(key)) { return true; }
							next = std::move(t);
							return false;
						}
					});
					from = std::move(next);
				}
				return 0;
			});
			break;
		default: {
			term_table table(snap);
			out = q.with([&](const auto& a) { return expand(snap, text_name, a, {}, table); });
		}
		}
		return out;
	};
	std::vector<std::size_t> matches(qs.size());
	for (std::size_t i = 0; i < qs.size(); ++i) {  // equal answers, in full, before timing
		std::array<std::vector<std::string>, arms> got;
		for (int arm = 0; arm < arms; ++arm) { got[arm] = run(arm, qs[i]); }
		for (int arm = 1; arm < arms; ++arm) {
			if (got[arm] != got[0]) {
				std::printf("MISMATCH %s \"%s\": %s %zu, %s %zu\n", qs[i].cls.c_str(), qs[i].str.c_str(), names[0], got[0].size(), names[arm], got[arm].size());
				return 1;
			}
		}
		matches[i] = got[0].size();
	}
	std::vector<std::array<std::vector<double>, arms>> us(qs.size());
	for (unsigned rr = 0; rr < runs; ++rr) {
		for (std::size_t i = 0; i < qs.size(); ++i) {
			for (int j = 0; j < arms; ++j) {
				const int arm = int((j + rr) % arms);
				const auto start = clock_type::now();
				const auto got = run(arm, qs[i]);
				us[i][arm].push_back(std::chrono::duration<double, std::micro>(clock_type::now() - start).count());
				if (got.size() != matches[i]) { return 1; }
			}
		}
	}
	auto median = [](std::vector<double> v) {
		std::sort(v.begin(), v.end());
		return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
	};
	std::printf("\n%-12s %-14s %8s", "class", "query", "matches");
	for (const char* n : names) { std::printf(" %10s_us", n); }
	std::printf("\n");
	struct total {
		double us[arms] = {};
		std::size_t n = 0;
	};
	std::vector<std::pair<std::string, total>> by_class;
	for (std::size_t i = 0; i < qs.size(); ++i) {
		std::printf("%-12s %-14s %8zu", qs[i].cls.c_str(), qs[i].str.c_str(), matches[i]);
		auto it = std::find_if(by_class.begin(), by_class.end(), [&](const auto& e) { return e.first == qs[i].cls; });
		if (it == by_class.end()) { it = by_class.insert(by_class.end(), {qs[i].cls, {}}); }
		for (int arm = 0; arm < arms; ++arm) {
			const double m = median(us[i][arm]);
			std::printf(" %13.1f", m);
			it->second.us[arm] += m;
		}
		++it->second.n;
		std::printf("\n");
	}
	std::printf("\n%-12s %4s", "class", "n");
	for (const char* n : names) { std::printf(" %10s_ms", n); }
	std::printf(" %10s %10s %10s %10s\n", "scan/walk", "walk/raw", "rescan/walk", "expand/walk");
	for (const auto& [cls, t] : by_class) {
		std::printf("%-12s %4zu", cls.c_str(), t.n);
		for (int arm = 0; arm < arms; ++arm) { std::printf(" %13.2f", t.us[arm] / 1e3); }
		std::printf(" %10.2f %10.2f %10.2f %10.2f\n", t.us[0] / t.us[2], t.us[2] / t.us[1], t.us[3] / t.us[2], t.us[4] / t.us[2]);
	}
	std::printf("\ntimed %.1f s, total %.1f s\n", std::chrono::duration<double>(clock_type::now() - t1).count(), std::chrono::duration<double>(clock_type::now() - t0).count());
	return 0;
}
