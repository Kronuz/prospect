// Background merges (D11, D18), on the deterministic inline executor and on the threaded
// one (Kronuz/threadpool task groups, one and two threads): deletes and replaces racing a
// running merge carry over once, against an inline-merging twin's digest; seeded random
// schedules with a power loss while merges run; close, destruction, and install while
// merges run; the rate limit; merges that never wait for indexing-budget room; ENOSPC
// discarding a merge, EIO fencing, and a damaged source left for heal(); merge outputs
// published in id order; and the merge build's cancellation points.
#include "text_field.hh"
#include "prospect/index.hh"
#include "strata/core/faultfs.hh"
#include "strata/core/pool_executor.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace prospect;
using clock_type = std::chrono::steady_clock;

static const std::array<char, 16> uuid = {'s', 't', 'r', 'a', 't', 'a', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i', 'd'};

static document doc(const std::string& key, const std::string& body) {
	document d;
	d.key = key;
	d.field().terms.push_back({body, 1, {1}});
	d.length = 1;
	d.stored = body;
	return d;
}
static operation replace(const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::replace_key;
	o.key = key;
	o.doc = doc(key, body);
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
	t.fields = text_fields();
	t.ops = std::move(ops);
	return t;
}
static bool throws(const std::function<void()>& f) {
	try {
		f();
	} catch (...) {
		return true;
	}
	return false;
}
static double seconds_since(clock_type::time_point t) { return std::chrono::duration<double>(clock_type::now() - t).count(); }

// docid -> (version, stored) over every live document.
static std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> contents(const prospect::index& ix) {
	std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> out;
	for (const auto& s : ix.segments()) {
		for (ordinal_t o = 0; o < s.seg()->core().doc_count(); ++o) {
			if (s.live(o)) { out[s.seg()->core().docid(o)] = {s.seg()->get<strata::keys_plugin>().version(o), std::string(s.seg()->get<strata::stored_plugin>().stored(o))}; }
		}
	}
	return out;
}

// The background executor of one run, inline (threads 0) or on a pool of `threads`, and
// the node's rate limiter on its clock.
struct background {
	std::unique_ptr<ThreadPool<>> pool;
	std::unique_ptr<executor> exec;
	inline_executor* inl = nullptr;
	rate_limiter rate;
	explicit background(std::size_t threads, std::uint64_t bytes_per_second = 0) : rate(bytes_per_second) {
		if (threads) {
			pool = std::make_unique<ThreadPool<>>("merge{}", threads);
			exec = std::make_unique<pool_executor<>>(*pool);
		} else {
			auto e = std::make_unique<inline_executor>();
			inl = e.get();
			exec = std::move(e);
		}
	}
	~background() { exec.reset(); }  // before the pool
	bool threaded() const { return inl == nullptr; }
	void run() {
		if (inl) { inl->run(); }
	}
	// Small tiers, so a handful of tiny segments asks for merges; `use`: merge here,
	// otherwise inline.
	prospect::index::options options(bool use = true) {
		prospect::index::options o;
		o.key_prefix = "Q";
		o.policy.segments_per_tier = 2;
		o.policy.floor_bytes = 1;
		if (use) {
			o.background = exec.get();
			o.rate = &rate;
		}
		return o;
	}
};
static const char* name_of(std::size_t threads) { return threads == 0 ? "inline" : threads == 1 ? "one-thread" : "two-thread"; }

// `segments` committed segments of `docs` documents each, every one with `bytes` random
// stored bytes (incompressible), keys from Q<first>.
static void fill(prospect::index& ix, int segments, int docs, std::size_t bytes, std::uint64_t seed, int first = 0) {
	std::mt19937_64 r(seed);
	int n = first;
	for (int s = 0; s < segments; ++s) {
		for (int d = 0; d < docs; ++d) {
			operation o = replace("Q" + std::to_string(n++), "body");
			o.doc.stored.resize(bytes);
			for (auto& c : o.doc.stored) { c = char(r()); }
			CHECK(ix.apply(tx({o}), {}).result == verdict::accepted);
		}
		ix.refresh();
	}
	ix.commit();
}

// Segment files on disk, and those the manifest names.
static std::set<std::string> segment_files(const faultfs::filesystem& fs) {
	std::set<std::string> out;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::segment) { out.insert(n); }
	}
	return out;
}
static std::set<std::string> named_segments(const prospect::index& ix) {
	std::set<std::string> named;
	for (const auto& s : ix.manifest().segments) { named.insert(index_detail::segment_name(s.id)); }
	return named;
}
static bool no_spills(const faultfs::filesystem& fs) {
	for (const auto& n : fs.list()) {
		if (n.ends_with(".spill")) { return false; }
	}
	return true;
}

