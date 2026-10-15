// Background merges (D11, D18): deletes and replaces racing a running merge carry over
// once, against an inline-merging twin's digest; seeded random schedules on one and two
// threads, with a power loss while merges run; close, destruction, and install while
// merges run; the rate limit; ENOSPC discarding a merge and EIO fencing.
#include "strata/faultfs.hh"
#include "strata/index.hh"
#include "strata/merge_scheduler.hh"

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

using namespace strata;
using clock_type = std::chrono::steady_clock;

static const std::array<char, 16> uuid = {'s', 't', 'r', 'a', 't', 'a', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i', 'd'};

static document doc(const std::string& key, const std::string& body) {
	document d;
	d.terms.push_back({key, 0, {}});
	d.terms.push_back({body, 1, {1}});
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
static std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> contents(const strata::index& ix) {
	std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> out;
	for (const auto& s : ix.segments()) {
		for (ordinal_t o = 0; o < s.seg->doc_count(); ++o) {
			if (s.live(o)) { out[s.seg->docid(o)] = {s.seg->version(o), std::string(s.seg->stored(o))}; }
		}
	}
	return out;
}

// Small tiers, so a handful of tiny segments asks for merges.
static strata::index::options small_tiers(merge_scheduler* s) {
	strata::index::options o;
	o.key_prefix = "Q";
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	o.scheduler = s;
	return o;
}

// `segments` committed segments of `docs` documents each, every one with `bytes` random
// stored bytes (incompressible), keys from Q<first>.
static void fill(strata::index& ix, int segments, int docs, std::size_t bytes, std::uint64_t seed, int first = 0) {
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

// Segment files on disk.
static std::set<std::string> segment_files(const faultfs::filesystem& fs) {
	std::set<std::string> out;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::segment) { out.insert(n); }
	}
	return out;
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

// One index merging in the background and its twin merging inline, fed the same
// transactions: their verdicts, digests, and contents must agree at every point.
struct twins {
	faultfs::filesystem fs, tfs;
	faultfs::io io{fs}, tio{tfs};
	strata::index::options bo, to;
	strata::index bg, twin;
	twins(merge_scheduler& s, bool auto_merge)
		: bo(make_options(&s, auto_merge)), to(make_options(nullptr, auto_merge)), bg(strata::index::create(io, bo, uuid)), twin(strata::index::create(tio, to, uuid)) {}
	static strata::index::options make_options(merge_scheduler* s, bool auto_merge) {
		auto o = small_tiers(s);
		o.auto_merge = auto_merge;
		return o;
	}
	void apply(const transaction& t) {
		const auto a = bg.apply(t, {}), b = twin.apply(t, {});
		CHECK(a.result == b.result && a.docids == b.docids && a.versions == b.versions);
	}
	bool same() const { return bg.digest() == twin.digest() && contents(bg) == contents(twin); }
};

int main() {
	// Deletes and replaces while merges run carry over to the outputs exactly once: no
	// document resurrected or lost, the digest the inline twin's.
	for (std::size_t threads : {1, 2}) {
		merge_scheduler sched({threads, 0});
		faultfs::filesystem fs, tfs;
		faultfs::io inner(fs), tio(tfs);
		gate_io io(inner);
		auto bg = strata::index::create(io, small_tiers(&sched), uuid);
		auto twin = strata::index::create(tio, small_tiers(nullptr), uuid);
		auto both = [&](const transaction& t) {
			const auto a = bg.apply(t, {}), b = twin.apply(t, {});
			CHECK(a.result == b.result && a.docids == b.docids && a.versions == b.versions);
		};
		std::set<std::uint64_t> before;
		for (int s = 0; s < 6; ++s) {
			for (int i = 0; i < 10; ++i) { both(tx({replace("Q" + std::to_string(s * 10 + i), "v0")})); }
			bg.refresh();
			twin.refresh();
		}
		both(tx({erase("Q0"), erase("Q11")}));  // deleted before the merges read them
		for (const auto& v : bg.segments()) { before.insert(v.id); }
		io.hold();
		bg.maybe_merge();
		CHECK(bg.merges_pending() >= 1);
		io.await_arrival();  // a merge has read its sources and waits to write its output
		// Every segment loses a document and has another replaced; some deleted twice.
		for (int s = 0; s < 6; ++s) {
			both(tx({erase("Q" + std::to_string(s * 10 + 2)), replace("Q" + std::to_string(s * 10 + 3), "v1")}));
			both(tx({replace("Q" + std::to_string(s * 10 + 4), "v1"), replace("Q" + std::to_string(s * 10 + 4), "v2")}));
		}
		both(tx({erase("Q11"), replace("Q0", "back")}));  // already gone: a no-op, then a new docid
		CHECK(bg.digest() == twin.digest());
		io.release();
		bg.wait_merges();
		CHECK(bg.merges_pending() == 0 && bg.segments().size() < 6);
		CHECK(bg.digest() == twin.digest() && contents(bg) == contents(twin));
		bool carried = false;
		for (const auto& v : bg.segments()) { carried = carried || (!before.count(v.id) && v.deleted); }
		CHECK(carried);  // an output was born with the deletes made while it ran
		(void)bg.check();
		bg.commit();
		twin.commit();
		const auto digest = bg.digest();
		bg.close();
		bg = strata::index::open(io, small_tiers(&sched));
		CHECK(bg.digest() == digest && contents(bg) == contents(twin));
		(void)bg.check();
	}

	// Random schedules, seeded, on one and two merge threads shared by two indexes:
	// transactions, refreshes and commits merging after each flush, forced merges, and
	// waits, against inline twins. Then a power loss while merges run recovers the last
	// commit.
	for (std::size_t threads : {1, 2}) {
		for (std::uint64_t seed = 1; seed <= 6; ++seed) {
			merge_scheduler sched({threads, 0});
			twins a(sched, true), b(sched, true);
			twins* pair[] = {&a, &b};
			std::mt19937_64 r(seed * 7919 + threads);
			for (int step = 0; step < 300; ++step) {
				auto& p = *pair[r() % 2];
				const std::string key = "Q" + std::to_string(r() % 60), other = "Q" + std::to_string(r() % 60);
				const std::string body = "b" + std::to_string(r() % 7);
				switch (r() % 16) {
				case 0: case 1: case 2: case 3: case 4: case 5: case 6: p.apply(tx({replace(key, body)})); break;
				case 7: case 8: case 9: p.apply(tx({erase(key)})); break;
				case 10: p.apply(tx({replace(key, body), erase(other)})); break;
				case 11: case 12:
					p.bg.refresh();
					p.twin.refresh();
					break;
				case 13:
					p.bg.commit();
					p.twin.commit();
					break;
				case 14:
					if (r() % 2) {
						p.bg.force_merge_deletes();
						p.twin.force_merge_deletes();
					} else {
						p.bg.force_merge(2);
						p.twin.force_merge(2);
					}
					break;
				case 15: p.bg.wait_merges(); break;
				}
				if (step % 8 == 0) { CHECK(p.same()); }
			}
			for (auto* p : pair) {
				p->bg.wait_merges();
				CHECK(p->bg.merges_pending() == 0 && p->same());
				(void)p->bg.check();
				p->bg.commit();
				p->twin.commit();
				CHECK(p->same());
			}
			// Power loss with merges in flight: the reopened index is the last commit.
			const auto durable = a.bg.digest();
			for (int i = 0; i < 40; ++i) {
				a.bg.apply(tx({replace("Q" + std::to_string(r() % 60), "late")}), {});
				if (i % 4 == 3) { a.bg.refresh(); }
			}
			a.fs.crash(seed);
			a.bg.close();
			a.bg = strata::index::open(a.io, a.bo);
			CHECK(a.bg.digest() == durable);
			(void)a.bg.check();
		}
	}

	// Close and destruction while merges run or wait: queued merges are dropped and running
	// ones stopped, promptly, and reopening finds the last commit and no merge's file.
	{
		merge_scheduler slow({1, 8 << 10});  // each merge here takes seconds at this rate
		faultfs::filesystem fa, fb, fc;
		faultfs::io ia(fa), ib(fb), ic(fc);
		const auto o = small_tiers(&slow);
		auto a = strata::index::create(ia, o, uuid);
		auto b = strata::index::create(ib, o, uuid);
		fill(a, 6, 4, 3000, 1);
		fill(b, 6, 4, 3000, 2);
		const auto da = a.digest(), db = b.digest();
		{
			auto c = strata::index::create(ic, o, uuid);
			fill(c, 6, 4, 3000, 3);
			c.maybe_merge();
		}  // destroyed with its merges queued or running
		a.maybe_merge();
		b.maybe_merge();
		CHECK(a.merges_pending() >= 1 && b.merges_pending() >= 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto t0 = clock_type::now();
		b.close();
		a.close();
		CHECK(seconds_since(t0) < 2.0);
		a = strata::index::open(ia, o);
		b = strata::index::open(ib, o);
		auto c = strata::index::open(ic, o);
		CHECK(a.digest() == da && b.digest() == db && contents(c).size() == 24);
		for (const auto& [ix, fs] : {std::pair{&a, &fa}, std::pair{&b, &fb}, std::pair{&c, &fc}}) {
			std::set<std::string> named;
			for (const auto& s : ix->manifest().segments) { named.insert(index_detail::segment_name(s.id)); }
			CHECK(segment_files(*fs) == named);
			(void)ix->check();
		}
	}

	// The rate limit: a merge writing N bytes under L bytes per second takes at least N/L.
	{
		const std::uint64_t rate = 64 << 10;
		merge_scheduler limited({1, rate});
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = strata::index::create(io, small_tiers(&limited), uuid);
		fill(ix, 4, 4, 3000, 4);
		const auto digest = ix.digest();
		const auto t0 = clock_type::now();
		ix.force_merge(1);
		const double took = seconds_since(t0);
		const double n = double(limited.bytes_written());
		CHECK(ix.segments().size() == 1 && ix.digest() == digest);
		CHECK(n > 40000 && took >= 0.9 * n / double(rate));
		if (took < 0.9 * n / double(rate)) { std::fprintf(stderr, "  rate: %.0f bytes in %.3f s\n", n, took); }
	}

	// ENOSPC while a background merge writes discards it, its partial file removed and the
	// index as it was; force_merge reports it; once space returns the next merge succeeds.
	// EIO fences, and reopening recovers the last commit.
	{
		merge_scheduler sched({1, 0});
		faultfs::filesystem fs;
		faultfs::io io(fs);
		const auto o = small_tiers(&sched);
		auto ix = strata::index::create(io, o, uuid);
		fill(ix, 6, 4, 3000, 5);
		const auto digest = ix.digest();
		const auto good = contents(ix);
		const auto files = segment_files(fs);
		fs.set_capacity(fs.used() + 100);
		ix.maybe_merge();
		CHECK(ix.merges_pending() >= 1);
		ix.wait_merges();
		CHECK(ix.merges_pending() == 0 && ix.segments().size() == 6 && ix.digest() == digest && contents(ix) == good);
		CHECK(segment_files(fs) == files);
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
		fs.add_fault({faultfs::op::create, 1, EIO, false});  // the merge thread's create
		ix.maybe_merge();
		CHECK(ix.merges_pending() >= 1);
		CHECK(throws([&] { ix.wait_merges(); }));
		CHECK(throws([&] { ix.commit(); }));  // fenced
		fs.clear_faults();
		ix.close();
		ix = strata::index::open(io, o);
		CHECK(ix.digest() == committed && contents(ix).size() == 36);
	}

	// Install while merges run (D11's carry-over at install): the merges end unpublished,
	// the index holds exactly the checkpoint, and no merge's file stays behind.
	{
		merge_scheduler slow({1, 8 << 10});
		faultfs::filesystem fs, sfs;
		faultfs::io io(fs), sio(sfs);
		const auto o = small_tiers(&slow);
		auto src = strata::index::create(sio, small_tiers(nullptr), uuid);
		fill(src, 2, 5, 100, 7, 1000);
		const auto generation = src.checkpoint();
		const auto want = src.digest();
		const auto want_contents = contents(src);
		auto ix = strata::index::create(io, o, uuid);
		fill(ix, 6, 4, 3000, 8);
		const auto files = segment_files(fs);
		ix.maybe_merge();
		CHECK(ix.merges_pending() >= 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		ix.install(sio, generation);
		CHECK(ix.merges_pending() == 0);
		ix.wait_merges();
		CHECK(ix.digest() == want && contents(ix) == want_contents && ix.segments().size() == 2);
		ix.commit();
		ix.reclaim();
		std::set<std::string> named;
		for (const auto& s : ix.manifest().segments) { named.insert(index_detail::segment_name(s.id)); }
		for (const auto& n : segment_files(fs)) { CHECK(files.count(n) || named.count(n)); }
		ix.close();
		ix = strata::index::open(io, o);
		CHECK(ix.digest() == want && contents(ix) == want_contents);
	}
	std::printf("background_merge: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
