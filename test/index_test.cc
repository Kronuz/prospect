// The index on faultfs: keys and versions, replace and delete, atomic transactions,
// explicit docids, replayed positions, commits surviving crashes, fencing, ENOSPC
// recovery, retention, and fail-closed opens.
#include "strata/faultfs.hh"
#include "strata/index.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;
using kv = std::pair<std::uint64_t, std::uint64_t>;

static const std::array<char, 16> uuid = {'s', 't', 'r', 'a', 't', 'a', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i', 'd'};

static document doc(const std::string& key, const std::string& body, std::uint32_t pos = 1) {
	document d;
	if (!key.empty()) { d.terms.push_back({key, 0, {}}); }
	d.terms.push_back({body, 1, {pos}});
	d.length = 1;
	d.stored = body;
	return d;
}
static operation add(const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::add;
	o.doc = doc(key, body);
	return o;
}
static operation replace(const std::string& key, const std::string& body, std::optional<std::uint64_t> expected = std::nullopt) {
	operation o;
	o.type = operation::replace_key;
	o.key = key;
	o.doc = doc(key, body);
	o.expected_version = expected;
	return o;
}
static operation erase(const std::string& key) {
	operation o;
	o.type = operation::delete_key;
	o.key = key;
	return o;
}
static transaction tx(std::vector<operation> ops) {
	transaction t;
	t.ops = std::move(ops);
	return t;
}

// docid -> (version, stored) over every live document in every segment.
static std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> contents(const strata::index& ix) {
	std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> out;
	for (const auto& s : ix.segments()) {
		for (ordinal_t o = 0; o < s.seg->doc_count(); ++o) {
			if (!s.live(o)) { continue; }
			const auto docid = s.seg->docid(o);
			if (out.count(docid)) { std::fprintf(stderr, "two live copies of docid %llu\n", (unsigned long long)docid); }
			out[docid] = {s.seg->version(o), s.seg->stored(o)};
		}
	}
	return out;
}

static bool throws(const std::function<void()>& f) {
	try {
		f();
	} catch (...) {
		return true;
	}
	return false;
}

int main() {
	strata::index::options keyed;
	keyed.key_prefix = "Q";
	// Keys, versions, replace, delete, and reopening.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		auto v = ix.apply(tx({add("Qa", "alpha"), add("Qb", "beta"), add("", "anonymous")}), {});
		CHECK(v.result == verdict::accepted && v.docids == std::vector<std::uint64_t>({1, 2, 3}) && v.versions == std::vector<std::uint64_t>({1, 1, 1}));
		CHECK(ix.find_key("Qa") == kv(1, 1));  // from the version map
		ix.refresh();
		CHECK(ix.find_key("Qa") == kv(1, 1));  // from the segment
		v = ix.apply(tx({replace("Qa", "alpha2")}), {});
		CHECK(v.result == verdict::accepted && v.docids[0] == 1 && v.versions[0] == 2);
		v = ix.apply(tx({erase("Qb")}), {});
		CHECK(v.result == verdict::accepted && v.docids[0] == 2);
		CHECK(!ix.find_key("Qb"));
		ix.commit();
		auto c = contents(ix);
		CHECK(c.size() == 2 && c[1] == std::make_pair(std::uint64_t(2), std::string("alpha2")) && c[3].second == "anonymous");
		// Re-adding a deleted key takes a new docid and version 1.
		v = ix.apply(tx({add("Qb", "beta again")}), {});
		CHECK(v.docids[0] == 4 && v.versions[0] == 1);
		ix.commit();
		const auto before = contents(ix);
		ix.close();
		ix = strata::index::open(io, keyed);
		CHECK(contents(ix) == before);
		CHECK(ix.last_docid() == 4 && ix.find_key("Qa") == kv(1, 2));
	}
	// Version checks and atomicity: a rejected transaction leaves no trace.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		ix.apply(tx({add("Qa", "a")}), {});
		auto v = ix.apply(tx({replace("Qa", "a2", 0)}), {});  // 0: must be absent
		CHECK(v.result == verdict::version_conflict && v.current_version == 1);
		v = ix.apply(tx({replace("Qa", "a2", 7)}), {});
		CHECK(v.result == verdict::version_conflict && v.current_version == 1);
		v = ix.apply(tx({replace("Qa", "a2", 1)}), {});
		CHECK(v.result == verdict::accepted && v.versions[0] == 2);
		const auto last = ix.last_docid();
		v = ix.apply(tx({add("Qx", "x"), erase("Qa"), replace("Qa", "never", 5)}), {});
		CHECK(v.result == verdict::version_conflict && v.op == 2);
		CHECK(!ix.find_key("Qx") && ix.find_key("Qa") == kv(1, 2) && ix.last_docid() == last);
		CHECK(ix.buffered_count() == 2);  // the original and its replacement; nothing of the rejected one
		// Invalid documents and a repeated key reject.
		document bad = doc("Qy", "dup");
		bad.terms.push_back({"dup", 1, {2}});
		operation o;
		o.doc = bad;
		CHECK(ix.apply(tx({o}), {}).result == verdict::invalid);
		document two_keys = doc("Qy", "y");
		two_keys.terms.push_back({"Qz", 0, {}});
		o.doc = two_keys;
		CHECK(ix.apply(tx({o}), {}).result == verdict::invalid);
		CHECK(ix.apply(tx({add("Qa", "again")}), {}).result == verdict::version_conflict);
	}
	// Explicit docids, the key/docid agreement, and replayed positions.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		operation o;
		o.type = operation::replace_docid;
		o.docid = 100;
		o.doc = doc("Qd", "d");
		auto v = ix.apply(tx({o}), {1, 1});
		CHECK(v.result == verdict::accepted && v.docids[0] == 100 && ix.last_docid() == 100);
		CHECK(ix.apply(tx({add("Qe", "e")}), {2, 1}).docids[0] == 101);
		o.docid = 50;  // Qd lives at 100, not 50
		CHECK(ix.apply(tx({o}), {3, 1}).result == verdict::invalid);
		CHECK(ix.apply(tx({add("Qf", "f")}), {2, 1}).result == verdict::skipped);
		CHECK(ix.apply(tx({add("Qf", "f")}), {3, 1}).result == verdict::skipped);  // applied at (3, 1) even rejected
		CHECK(ix.apply(tx({add("Qf", "f")}), {3, 2}).result == verdict::accepted);
		ix.commit();
		CHECK(ix.manifest().applied == files::position({3, 2}));
		ix.close();
		ix = strata::index::open(io, keyed);
		CHECK(ix.manifest().applied == files::position({3, 2}) && ix.has_docid(100) && ix.has_docid(101));
	}
	// Commits survive crashes; uncommitted work does not, and nothing half-applied appears.
	for (std::uint64_t seed = 0; seed < 200; ++seed) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> committed;
		{
			auto ix = strata::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a"), add("Qb", "b")}), {});
			ix.commit();
			ix.apply(tx({replace("Qa", "a2")}), {});
			ix.refresh();
			ix.apply(tx({erase("Qb")}), {});
			ix.commit();
			committed = contents(ix);
			ix.apply(tx({add("Qc", "c"), replace("Qa", "a3")}), {});
			if (seed % 2) { ix.refresh(); }
		}
		fs.crash(seed);
		auto ix = strata::index::open(io, keyed);
		CHECK(contents(ix) == committed);
	}
	// A crash during commit opens either the previous commit or the new one.
	for (std::uint64_t seed = 0; seed < 300; ++seed) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> first, second;
		{
			auto ix = strata::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a"), add("Qb", "b")}), {});
			ix.commit();
			first = contents(ix);
			ix.apply(tx({replace("Qa", "a2"), erase("Qb"), add("Qc", "c")}), {});
			// Stop the commit at a seeded step with a failure after its effect, as power loss would.
			const faultfs::op steps[] = {faultfs::op::sync, faultfs::op::sync_directory, faultfs::op::replace, faultfs::op::create};
			fs.add_fault({steps[seed % 4], 1 + (seed / 4) % 3, EIO, true});
			try {
				ix.commit();
			} catch (const index_fenced&) {
			}
			second = contents(ix);  // what the commit meant to make durable
		}
		fs.crash(seed);
		auto ix = strata::index::open(io, keyed);
		const auto now = contents(ix);
		CHECK(now == first || now == second);
	}
	// A failed sync fences; reopening recovers the last commit.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		ix.apply(tx({add("Qa", "a")}), {});
		ix.commit();
		const auto good = contents(ix);
		ix.apply(tx({add("Qb", "b")}), {});
		fs.add_fault({faultfs::op::sync, 1, EIO, false});
		CHECK(throws([&] { ix.commit(); }));
		CHECK(throws([&] { ix.apply(tx({add("Qc", "c")}), {}); }));  // fenced
		fs.clear_faults();
		ix.close();
		ix = strata::index(strata::index::open(io, keyed));
		CHECK(contents(ix) == good);
	}
	// ENOSPC while flushing keeps the buffer; the same commit succeeds once space returns.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		operation big = add("Qa", "a");
		std::mt19937_64 r(9);
		big.doc.stored.resize(20000);
		for (auto& c : big.doc.stored) { c = char(r()); }  // incompressible
		CHECK(ix.apply(tx({big}), {}).result == verdict::accepted);
		fs.set_capacity(fs.used() + 100);
		bool space = false;
		try {
			ix.commit();
		} catch (const std::system_error& e) {
			space = e.code().value() == ENOSPC;
		}
		CHECK(space);
		fs.set_capacity(~std::uint64_t(0));
		ix.commit();
		CHECK(contents(ix).size() == 1);
	}
	// Retention keeps the current manifest and two predecessors; orphans leave at open.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		for (int i = 0; i < 6; ++i) {
			ix.apply(tx({add("Q" + std::to_string(i), "x")}), {});
			ix.commit();
		}
		std::size_t manifests = 0;
		for (const auto& n : fs.list()) { manifests += n.starts_with("manifest-"); }
		CHECK(manifests == 3);
		io.create_exclusive("seg-999.sx")->write_at(0, "orphan");
		io.create_exclusive("manifest-1.tmp");
		ix.close();
		ix = strata::index(strata::index::open(io, keyed));
		for (const auto& n : fs.list()) { CHECK(n != "seg-999.sx" && n != "manifest-1.tmp"); }
		CHECK(contents(ix).size() == 6);
		// recover --to an older retained manifest drops the newer ones first.
		const std::uint64_t current = ix.manifest().generation;
		ix.close();
		ix = strata::index(strata::index::open(io, keyed, current - 2));
		CHECK(contents(ix).size() == 4);
		for (const auto& n : fs.list()) { CHECK(n != "manifest-" + std::to_string(current)); }
	}
	// A missing or changed file fails the open and changes nothing.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		{
			auto ix = strata::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a")}), {});
			ix.commit();
		}
		const auto names = fs.list();
		std::string seg;
		for (const auto& n : names) {
			if (n.starts_with("seg-")) { seg = n; }
		}
		const std::string kept(fs.view(seg));
		io.remove(seg);
		CHECK(throws([&] { strata::index::open(io, keyed); }));
		CHECK(fs.list().size() == names.size() - 1);
		auto f = io.create_exclusive(seg);
		std::string changed = kept;
		changed[changed.size() - 30] ^= 1;  // in the footer the manifest's CRC covers
		f->write_at(0, changed);
		CHECK(throws([&] { strata::index::open(io, keyed); }));
	}
	// The memory budget: permits draw and return bytes; two indexes sharing a small budget
	// stay within it plus one transaction, flush into several segments, and reach the same
	// digest as unbudgeted runs.
	{
		memory_budget b(1000);
		auto p = b.open();
		CHECK(p.try_grow(600) && b.used() == 600);
		CHECK(!p.try_grow(500) && b.used() == 600);  // refused, nothing drawn
		p.grow(500);                                  // forced: allocated memory is real
		CHECK(b.used() == 1100 && b.pressure());
		p.shrink(700);
		CHECK(b.used() == 400 && !b.pressure());
		{
			auto q = std::move(p);
			CHECK(b.used() == 400 && q.bytes() == 400 && p.bytes() == 0);
		}
		CHECK(b.used() == 0);  // the moved-to permit settled when it ended
	}
	{
		memory_budget budget(200 * 1024);
		index::options opts = keyed;
		opts.budget = &budget;
		faultfs::filesystem fa, fb, fc, fd;
		faultfs::io ia(fa), ib(fb), ic(fc), id(fd);
		auto a = strata::index::create(ia, opts, uuid), b = strata::index::create(ib, opts, uuid);
		auto ra = strata::index::create(ic, keyed, uuid), rb = strata::index::create(id, keyed, uuid);  // unbudgeted
		std::mt19937_64 r(4);
		std::uint64_t worst = 0, largest = 0;
		for (int i = 0; i < 4000; ++i) {
			const std::string key = "Q" + std::to_string(r() % 3000);
			const auto t = tx({replace(key, "body" + std::to_string(r() % 50))});
			auto& ix = i % 2 ? a : b;
			auto& ref = i % 2 ? ra : rb;
			const auto before = budget.used();
			ix.apply(t, {});
			ref.apply(t, {});
			largest = std::max(largest, budget.used() > before ? budget.used() - before : 0);
			worst = std::max(worst, budget.used());
		}
		CHECK(worst <= budget.capacity() + largest);
		CHECK(a.segments().size() > 2 && b.segments().size() > 2);
		CHECK(a.digest() == ra.digest() && b.digest() == rb.digest());
		a.commit();
		b.commit();
		CHECK(budget.used() == 0);  // committed buffers return their bytes
	}
	// The offline check: a healthy index passes and reports what it read; a byte changed
	// inside a section (which opening the index does not read) fails it.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::string seg_name;
		{
			auto ix = strata::index::create(io, keyed, uuid);
			for (int i = 0; i < 400; ++i) { ix.apply(tx({replace("Q" + std::to_string(i % 150), "w" + std::to_string(i % 17))}), {}); }
			ix.commit();
			const auto r = ix.check();
			CHECK(r.segments == 1 && r.documents == 150 && r.terms > 150 && r.postings >= 2 * 150 && r.stored_bytes > 0);
			for (const auto& n : fs.list()) {
				if (n.starts_with("seg-") && n.ends_with(".sx")) { seg_name = n; }
			}
		}
		const std::string bytes(fs.view(seg_name));
		auto f = io.open_existing(seg_name);
		std::string changed = bytes;
		// The middle of the largest section the reader verifies on first read.
		std::uint64_t at = 0, longest = 0;
		for (const auto& e : strata::format::decode_toc(bytes, strata::format::decode_footer(bytes))) {
			const bool lazy = e.kind == strata::format::section::postings || e.kind == strata::format::section::positions || e.kind == strata::format::section::terms;
			if (lazy && e.length > longest) {
				longest = e.length;
				at = e.offset + e.length / 2;
			}
		}
		CHECK(longest > 0);
		changed[at] = char(changed[at] ^ 0x10);
		f->write_at(0, changed);
		f->sync();
		auto ix = strata::index::open(io, keyed);
		CHECK(throws([&] { ix.check(); }));
	}
	std::printf("index: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