// faultfs::io whose segment creations wait while it is held, counting the arrivals.
class gate_io final : public kronuz::journal::IO {
public:
	explicit gate_io(faultfs::io& inner) : inner_(&inner) {}
	void hold() {
		std::lock_guard l(mu_);
		held_ = true;
	}
	void release() {
		{
			std::lock_guard l(mu_);
			held_ = false;
		}
		cv_.notify_all();
	}
	void await_arrival() {
		std::unique_lock l(mu_);
		cv_.wait(l, [&] { return arrived_ > 0; });
	}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return inner_->acquire_owner(create); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_->acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return inner_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return inner_->directory_trust(); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override { return inner_->open_existing(name); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		if (name.starts_with("seg-") && name.ends_with(".sx")) {
			std::unique_lock l(mu_);
			if (held_) {
				++arrived_;
				cv_.notify_all();
				cv_.wait(l, [&] { return !held_; });
			}
		}
		return inner_->create_exclusive(name);
	}
	void replace(std::string_view a, std::string_view b) override { inner_->replace(a, b); }
	void remove(std::string_view name) override { inner_->remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_->scan_directory(); }
	void sync_directory() override { inner_->sync_directory(); }

private:
	faultfs::io* inner_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool held_ = false;
	int arrived_ = 0;
};

// Deletes and replaces while merges run carry over to the outputs exactly once: no
// document resurrected or lost, the digest the inline twin's. On the pool, a merge has
// read its sources and waits to create its output while they arrive.
static void carry_over(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs, tfs;
	faultfs::io inner(fs), tio(tfs);
	gate_io gate(inner);
	kronuz::journal::IO& io = bg.threaded() ? static_cast<kronuz::journal::IO&>(gate) : static_cast<kronuz::journal::IO&>(inner);
	auto ix = prospect::index::create(io, bg.options(), uuid);
	auto twin = prospect::index::create(tio, bg.options(false), uuid);
	auto both = [&](const transaction& t) {
		const auto a = ix.apply(t, {}), b = twin.apply(t, {});
		CHECK(a.result == b.result && a.docids == b.docids && a.versions == b.versions);
	};
	for (int s = 0; s < 6; ++s) {
		for (int i = 0; i < 10; ++i) { both(tx({replace("Q" + std::to_string(s * 10 + i), "v0")})); }
		ix.refresh();
		twin.refresh();
	}
	both(tx({erase("Q0"), erase("Q11")}));  // deleted before the merges read them
	std::set<std::uint64_t> before;
	for (const auto& v : ix.segments()) { before.insert(v.id); }
	if (bg.threaded()) { gate.hold(); }
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	if (bg.threaded()) { gate.await_arrival(); }
	for (int s = 0; s < 6; ++s) {
		both(tx({erase("Q" + std::to_string(s * 10 + 2)), replace("Q" + std::to_string(s * 10 + 3), "v1")}));
		both(tx({replace("Q" + std::to_string(s * 10 + 4), "v1"), replace("Q" + std::to_string(s * 10 + 4), "v2")}));
	}
	both(tx({erase("Q11"), replace("Q0", "back")}));  // already gone: a no-op, then a new docid
	CHECK(ix.digest() == twin.digest());
	if (bg.threaded()) { gate.release(); }
	ix.wait_merges();
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 6);
	CHECK(ix.digest() == twin.digest() && contents(ix) == contents(twin));
	bool carried = false;
	for (const auto& v : ix.segments()) { carried = carried || (!before.count(v.id) && v.deleted); }
	CHECK(carried);  // an output was born with the deletes made while it ran
	(void)ix.check();
	ix.commit();
	twin.commit();
	const auto digest = ix.digest();
	ix.close();
	ix = prospect::index::open(io, bg.options());
	CHECK(ix.digest() == digest && contents(ix) == contents(twin));
	(void)ix.check();
}

