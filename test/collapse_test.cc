// Collapse (Phase 4, D9): kept documents and per-key collapse counts equal a brute force
// over random documents, by comparator and by arrival; pruning keeps counts as lower
// bounds; binary keys keep stored order where numeric and bytes keys are canonical; shards
// collapsed and merged equal one collapse of the union, and their count triples merge by
// D9's rule.
#include "strata/search/collapse.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

namespace co = strata::collapse;

namespace {

struct item {
	std::uint64_t docid = 0;
	int score = 0;
	bool operator==(const item&) const = default;
};

// Higher score first, then smaller docid: a total order over distinct docids.
struct by_score {
	bool operator()(const item& a, const item& b) const noexcept { return a.score != b.score ? a.score > b.score : a.docid < b.docid; }
};

struct doc {
	item it;
	std::optional<std::string> key;
};

using row = std::tuple<std::uint64_t, std::optional<std::string>, std::uint64_t>;  // docid, key, count

std::vector<row> rows(const co::result<item>& r) {
	std::vector<row> out;
	for (const auto& k : r.items) { out.emplace_back(k.item.docid, k.key, k.collapse_count); }
	return out;
}

// The brute force: per key the collapse_max best by score, the rest counted; keyless
// documents all kept; best first.
std::vector<row> brute(const std::vector<doc>& docs, std::size_t collapse_max) {
	std::map<std::string, std::vector<item>> groups;
	std::vector<std::pair<item, row>> kept;
	for (const auto& d : docs) {
		if (d.key) { groups[*d.key].push_back(d.it); } else { kept.push_back({d.it, row{d.it.docid, std::nullopt, 0}}); }
	}
	for (auto& [k, items] : groups) {
		std::sort(items.begin(), items.end(), by_score{});
		const std::size_t n = std::min(items.size(), collapse_max);
		for (std::size_t i = 0; i < n; ++i) { kept.push_back({items[i], row{items[i].docid, k, items.size() - n}}); }
	}
	std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return by_score{}(a.first, b.first); });
	std::vector<row> out;
	for (const auto& [it, r] : kept) { out.push_back(r); }
	return out;
}

// The number of documents left after collapsing.
std::uint64_t collapsed_count(const std::vector<doc>& docs, std::size_t collapse_max) {
	std::map<std::string, std::uint64_t> sizes;
	std::uint64_t n = 0;
	for (const auto& d : docs) {
		if (d.key) { ++sizes[*d.key]; } else { ++n; }
	}
	for (const auto& [k, s] : sizes) { n += std::min<std::uint64_t>(s, collapse_max); }
	return n;
}

std::vector<doc> random_docs(std::mt19937_64& rng) {
	const std::size_t n = 1 + rng() % 300, pool = 1 + rng() % 20;
	std::vector<doc> docs;
	for (std::size_t i = 0; i < n; ++i) {
		doc d;
		d.it = {2 * i + 1, int(rng() % 10)};
		if (rng() % 5) { d.key = "k" + std::to_string(rng() % pool); }
		docs.push_back(std::move(d));
	}
	std::shuffle(docs.begin(), docs.end(), rng);
	return docs;
}

void check_keys() {
	using sv = std::string_view;
	auto numeric = [](std::vector<std::uint64_t> v) { return co::numeric_key(v); };
	auto bytes = [](std::vector<sv> v) { return co::bytes_key(v); };
	auto binary = [](std::vector<sv> v) { return co::binary_key(v); };
	CHECK(numeric({5, 3}) == numeric({3, 5}));
	CHECK(numeric({5, 5}) != numeric({5}));
	CHECK(numeric({1, 256}) != numeric({256, 256}));
	CHECK(!numeric({}));
	CHECK(bytes({"b", "a"}) == bytes({"a", "b"}));
	CHECK(bytes({"a", "a"}) == bytes({"a"}));
	CHECK(bytes({"ab"}) != bytes({"a", "b"}));
	CHECK(bytes({""}).has_value() && bytes({""}) == bytes({"", ""}));
	CHECK(!bytes({}));
	CHECK(binary({"a", "b"}) != binary({"b", "a"}));
	CHECK(binary({"a", "a"}) != binary({"a"}));
	CHECK(binary({"ab"}) != binary({"a", "b"}));
	CHECK(binary({"a", "b"}) == binary({"a", "b"}));
	CHECK(binary({""}).has_value() && binary({""}) != binary({"", ""}));
	CHECK(!binary({}));

	// [a, b] and [b, a] collapse together as bytes, apart as binary.
	for (const bool is_binary : {false, true}) {
		co::collapser<item, by_score> c(1);
		const std::vector<sv> ab{"a", "b"}, ba{"b", "a"};
		c.add({1, 5}, is_binary ? co::binary_key(ab) : co::bytes_key(ab));
		c.add({2, 9}, is_binary ? co::binary_key(ba) : co::bytes_key(ba));
		const auto r = std::move(c).take();
		if (is_binary) {
			CHECK(r.items.size() == 2 && r.items[0].item.docid == 2 && r.items[0].collapse_count == 0 && r.items[1].collapse_count == 0);
		} else {
			CHECK(r.items.size() == 1 && r.items[0].item.docid == 2 && r.items[0].collapse_count == 1);
		}
	}

	bool refused = false;
	try { co::collapser<item> c(0); } catch (const std::invalid_argument&) { refused = true; }
	CHECK(refused);
}

