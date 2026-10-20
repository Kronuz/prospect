// Sort keys (Phase 4, D8): top-k equals a brute-force sort by (key tuple, docid) at k = 1,
// 10, and 100 over random multi-valued documents with missing values, mixed directions,
// and composite keys; shards' top-k lists merge to the single-shard answer; the wire form
// round trips and rejects malformed input; block skipping never skips a competitor.
#include "strata/search/sortkey.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

namespace sk = strata::sortkey;

namespace {

// Slot 0 holds ordered u64 keys, slot 1 bytes, slot 2 doubles for distance components.
struct doc {
	std::uint64_t docid = 0;
	std::vector<std::uint64_t> nums;
	std::vector<std::string> words;
	std::vector<double> points;
};

sk::key key_of(const sk::spec& spec, const doc& d) {
	sk::key k;
	for (const auto& c : spec) {
		if (c.slot == 0) {
			k.push_back(sk::select_u64(c, d.nums));
		} else if (c.slot == 1) {
			const std::vector<std::string_view> views(d.words.begin(), d.words.end());
			k.push_back(sk::select_bytes(c, views));
		} else {
			k.push_back(sk::select_distance(c, d.points));
		}
	}
	return k;
}

// The oracle, written apart from sortkey's comparison: each component's selected value as
// an optional, missing last, then docid.
template <typename T>
std::optional<T> pick(const std::vector<T>& v, bool largest) {
	if (v.empty()) { return std::nullopt; }
	return largest ? *std::max_element(v.begin(), v.end()) : *std::min_element(v.begin(), v.end());
}
template <typename T>
int compare_opt(const std::optional<T>& x, const std::optional<T>& y, bool descending) {
	if (!x || !y) { return int(!x) - int(!y); }
	if (*x == *y) { return 0; }
	return (*x < *y) != descending ? -1 : 1;
}
bool oracle_before(const sk::spec& spec, const doc& a, const doc& b) {
	for (const auto& c : spec) {
		const bool desc = c.dir == sk::direction::descending;
		int r = 0;
		if (c.slot == 0) {
			r = compare_opt(pick(a.nums, desc), pick(b.nums, desc), desc);
		} else if (c.slot == 1) {
			r = compare_opt(pick(a.words, desc), pick(b.words, desc), desc);
		} else {
			auto dist = [&](const doc& d) {
				std::vector<double> ds;
				for (const double p : d.points) { ds.push_back(std::fabs(c.reference - p)); }
				return pick(ds, c.rule == sk::selection::farthest);
			};
			r = compare_opt(dist(a), dist(b), desc);
		}
		if (r) { return r < 0; }
	}
	return a.docid < b.docid;
}

const std::vector<std::string>& alphabet() {
	static const std::vector<std::string> words{std::string(), "a", "ab", "b", std::string("\0b", 2), std::string("\xff", 1), std::string("a\xff", 2), "ba"};
	return words;
}

doc random_doc(std::mt19937_64& rng, std::uint64_t docid, bool infinities) {
	doc d;
	d.docid = docid;
	for (int n = int(rng() % 4); n > 0; --n) {
		const auto r = rng() % 40;
		d.nums.push_back(r == 0 ? 0 : r == 1 ? std::numeric_limits<std::uint64_t>::max() : r % 12);
	}
	for (int n = int(rng() % 4); n > 0; --n) { d.words.push_back(alphabet()[rng() % alphabet().size()]); }
	for (int n = int(rng() % 4); n > 0; --n) {
		const auto r = rng() % 30;
		const double inf = std::numeric_limits<double>::infinity();
		d.points.push_back(infinities && r == 0 ? inf : infinities && r == 1 ? -inf : double(int(rng() % 21) - 10) / 2);
	}
	return d;
}

sk::component random_component(std::mt19937_64& rng, std::uint32_t slot, bool infinities) {
	const auto dir = rng() % 2 ? sk::direction::descending : sk::direction::ascending;
	if (slot == 0) { return sk::component::by_value(0, sk::type::u64, dir); }
	if (slot == 1) { return sk::component::by_value(1, sk::type::bytes, dir); }
	const auto r = rng() % 12;
	const double inf = std::numeric_limits<double>::infinity();
	const double reference = infinities && r == 0 ? inf : infinities && r == 1 ? -inf : double(int(rng() % 25) - 12) / 2;
	return sk::component::by_distance(2, reference, rng() % 2 ? sk::selection::farthest : sk::selection::closest, dir);
}

sk::spec random_spec(std::mt19937_64& rng, bool infinities) {
	sk::spec s;
	for (int n = 1 + int(rng() % 3); n > 0; --n) { s.push_back(random_component(rng, std::uint32_t(rng() % 3), infinities)); }
	return s;
}

std::vector<doc> random_docs(std::mt19937_64& rng, std::size_t n, bool infinities) {
	std::vector<std::uint64_t> ids(n);
	for (std::size_t i = 0; i < n; ++i) { ids[i] = 3 * i + 1; }
	std::shuffle(ids.begin(), ids.end(), rng);
	std::vector<doc> docs;
	for (std::size_t i = 0; i < n; ++i) { docs.push_back(random_doc(rng, ids[i], infinities)); }
	return docs;
}

bool round_trips(const sk::key& k) {
	const std::string wire = sk::encode(k);
	return sk::decode(wire) == k && sk::encode(sk::decode(wire)) == wire;
}

bool rejects(std::string_view wire) {
	try {
		sk::decode(wire);
	} catch (const sk::malformed_key&) {
		return true;
	}
	return false;
}

std::string be(std::uint64_t k) {
	unsigned char buf[8];
	strata::ordered::put_be(k, buf);
	return std::string(reinterpret_cast<const char*>(buf), 8);
}

void check_order() {
	using sk::direction;
	auto u = [](direction dir, std::optional<std::uint64_t> v) {
		sk::part p{dir, sk::type::u64, {}};
		if (v) { p.v.emplace<std::uint64_t>(*v); }
		return p;
	};
	// Missing sorts last in either direction.
	CHECK(sk::compare(u(direction::ascending, 5), u(direction::ascending, std::nullopt)) < 0);
	CHECK(sk::compare(u(direction::descending, 5), u(direction::descending, std::nullopt)) < 0);
	CHECK(sk::compare(u(direction::ascending, std::nullopt), u(direction::ascending, std::nullopt)) == 0);
	CHECK(sk::compare(u(direction::ascending, 3), u(direction::ascending, 5)) < 0);
	CHECK(sk::compare(u(direction::descending, 3), u(direction::descending, 5)) > 0);
	// Ties break by docid ascending; later components decide before docid does.
	const sk::hit a{{u(direction::ascending, 1), u(direction::descending, 2)}, 9};
	const sk::hit b{{u(direction::ascending, 1), u(direction::descending, 2)}, 4};
	const sk::hit c{{u(direction::ascending, 1), u(direction::descending, 7)}, 99};
	CHECK(sk::before(b, a) && !sk::before(a, b));
	CHECK(sk::before(c, b) && !sk::before(b, c));
	// Bytes compare as unsigned bytes.
	const auto v = std::vector<std::string_view>{std::string_view("\xff", 1), "a"};
	const auto smallest = sk::select_bytes(sk::component::by_value(1, sk::type::bytes, direction::ascending), v);
	CHECK(smallest.v == sk::value(std::string("a")));
	// Distances select closest or farthest.
	const std::vector<double> points{-4.0, 1.0, 9.0};
	CHECK(sk::select_distance(sk::component::by_distance(2, 2.0), points).v == sk::value(1.0));
	CHECK(sk::select_distance(sk::component::by_distance(2, 2.0, sk::selection::farthest), points).v == sk::value(7.0));
	CHECK(sk::select_distance(sk::component::by_distance(2, 0.0), std::vector<double>{}).missing());
}

void check_top_k(std::mt19937_64& rng) {
	for (int round = 0; round < 60; ++round) {
		const auto docs = random_docs(rng, 400 + rng() % 400, false);
		const auto spec = random_spec(rng, false);
		std::vector<const doc*> sorted;
		for (const auto& d : docs) { sorted.push_back(&d); }
		std::sort(sorted.begin(), sorted.end(), [&](const doc* a, const doc* b) { return oracle_before(spec, *a, *b); });
		for (const std::size_t k : {std::size_t(1), std::size_t(10), std::size_t(100)}) {
			sk::top_k top(k);
			for (const auto& d : docs) { top.push({key_of(spec, d), d.docid}); }
			const auto got = std::move(top).take();
			CHECK(got.size() == std::min(k, docs.size()));
			for (std::size_t i = 0; i < got.size() && i < sorted.size(); ++i) {
				CHECK(got[i].docid == sorted[i]->docid);
				CHECK(got[i].k == key_of(spec, *sorted[i]));
			}
			// Any split into shards merges to the same list.
			const std::size_t shards = 1 + rng() % 6;
			std::vector<sk::top_k> parts;
			for (std::size_t s = 0; s < shards; ++s) { parts.emplace_back(k); }
			for (const auto& d : docs) { parts[rng() % shards].push({key_of(spec, d), d.docid}); }
			std::vector<std::vector<sk::hit>> lists;
			for (auto& p : parts) { lists.push_back(std::move(p).take()); }
			CHECK(sk::merge(lists, k) == got);
			// Wire round trips of every kept key.
			for (const auto& h : got) { CHECK(round_trips(h.k)); }
		}
	}
}

void check_wire(std::mt19937_64& rng) {
	using sk::direction;
	using sk::type;
	const double inf = std::numeric_limits<double>::infinity();
	std::string every_byte;
	for (int i = 0; i < 300; ++i) { every_byte.push_back(static_cast<char>(i & 0xff)); }
	sk::key extremes;
	for (const auto dir : {direction::ascending, direction::descending}) {
		for (const auto v : {std::uint64_t(0), std::uint64_t(1), std::numeric_limits<std::uint64_t>::max()}) { extremes.push_back({dir, type::u64, v}); }
		for (const double d : {0.0, std::numeric_limits<double>::denorm_min(), 1.5, std::numeric_limits<double>::max(), inf}) { extremes.push_back({dir, type::distance, d}); }
		for (const auto& s : {std::string(), std::string("\0", 1), every_byte}) { extremes.push_back({dir, type::bytes, s}); }
		for (const auto t : {type::u64, type::distance, type::bytes}) { extremes.push_back({dir, t, {}}); }
	}
	CHECK(round_trips(extremes));
	CHECK(round_trips(sk::key{}));
	// -0.0 encodes as +0.0.
	CHECK(sk::decode(sk::encode(sk::key{{direction::ascending, type::distance, -0.0}})).front().v == sk::value(0.0));

	// Every strict prefix is truncated; a trailing byte is rejected.
	const std::string wire = sk::encode(extremes);
	for (std::size_t n = 0; n < wire.size(); ++n) { CHECK(rejects(std::string_view(wire).substr(0, n))); }
	CHECK(rejects(wire + '\0'));

	// Malformed by construction.
	const std::string one_u64 = std::string("\x01\x01\x00", 3) + be(7);
	CHECK(sk::decode(one_u64).front().v == sk::value(std::uint64_t(7)));
	CHECK(rejects(std::string("\x02\x01\x00", 3) + be(7)));                            // version
	CHECK(rejects(std::string("\x01\x01\x03", 3) + be(7)));                            // type 3
	CHECK(rejects(std::string("\x01\x01\x18", 3)));                                    // reserved tag bit
	CHECK(rejects(std::string("\x01\x80\x00", 3)));                                    // overlong count
	CHECK(rejects(std::string("\x01\x05\x08", 3)));                                    // count past the input
	CHECK(rejects(std::string("\x01\x01\x02\x80\x00", 5)));                            // overlong length
	CHECK(rejects(std::string("\x01\x01\x02\x05", 4) + "ab"));                          // length past the input
	CHECK(rejects(std::string("\x01\x01\x01", 3) + be(strata::ordered::from_f64(-1.0))));  // negative distance
	CHECK(rejects(std::string("\x01\x01\x01", 3) + be(0x7fffffffffffffffull)));          // -0.0
	CHECK(rejects(std::string("\x01\x01\x01", 3) + be(0xfff8000000000000ull)));          // NaN
	CHECK(rejects(std::string("\x01\x01\x02", 3) + std::string(9, '\xff') + '\x02'));  // varint past 64 bits

	// encode refuses a part whose value does not match its type.
	bool refused = false;
	try { sk::encode(sk::key{{direction::ascending, type::u64, 1.0}}); } catch (const std::invalid_argument&) { refused = true; }
	CHECK(refused);

	// Random bytes and mutated encodings either decode to the key they are the unique
	// encoding of, or reject.
	int decoded = 0;
	auto probe = [&](const std::string& bytes) {
		try {
			const auto k = sk::decode(bytes);
			CHECK(sk::encode(k) == bytes);
			++decoded;
		} catch (const sk::malformed_key&) {
		} catch (...) {
			CHECK(!"decode threw something other than malformed_key");
		}
	};
	for (int i = 0; i < 50000; ++i) {
		std::string bytes(rng() % 24, '\0');
		for (auto& b : bytes) { b = static_cast<char>(rng() % 8 < 3 ? rng() % 16 : rng()); }  // small values reach valid tags
		if (bytes.size() > 1 && rng() % 4) { bytes[0] = 1; bytes[1] = static_cast<char>(rng() % 4); }
		probe(bytes);
	}
	for (int i = 0; i < 5000; ++i) {
		std::string bytes = wire;
		bytes[rng() % bytes.size()] = static_cast<char>(rng());
		probe(bytes);
	}
	CHECK(decoded > 100);
}

void check_block_skip(std::mt19937_64& rng) {
	int skipped = 0, equal_skipped = 0;
	for (int round = 0; round < 3000; ++round) {
		const bool infinities = round % 3 == 0;
		sk::spec spec = random_spec(rng, infinities);
		if (round % 2) { spec.resize(1); }  // one-component keys reach the equal-key rule
		const auto docs = random_docs(rng, 40 + rng() % 200, infinities);
		const std::size_t k = 1 + rng() % 20;
		sk::top_k top(k);
		std::uint64_t max_docid = 0;
		for (const auto& d : docs) {
			top.push({key_of(spec, d), d.docid});
			max_docid = std::max(max_docid, d.docid);
		}
		const sk::hit* worst = top.worst();
		if (!worst) { continue; }
		const bool docid_ordered = rng() % 2;
		std::vector<doc> block;
		for (std::size_t i = 0, n = 1 + rng() % 64; i < n; ++i) {
			// Ordered blocks follow every docid seen; others may interleave.
			const std::uint64_t id = docid_ordered ? max_docid + 1 + i : 3 * (rng() % 1000);
			block.push_back(random_doc(rng, id, infinities));
		}
		// The first component's slot bounds over present values.
		const auto slot = spec.front().slot;
		sk::block_bounds bounds;
		bool any = false;
		for (const auto& d : block) {
			if (slot == 0) {
				for (const auto v : d.nums) {
					if (!any || v < std::get<std::uint64_t>(bounds.min)) { bounds.min = v; }
					if (!any || v > std::get<std::uint64_t>(bounds.max)) { bounds.max = v; }
					any = true;
				}
			} else if (slot == 1) {
				for (const auto& v : d.words) {
					if (!any || v < std::get<std::string>(bounds.min)) { bounds.min = v; }
					if (!any || v > std::get<std::string>(bounds.max)) { bounds.max = v; }
					any = true;
				}
			} else {
				for (const auto v : d.points) {
					if (!any || v < std::get<double>(bounds.min)) { bounds.min = v; }
					if (!any || v > std::get<double>(bounds.max)) { bounds.max = v; }
					any = true;
				}
			}
		}
		if (!any) {
			CHECK(sk::may_compete(spec, bounds, worst, docid_ordered));  // no bound, no skip
			continue;
		}
		if (sk::may_compete(spec, bounds, worst, docid_ordered)) { continue; }
		++skipped;
		for (const auto& d : block) {
			const sk::hit h{key_of(spec, d), d.docid};
			CHECK(!sk::before(h, *worst));
			if (sk::compare(h.k, worst->k) == 0) { ++equal_skipped; }
		}
	}
	CHECK(skipped > 100);
	CHECK(equal_skipped > 0);
}

}  // namespace

int main() {
	std::mt19937_64 rng(8);
	check_order();
	check_top_k(rng);
	check_wire(rng);
	check_block_skip(rng);
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
	std::puts("sortkey: ok");
	return 0;
}