// A merge takes its output's id when scheduled; a flush that publishes while it runs takes
// a higher one. The output goes in id order, so the next commit's manifest lists the
// segments in increasing id order and reopens.
static void id_order(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = prospect::index::create(io, bg.options(), uuid);
	fill(ix, 6, 4, 300, 11);
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	CHECK(ix.apply(tx({replace("Q500", "flushed while merging")}), {}).result == verdict::accepted);
	ix.refresh();  // the flush's segment id is above the pending merges' output ids
	ix.wait_merges();
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 7);
	std::uint64_t last = 0;
	for (const auto& v : ix.segments()) {
		CHECK(v.id > last);
		last = v.id;
	}
	ix.commit();
	const auto digest = ix.digest();
	ix.close();
	ix = prospect::index::open(io, bg.options());
	CHECK(ix.digest() == digest && contents(ix).size() == 25);
	last = 0;
	for (const auto& s : ix.manifest().segments) {
		CHECK(s.id > last);
		last = s.id;
	}
}

namespace strata {
// Breaks an invariant no public call can: two segments out of id order.
struct index_test_access {
	static void swap_segments(prospect::index& ix) { std::swap(ix.segs_[0], ix.segs_[1]); }
};
}  // namespace strata

// Segments out of id order fence the commit before it writes anything: no new manifest,
// no new file, and the index stays fenced.
static void order_check_fences() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	background bg(0);
	auto ix = prospect::index::create(io, bg.options(false), uuid);
	fill(ix, 2, 2, 100, 12);
	const auto files = fs.list();
	strata::index_test_access::swap_segments(ix);
	bool fenced = false;
	try {
		ix.commit();
	} catch (const index_fenced&) {
		fenced = true;
	}
	CHECK(fenced && fs.list() == files);
	CHECK(throws([&] { ix.commit(); }));
	CHECK(fs.list() == files);
}

// One index merging in the background and its twin merging inline, fed the same
// transactions: their verdicts, digests, and contents must agree at every point.
struct twins {
	faultfs::filesystem fs, tfs;
	faultfs::io io{fs}, tio{tfs};
	prospect::index::options bo, to;
	prospect::index ix, twin;
	explicit twins(background& bg)
		: bo(with_auto(bg.options())), to(with_auto(bg.options(false))), ix(prospect::index::create(io, bo, uuid)), twin(prospect::index::create(tio, to, uuid)) {}
	static prospect::index::options with_auto(prospect::index::options o) {
		o.auto_merge = true;
		return o;
	}
	void apply(const transaction& t) {
		const auto a = ix.apply(t, {}), b = twin.apply(t, {});
		CHECK(a.result == b.result && a.docids == b.docids && a.versions == b.versions);
	}
	bool same() const { return ix.digest() == twin.digest() && contents(ix) == contents(twin); }
};