void check_collapse(std::mt19937_64& rng) {
	for (int round = 0; round < 400; ++round) {
		const auto docs = random_docs(rng);
		const std::size_t collapse_max = std::vector<std::size_t>{1, 2, 3, 5}[rng() % 4];
		const auto want = brute(docs, collapse_max);

		// By comparator, in any order.
		co::collapser<item, by_score> ranked(collapse_max);
		for (const auto& d : docs) { ranked.add(d.it, d.key); }
		const auto got = std::move(ranked).take();
		CHECK(got.counts_exact);
		CHECK(rows(got) == want);

		// By arrival, offered best first.
		auto in_order = docs;
		std::sort(in_order.begin(), in_order.end(), [](const doc& a, const doc& b) { return by_score{}(a.it, b.it); });
		co::collapser<item> arrived(collapse_max);
		for (const auto& d : in_order) { arrived.add(d.it, d.key); }
		CHECK(rows(std::move(arrived).take()) == want);

		// Pruning skips documents that would not be kept: the same kept documents, counts
		// no higher than exhaustive ones, and flagged as bounds.
		std::set<std::uint64_t> kept_ids;
		for (const auto& r : want) { kept_ids.insert(std::get<0>(r)); }
		co::collapser<item, by_score> pruned(collapse_max);
		for (const auto& d : docs) {
			if (kept_ids.count(d.it.docid) || rng() % 2) { pruned.add(d.it, d.key); }
		}
		const auto bounded = std::move(pruned).take(true);
		CHECK(!bounded.counts_exact);
		const auto bounded_rows = rows(bounded);
		CHECK(bounded_rows.size() == want.size());
		for (std::size_t i = 0; i < bounded_rows.size() && i < want.size(); ++i) {
			CHECK(std::get<0>(bounded_rows[i]) == std::get<0>(want[i]));
			CHECK(std::get<1>(bounded_rows[i]) == std::get<1>(want[i]));
			CHECK(std::get<2>(bounded_rows[i]) <= std::get<2>(want[i]));
		}

		// Shards collapse, the coordinator collapses again: one collapse of the union.
		const std::size_t shards = 1 + rng() % 6;
		std::vector<std::vector<doc>> split(shards);
		for (const auto& d : docs) { split[rng() % shards].push_back(d); }
		std::vector<co::result<item>> partials;
		std::vector<co::triple> collapsed, uncollapsed;
		for (const auto& part : split) {
			co::collapser<item, by_score> c(collapse_max);
			for (const auto& d : part) { c.add(d.it, d.key); }
			partials.push_back(std::move(c).take());
			const auto n = collapsed_count(part, collapse_max);
			collapsed.push_back({n, n, n});
			uncollapsed.push_back({part.size(), part.size(), part.size()});
		}
		const auto merged = co::merge(partials, collapse_max, by_score{});
		CHECK(merged.counts_exact);
		CHECK(rows(merged) == want);
		partials.front().counts_exact = false;
		CHECK(!co::merge(partials, collapse_max, by_score{}).counts_exact);

		// Collapsed bounds hold the true count; uncollapsed ones sum exactly.
		const auto total = collapsed_count(docs, collapse_max);
		const auto c = co::merge_collapsed(collapsed);
		CHECK(c.lower <= total && total <= c.upper);
		CHECK(c.lower <= c.estimate && c.estimate <= c.upper);
		const auto u = co::merge_uncollapsed(uncollapsed);
		CHECK(u == (co::triple{docs.size(), docs.size(), docs.size()}));
	}
}

void check_triples() {
	const std::vector<co::triple> a{{1, 2, 3}, {4, 5, 6}};
	CHECK(co::merge_uncollapsed(a) == (co::triple{5, 7, 9}));
	const std::vector<co::triple> b{{3, 4, 10}, {5, 6, 7}};
	CHECK(co::merge_collapsed(b) == (co::triple{5, 10, 17}));
	const std::vector<co::triple> c{{0, 0, 1}, {7, 7, 9}};
	CHECK(co::merge_collapsed(c) == (co::triple{7, 7, 10}));
	CHECK(co::merge_collapsed(std::vector<co::triple>{}) == co::triple{});
}

}  // namespace

int main() {
	std::mt19937_64 rng(9);
	check_keys();
	check_collapse(rng);
	check_triples();
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
	std::puts("collapse: ok");
	return 0;
}
