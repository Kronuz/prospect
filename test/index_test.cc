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
	// Merges under faults: ENOSPC leaves the index as it was and the merge succeeds once
	// space returns; any other write failure fences, and reopening recovers the last commit.
	for (int fault = 0; fault < 2; ++fault) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, keyed, uuid);
		std::mt19937_64 r(10);
		for (int i = 0; i < 4; ++i) {
			operation o = add("Q" + std::to_string(i), "body");
			o.doc.stored.resize(5000);
			for (auto& c : o.doc.stored) { c = char(r()); }
			CHECK(ix.apply(tx({o}), {}).result == verdict::accepted);
			ix.refresh();
		}
		ix.apply(tx({erase("Q1")}), {});
		ix.commit();
		const auto good = contents(ix);
		const auto digest = ix.digest();
		std::vector<std::uint64_t> ids;
		for (const auto& v : ix.segments()) { ids.push_back(v.id); }
		CHECK(ids.size() == 4);
		if (fault == 0) {
			fs.set_capacity(fs.used() + 100);
			bool space = false;
			try {
				ix.merge(ids);
			} catch (const std::system_error& e) {
				space = e.code().value() == ENOSPC;
			}
			CHECK(space);
			CHECK(ix.segments().size() == ids.size() && ix.digest() == digest && contents(ix) == good);
			fs.set_capacity(~std::uint64_t(0));
			ix.merge(ids);
			CHECK(ix.segments().size() == 1 && ix.digest() == digest);
			ix.commit();
			ix.close();
			ix = strata::index(strata::index::open(io, keyed));
			CHECK(contents(ix) == good && ix.segments().size() == 1);
		} else {
			fs.add_fault({faultfs::op::create, 1, EIO, false});
			CHECK(throws([&] { ix.merge(ids); }));
			CHECK(throws([&] { ix.commit(); }));  // fenced
			fs.clear_faults();
			ix.close();
			ix = strata::index(strata::index::open(io, keyed));
			CHECK(contents(ix) == good && ix.digest() == digest);
		}
	}
	// Storage admission (T5). The index holds its files' bytes and the standing allowance
	// for its next commit's files: after a commit, the budget's use is the bytes on disk
	// plus a small allowance (a manifest and nothing pending), across reopens too.
	{
		auto on_disk = [](const faultfs::filesystem& fs) {
			std::uint64_t n = 0;
			for (const auto& name : fs.list()) {
				if (strata::index_detail::parse_name(name)) { n += fs.view(name).size(); }
			}
			return n;
		};
		faultfs::filesystem fs;
		faultfs::io io(fs);
		byte_budget storage(~std::uint64_t(0) >> 1);
		strata::index::options o = keyed;
		o.storage = &storage;
		o.auto_merge = true;
		{
			auto ix = strata::index::create(io, o, uuid);
			for (int i = 0; i < 300; ++i) {
				ix.apply(tx({add("Q" + std::to_string(i % 120), "body " + std::to_string(i))}), {});
				if (i % 40 == 39) { ix.commit(); }
			}
			ix.commit();
			CHECK(storage.used() > on_disk(fs) && storage.used() - on_disk(fs) < 4096);
		}
		CHECK(storage.used() == 0);
		auto ix = strata::index::open(io, o);
		CHECK(storage.used() > on_disk(fs) && storage.used() - on_disk(fs) < 4096);
	}
	// Admission rejects with no_space and changes nothing; what it admitted commits without
	// ENOSPC on a disk of the budget's size; merges wait for room; once another index on
	// the same storage is dropped, work continues and the content holds.
	{
		faultfs::filesystem fs, other_fs;
		faultfs::io io(fs), other_io(other_fs);
		const std::uint64_t capacity = 256 * 1024;
		byte_budget storage(capacity);
		strata::index::options o = keyed;
		o.storage = &storage;
		std::mt19937_64 r(21);
		auto doc_of = [&](const std::string& key) {
			operation op = add(key, "body");
			op.doc.stored.resize(3000);
			for (auto& c : op.doc.stored) { c = char(r()); }
			return op;
		};
		auto other = strata::index::create(other_io, o, uuid);
		for (int i = 0; i < 30; ++i) { CHECK(other.apply(tx({doc_of("Qo" + std::to_string(i))}), {}).result == verdict::accepted); }
		other.commit();
		fs.set_capacity(capacity - storage.used());  // the disk this index has left
		auto ix = strata::index::create(io, o, uuid);
		int accepted = 0;
		for (int i = 0; i < 200; ++i) {
			const auto v = ix.apply(tx({doc_of("Q" + std::to_string(i))}), {});
			if (v.result == verdict::no_space) { break; }
			CHECK(v.result == verdict::accepted);
			++accepted;
			if (i % 10 == 9) { ix.commit(); }
		}
		CHECK(accepted > 10 && accepted < 200);
		ix.commit();  // no ENOSPC: admission reserved it
		const auto before = contents(ix);
		const auto digest = ix.digest();
		CHECK(int(before.size()) == accepted);
		CHECK(ix.apply(tx({doc_of("Qx")}), {}).result == verdict::no_space);
		CHECK(contents(ix) == before && ix.digest() == digest);
		std::vector<std::uint64_t> ids;
		for (const auto& v : ix.segments()) { ids.push_back(v.id); }
		bool space = false;
		try {
			ix.merge(ids);
		} catch (const std::system_error& e) {
			space = e.code().value() == ENOSPC;
		}
		CHECK(space && ix.segments().size() == ids.size() && ix.digest() == digest);
		ix.maybe_merge();  // waits for room instead of failing
		CHECK(ix.digest() == digest);
		// The other index is dropped: its files leave and its bytes return.
		other.close();
		for (const auto& name : other_fs.list()) { other_io.remove(name); }
		fs.set_capacity(capacity);
		CHECK(ix.apply(tx({doc_of("Qx")}), {}).result == verdict::accepted);
		ix.merge({ids[0], ids[1]});  // a merge needs room for its output beside its sources
		ix.commit();
		CHECK(contents(ix).size() == before.size() + 1 && ix.segments().size() == ids.size());
		ix.close();
		ix = strata::index::open(io, o);
		CHECK(contents(ix).size() == before.size() + 1);
	}
	// Admission covers what it admits (review findings 9-11): on a disk exactly the budget's
	// size, one-document commits run until admission says no_space and the disk never does;
	// a merge of segments with distant docids is refused by admission or fits.
	{
		const std::uint64_t capacity = 64 * 1024;
		faultfs::filesystem fs(capacity);
		faultfs::io io(fs);
		byte_budget storage(capacity);
		strata::index::options o = keyed;
		o.storage = &storage;
		auto ix = strata::index::create(io, o, uuid);
		int accepted = 0;
		bool disk_full = false;
		for (int i = 0; i < 1000; ++i) {
			const auto v = ix.apply(tx({add("Q" + std::to_string(i), "b" + std::to_string(i))}), {});
			if (v.result == verdict::no_space) { break; }
			++accepted;
			try {
				ix.commit();
			} catch (const std::system_error&) {
				disk_full = true;
				break;
			}
		}
		CHECK(!disk_full && accepted > 5 && accepted < 1000);
		std::printf("  admission: %d one-document commits on a 64 KiB disk\n", accepted);
	}
	std::size_t merged = 0, refused = 0;
	for (std::uint64_t room : {0u, 4000u, 8000u, 16000u, 32000u, 64000u, 128000u}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		byte_budget storage(1ull << 40);
		strata::index::options o = keyed;
		o.storage = &storage;
		auto ix = strata::index::create(io, o, uuid);
		for (std::uint64_t base : {1ull, 1ull << 40}) {  // two segments, docids far apart
			for (std::uint64_t d = 0; d < 2000; ++d) {
				operation op;
				op.type = operation::replace_docid;
				op.docid = base + d;
				op.doc = doc("", "w" + std::to_string(d % 7));
				ix.apply(tx({op}), {});
			}
			ix.refresh();
		}
		ix.commit();
		std::vector<std::uint64_t> ids;
		for (const auto& v : ix.segments()) { ids.push_back(v.id); }
		byte_budget tight(storage.used() + room);  // the same index, reopened on a budget this tight
		ix.close();
		o.storage = &tight;
		ix = strata::index::open(io, o);
		fs.set_capacity(fs.used() + (tight.capacity() - tight.used()));  // the disk has what the budget has
		try {
			ix.merge(ids);
			CHECK(ix.segments().size() == 1);
			++merged;
		} catch (const std::system_error& e) {
			CHECK(std::string(e.what()).find("no storage reserved") != std::string::npos);  // admission, not the disk
			++refused;
		}
	}
	CHECK(merged > 0 && refused > 0);
	// ENOSPC strikes mid-file under short writes: the partial file is removed, and the same
	// commit succeeds once space returns.
	std::size_t mid_file = 0;
	for (std::uint64_t room = 0; room < 3000; room += 211) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		fs.short_writes(97);
		auto ix = strata::index::create(io, keyed, uuid);
		operation big = add("Qb", "b");
		big.doc.stored.assign(2500, 'x');
		ix.apply(tx({big, add("Qc", "c")}), {});
		fs.set_capacity(fs.used() + room);
		try {
			ix.commit();
		} catch (const std::system_error& e) {
			CHECK(e.code().value() == ENOSPC);
			++mid_file;
			// The partial file is gone: every segment file left is whole.
			for (const auto& name : fs.list()) {
				if (name.starts_with("seg-")) { CHECK(!throws([&] { (void)strata::format::decode_footer(fs.view(name)); })); }
			}
			fs.set_capacity(~std::uint64_t(0));
			ix.commit();
		}
		ix.close();
		ix = strata::index::open(io, keyed);
		CHECK(contents(ix).size() == 2);
	}
	CHECK(mid_file > 3);
	// ENOSPC at every point of a commit's files (segment, live docs, metadata, manifest):
	// the commit fails without fencing, and succeeds as a reference commit does once space
	// returns.
	{
		auto run = [&](faultfs::filesystem& fs, faultfs::io& io, std::optional<std::uint64_t> room) {
			auto ix = strata::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a"), add("Qb", "b"), add("Qc", "c")}), {});
			ix.commit();
			operation big = add("Qd", "d");
			big.doc.stored.assign(1500, 'd');
			transaction t = tx({erase("Qa"), replace("Qb", "b2"), big});
			operation meta;
			meta.type = operation::set_meta;
			meta.meta_key = "m";
			meta.meta_value = std::string(1500, 'm');
			t.ops.push_back(meta);
			CHECK(ix.apply(t, {}).result == verdict::accepted);
			bool failed = false;
			if (room) { fs.set_capacity(fs.used() + *room); }
			try {
				ix.commit();
			} catch (const std::system_error& e) {
				CHECK(e.code().value() == ENOSPC);
				failed = true;
				fs.set_capacity(~std::uint64_t(0));
				ix.commit();
			}
			return std::pair{failed, std::move(ix)};
		};
		faultfs::filesystem ref_fs;
		faultfs::io ref_io(ref_fs);
		auto [_, ref] = run(ref_fs, ref_io, std::nullopt);
		const auto want = contents(ref);
		const auto digest = ref.digest();
		std::size_t failed = 0;
		for (std::uint64_t room = 0; room < 6000; room += 97) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto [f, ix] = run(fs, io, room);
			failed += f;
			CHECK(contents(ix) == want && ix.digest() == digest && ix.metadata().at("m").size() == 1500);
			ix.close();
			ix = strata::index::open(io, keyed);
			CHECK(contents(ix) == want && ix.digest() == digest && ix.metadata().at("m").size() == 1500);
		}
		CHECK(failed > 20);
	}
	// install holds a checkpoint to what an open holds its head to: a checkpoint whose
	// files disagree with their entries, or that cannot be read, is refused before the
	// target changes, and the target stays unfenced.
	{
		auto build_source = [&](faultfs::filesystem& fs, faultfs::io& io) {
			auto ix = strata::index::create(io, keyed, uuid);
			for (int i = 0; i < 10; ++i) { ix.apply(tx({add("Q" + std::to_string(i), "s" + std::to_string(i))}), {}); }
			ix.refresh();
			for (int i = 10; i < 20; ++i) { ix.apply(tx({add("Q" + std::to_string(i), "s" + std::to_string(i))}), {}); }
			ix.apply(tx({erase("Q3"), erase("Q12")}), {});
			operation meta;
			meta.type = operation::set_meta;
			meta.meta_key = "m";
			meta.meta_value = "v";
			ix.apply(tx({meta}), {});
			const std::uint64_t cp = ix.checkpoint();
			(void)fs;
			return std::pair{std::move(ix), cp};
		};
		auto rewrite = [](faultfs::io& io, const std::string& name, const std::string& bytes) {
			io.remove(name);
			auto f = io.create_exclusive(name);
			f->write_at(0, bytes);
			f->sync();
		};
		auto manifest_of = [](faultfs::filesystem& fs, std::uint64_t g) { return strata::files::decode_manifest(fs.view(strata::index_detail::manifest_name(g))); };
		// One damage per case, applied to the source; the target must refuse it unchanged.
		const std::function<void(faultfs::filesystem&, faultfs::io&, std::uint64_t)> damages[] = {
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // live docs past their segment's document count
				auto m = manifest_of(fs, g);
				for (auto& r : m.segments) {
					if (!r.live.generation) { continue; }
					const auto name = strata::index_detail::live_name(r.id, r.live.generation);
					auto l = strata::files::decode_live_docs(fs.view(name));
					l.doc_count = 4096;
					l.live_count = 4096 - l.deleted.size() - 1;
					l.deleted.push_back(4000);
					const std::string lb = strata::files::encode(l);
					rewrite(io, name, lb);
					r.live.size = lb.size();
					r.live.crc = strata::files::trailing_crc(lb);
				}
				rewrite(io, strata::index_detail::manifest_name(g), strata::files::encode(m));
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // a live count unlike its entry
				auto m = manifest_of(fs, g);
				for (auto& r : m.segments) {
					if (r.live.generation) { --r.live_count; }
				}
				rewrite(io, strata::index_detail::manifest_name(g), strata::files::encode(m));
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // a segment's document count unlike its entry
				auto m = manifest_of(fs, g);
				++m.segments[0].doc_count;
				rewrite(io, strata::index_detail::manifest_name(g), strata::files::encode(m));
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // a damaged postings region
				auto m = manifest_of(fs, g);
				const auto name = strata::index_detail::segment_name(m.segments[0].id);
				std::string bytes(fs.view(name));
				for (const auto& e : strata::format::decode_toc(bytes, strata::format::decode_footer(bytes))) {
					if (e.kind == strata::format::section::postings) { bytes[e.offset + e.length / 2] ^= 1; }
				}
				rewrite(io, name, bytes);
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // a file missing from the source
				io.remove(strata::index_detail::segment_name(manifest_of(fs, g).segments[0].id));
			},
		};
		for (const auto& damage : damages) {
			faultfs::filesystem sfs, tfs;
			faultfs::io sio(sfs), tio(tfs);
			auto [source, cp] = build_source(sfs, sio);
			auto target = strata::index::create(tio, keyed, uuid);
			target.apply(tx({add("Qt", "t")}), {});
			target.commit();
			const auto before = target.digest();
			const auto generation = target.manifest().generation;
			damage(sfs, sio, cp);
			bool refused = false;
			try {
				target.install(sio, cp);
			} catch (const strata::corrupt_index&) {
				refused = true;
			} catch (const strata::corrupt_segment&) {
				refused = true;
			}
			CHECK(refused && target.digest() == before && target.manifest().generation == generation);
			CHECK(target.apply(tx({add("Qu", "u")}), {}).result == verdict::accepted);  // not fenced
			target.commit();
			target.close();
			target = strata::index::open(tio, keyed);
			CHECK(contents(target).size() == 2);
		}
		// The same checkpoint, undamaged, installs and reopens.
		faultfs::filesystem sfs, tfs;
		faultfs::io sio(sfs), tio(tfs);
		auto [source, cp] = build_source(sfs, sio);
		auto target = strata::index::create(tio, keyed, uuid);
		target.install(sio, cp);
		CHECK(target.digest() == source.digest() && target.metadata().at("m") == "v");
		target.close();
		target = strata::index::open(tio, keyed);
		CHECK(target.digest() == source.digest());
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
		// recover --to an older retained manifest commits its content as a newer
		// generation, so the rollback holds when the index reopens.
		const std::uint64_t current = ix.manifest().generation;
		ix.close();
		ix = strata::index(strata::index::open(io, keyed, current - 2));
		CHECK(contents(ix).size() == 4 && ix.manifest().generation > current);
		ix.close();
		ix = strata::index(strata::index::open(io, keyed));
		CHECK(contents(ix).size() == 4);
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
		byte_budget b(1000);
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
		byte_budget budget(200 * 1024);
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
			CHECK(r.segments == 1 && r.documents == 150 && r.terms > 150 && r.postings >= 2 * 150 && r.stored_bytes > 0 && r.deleted == 0);
			// Deleted documents are counted, not skipped.
			for (int i = 0; i < 10; ++i) { ix.apply(tx({erase("Q" + std::to_string(i))}), {}); }
			ix.commit();
			const auto d = ix.check();
			CHECK(d.documents == 150 && d.deleted == 10 && d.postings == r.postings);
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