// Random schedules, seeded, with two indexes sharing the executor: transactions,
// refreshes and commits merging after each flush, forced merges, waits, and (inline) the
// executor running at seeded points. Then a power loss while merges run recovers the
// last commit.
static void random_schedules(std::size_t threads, std::uint64_t seed) {
	background bg(threads);
	twins a(bg), b(bg);
	twins* pair[] = {&a, &b};
	std::mt19937_64 r(seed * 7919 + threads);
	for (int step = 0; step < 300; ++step) {
		auto& p = *pair[r() % 2];
		const std::string key = "Q" + std::to_string(r() % 60), other = "Q" + std::to_string(r() % 60);
		const std::string body = "b" + std::to_string(r() % 7);
		switch (r() % 17) {
		case 0: case 1: case 2: case 3: case 4: case 5: case 6: p.apply(tx({replace(key, body)})); break;
		case 7: case 8: case 9: p.apply(tx({erase(key)})); break;
		case 10: p.apply(tx({replace(key, body), erase(other)})); break;
		case 11: case 12:
			p.ix.refresh();
			p.twin.refresh();
			break;
		case 13:
			p.ix.commit();
			p.twin.commit();
			break;
		case 14:
			if (r() % 2) {
				p.ix.force_merge_deletes();
				p.twin.force_merge_deletes();
			} else {
				p.ix.force_merge(2);
				p.twin.force_merge(2);
			}
			break;
		case 15: p.ix.wait_merges(); break;
		case 16: bg.run(); break;
		}
		if (step % 8 == 0) { CHECK(p.same()); }
	}
	for (auto* p : pair) {
		p->ix.wait_merges();
		CHECK(p->ix.merges_pending() == 0 && p->same());
		(void)p->ix.check();
		p->ix.commit();
		p->twin.commit();
		CHECK(p->same());
	}
	const auto durable = a.ix.digest();
	for (int i = 0; i < 40; ++i) {
		a.ix.apply(tx({replace("Q" + std::to_string(r() % 60), "late")}), {});
		if (i % 4 == 3) { a.ix.refresh(); }
		if (i % 8 == 5) { bg.run(); }
	}
	a.fs.crash(seed);
	a.ix.close();
	a.ix = prospect::index::open(a.io, a.bo);
	CHECK(a.ix.digest() == durable);
	(void)a.ix.check();
}

// Close and destruction while merges run or wait: queued merges are dropped and running
// ones stopped, promptly, and reopening finds the last commit and no merge's file.
static void close_while_merging(std::size_t threads) {
	background bg(threads, 8 << 10);  // each merge here takes seconds at this rate
	faultfs::filesystem fa, fb, fc;
	faultfs::io ia(fa), ib(fb), ic(fc);
	const auto o = bg.options();
	auto a = prospect::index::create(ia, o, uuid);
	auto b = prospect::index::create(ib, o, uuid);
	fill(a, 6, 4, 3000, 1);
	fill(b, 6, 4, 3000, 2);
	const auto da = a.digest(), db = b.digest();
	{
		auto c = prospect::index::create(ic, o, uuid);
		fill(c, 6, 4, 3000, 3);
		c.maybe_merge();
	}  // destroyed with its merges queued or running
	a.maybe_merge();
	b.maybe_merge();
	CHECK(a.merges_pending() >= 1 && b.merges_pending() >= 1);
	if (bg.threaded()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
	const auto t0 = clock_type::now();
	b.close();
	a.close();
	CHECK(seconds_since(t0) < 2.0);
	a = prospect::index::open(ia, o);
	b = prospect::index::open(ib, o);
	auto c = prospect::index::open(ic, o);
	a.wait_deletes();  // an open posts its orphans' deletes to the executor
	b.wait_deletes();
	c.wait_deletes();
	CHECK(a.digest() == da && b.digest() == db && contents(c).size() == 24);
	CHECK(segment_files(fa) == named_segments(a) && segment_files(fb) == named_segments(b) && segment_files(fc) == named_segments(c));
	CHECK(no_spills(fa) && no_spills(fb) && no_spills(fc));
	(void)a.check();
	(void)b.check();
	(void)c.check();
}

// The rate limit: a merge writing N bytes under L bytes per second takes at least N/L, on
// the wall clock with threads and on the inline executor's clock without.
static void rate_limit(std::size_t threads) {
	const std::uint64_t rate = 64 << 10;
	background bg(threads, rate);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = prospect::index::create(io, bg.options(), uuid);
	fill(ix, 4, 4, 3000, 4);
	const auto digest = ix.digest();
	const auto t0 = clock_type::now();
	const std::uint64_t v0 = bg.exec->now();
	ix.force_merge(1);
	const double took = bg.threaded() ? seconds_since(t0) : double(bg.exec->now() - v0) / 1e9;
	const double n = double(bg.rate.admitted());
	CHECK(ix.segments().size() == 1 && ix.digest() == digest);
	CHECK(n > 40000 && took >= (bg.threaded() ? 0.9 : 0.999) * n / double(rate));
	if (took < 0.9 * n / double(rate)) { std::fprintf(stderr, "  rate (%s): %.0f bytes in %.3f s\n", name_of(threads), n, took); }
}

// The executor of one run, counting the merges' sleeps.
class counting_executor final : public strata::executor {
public:
	explicit counting_executor(strata::executor& inner) : inner_(&inner) {}
	std::unique_ptr<group> open() override { return inner_->open(); }
	std::uint64_t now() override { return inner_->now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		sleeps_.fetch_add(1);
		return inner_->sleep_until(t, stop);
	}
	bool checkpoint(const task::cancel_token& stop) override { return inner_->checkpoint(stop); }
	std::uint64_t sleeps() const { return sleeps_.load(); }

private:
	strata::executor* inner_;
	std::atomic<std::uint64_t> sleeps_{0};
};

// Other holders of the indexing budget hold all of it: a merge still runs at once, drawing
// past the capacity, while flushes go on (they draw past it too). It waits for none of them
// (strata main 6e13416 removed merge_memory_wait).
static void memory_held(std::size_t threads) {
	background bg(threads);
	counting_executor counted(*bg.exec);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget memory(4 << 20);
	auto o = bg.options();
	o.background = &counted;
	o.budget = &memory;
	auto ix = prospect::index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 9);
	auto hog = memory.open();
	CHECK(hog.try_grow(memory.capacity() - memory.used()));
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	ix.apply(tx({replace("Q1000", "flushed while the merge runs")}), {});
	ix.refresh();
	ix.wait_merges();
	CHECK(counted.sleeps() == 0);
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 7 && contents(ix).count(25));
	CHECK(memory.peak() > memory.capacity());
	hog.settle();
	CHECK(contents(ix).size() == 25);
	CHECK(memory.used() < (1 << 20));  // the merges' charges settled at publication
}

// ENOSPC while a background merge writes discards it, its partial files removed and the
// index as it was; force_merge reports it; once space returns the next merge succeeds.
// EIO fences, and reopening recovers the last commit.
static void space_and_faults(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	const auto o = bg.options();
	auto ix = prospect::index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 5);
	const auto digest = ix.digest();
	const auto good = contents(ix);
	const auto files = segment_files(fs);
	fs.set_capacity(fs.used() + 100);
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	ix.wait_merges();
	CHECK(ix.merges_pending() == 0 && ix.segments().size() == 6 && ix.digest() == digest && contents(ix) == good);
	CHECK(segment_files(fs) == files && no_spills(fs));
	bool space = false;
	try {
		ix.force_merge(1);
	} catch (const std::system_error& e) {
		space = e.code().value() == ENOSPC;
	}
	CHECK(space && ix.segments().size() == 6 && ix.digest() == digest && segment_files(fs) == files);
	fs.set_capacity(~std::uint64_t(0));
	ix.maybe_merge();
	ix.wait_merges();
	CHECK(ix.segments().size() < 6 && ix.digest() == digest && contents(ix) == good);
	ix.commit();
	fill(ix, 3, 4, 3000, 6, 100);
	const auto committed = ix.digest();
	fs.add_fault({faultfs::op::create, 1, EIO, false});  // the merge's first create
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	CHECK(throws([&] { ix.wait_merges(); }));
	CHECK(throws([&] { ix.commit(); }));  // fenced
	fs.clear_faults();
	ix.close();
	ix = prospect::index::open(io, o);
	CHECK(ix.digest() == committed && contents(ix).size() == 36);
}

// A merge reading a damaged source marks it for heal() instead of failing the index; the
// policy leaves it out until heal() reads it back whole.
static void damaged_source(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	fs.map_files(false);  // media faults act on reads
	faultfs::io io(fs);
	const auto o = bg.options();
	auto ix = prospect::index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 10);
	const auto digest = ix.digest();
	const std::uint64_t victim = ix.manifest().segments.front().id;
	ix.close();
	const std::string name = index_detail::segment_name(victim);
	{
		const segment seg(fs.view(name));
		for (const auto& e : seg.core().sections()) {
			if (e.kind == strata::format::section::stored) { fs.add_media_fault(name, faultfs::media::bit_flip, e.offset + e.length / 2, e.offset + e.length / 2 + 1, 0); }
		}
	}
	ix = prospect::index::open(io, o);
	ix.force_merge(1);  // the damaged one is left out; the rest merge
	CHECK(ix.needs_heal() && ix.segments().size() == 2);
	CHECK(ix.heal() == std::vector<std::uint64_t>{victim});  // its file reads damaged too
	fs.clear_faults();
	CHECK(ix.heal().empty() && !ix.needs_heal());
	ix.force_merge(1);
	CHECK(ix.segments().size() == 1 && ix.digest() == digest);
}

// Install while merges run (D11's carry-over at install): the merges end unpublished,
// the index holds exactly the checkpoint, and no merge's file stays behind.
static void install_while_merging(std::size_t threads) {
	background bg(threads, 8 << 10);
	faultfs::filesystem fs, sfs;
	faultfs::io io(fs), sio(sfs);
	const auto o = bg.options();
	auto src = prospect::index::create(sio, bg.options(false), uuid);
	fill(src, 2, 5, 100, 7, 1000);
	const auto generation = src.checkpoint();
	const auto want = src.digest();
	const auto want_contents = contents(src);
	auto ix = prospect::index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 8);
	const auto files = segment_files(fs);
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	if (bg.threaded()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
	ix.install(sio, generation);
	CHECK(ix.merges_pending() == 0);
	ix.wait_merges();
	CHECK(ix.digest() == want && contents(ix) == want_contents && ix.segments().size() == 2);
	ix.commit();
	ix.reclaim();
	const auto named = named_segments(ix);
	for (const auto& n : segment_files(fs)) { CHECK(files.count(n) || named.count(n)); }
	CHECK(no_spills(fs));
	ix.close();
	ix = prospect::index::open(io, o);
	CHECK(ix.digest() == want && contents(ix) == want_contents);
}

// The merge build stops at its next document, term, or stored frame once cancelled.
static void build_cancellation() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	prospect::index::options o;
	o.key_prefix = "Q";
	auto ix = prospect::index::create(io, o, uuid);
	for (int s = 0; s < 2; ++s) {
		for (int d = 0; d < 200; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(s * 200 + d), "t" + std::to_string(d % 50))}), {}).result == verdict::accepted); }
		ix.refresh();
	}
	std::vector<merge_source> sources;
	for (const auto& v : ix.segments()) { sources.push_back({v.seg(), nullptr}); }
	const std::array<std::uint8_t, 16> header{};
	std::string full;
	{
		string_output out(full);
		string_spill spill;
		(void)merge_segments(sources, header, out, spill, {.key_prefix = "Q"});
	}
	{
		task::cancel_source source;
		const auto token = source.token();
		source.request();
		std::string bytes;
		string_output out(bytes);
		string_spill spill;
		bool cancelled = false;
		try {
			(void)merge_segments(sources, header, out, spill, {.cancel = &token, .key_prefix = "Q"});
		} catch (const task::cancelled&) {
			cancelled = true;
		}
		CHECK(cancelled && bytes.empty());
	}
	{
		// Cancelled at the output's first write (the header): the first term stops it.
		struct cancelling final : output {
			task::cancel_source& source;
			std::string bytes;
			explicit cancelling(task::cancel_source& s) : source(s) {}
			using output::write;
			void write(const char* data, std::size_t size) override {
				bytes.append(data, size);
				source.request();
			}
		};
		task::cancel_source source;
		const auto token = source.token();
		cancelling out(source);
		string_spill spill;
		bool cancelled = false;
		try {
			(void)merge_segments(sources, header, out, spill, {.cancel = &token, .key_prefix = "Q"});
		} catch (const task::cancelled&) {
			cancelled = true;
		}
		CHECK(cancelled && !out.bytes.empty() && out.bytes.size() < full.size() / 2);
	}
}

int main() {
	build_cancellation();
	order_check_fences();
	for (std::size_t threads : {0, 1, 2}) {
		const int before = failures;
		carry_over(threads);
		id_order(threads);
		for (std::uint64_t seed = 1; seed <= 6; ++seed) { random_schedules(threads, seed); }
		close_while_merging(threads);
		rate_limit(threads);
		memory_held(threads);
		space_and_faults(threads);
		damaged_source(threads);
		install_while_merging(threads);
		if (failures != before) { std::fprintf(stderr, "  (failures above: the %s executor)\n", name_of(threads)); }
	}
	std::printf("background_merge: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
