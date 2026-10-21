// The Phase 3 lifecycle gate rows on faultfs (design T1, T2):
//   - Replays: the same transactions under different flush and commit schedules give the
//     same verdicts and digests, and reopening keeps the digest.
//   - Checkpoints and install: an installed checkpoint has the source's digest, and a
//     crash during install opens the old content or the installed one.
//   - Power loss at every mutation of seeded workloads applied at log positions (half
//     under a small memory budget, flushing as it fills): reopening succeeds, its digest
//     and applied position are one of the commit points from the last acknowledged commit
//     to the last attempted, and replaying the whole log reaches the uninterrupted digest;
//     the index then merges, commits, and reopens to the same digest. At every 50th point,
//     a second power loss after each mutation of the recovery. Then the same with background
//     merges on a stepping executor, crash points covering the merges' mutations too, and
//     the commit points equal to an inline-merging twin's.
// PROSPECT_CRASH_POINTS sets how many crash points to cover (default 10,000), and
// PROSPECT_BG_CRASH_POINTS how many with background merges (default 2,000; the gate's
// count is 10,000, run as docs/gates/phase-3-lifecycle.md records).
#include "prospect/index.hh"
#include "stepping_executor.hh"
#include "strata/core/faultfs.hh"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace prospect;

static const std::array<char, 16> uuid_a = {'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a'};
static const std::array<char, 16> uuid_b = {'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b'};

// One step of a workload: a transaction, or a refresh, commit, or checkpoint.
struct step {
	enum kind { apply, refresh, commit, checkpoint, release, merge } type = apply;
	transaction t;
	std::size_t merge_to = 0;  // merge: at most this many segments, or 0 for the policy's merges
};

// Merges as a step asks; they change no verdict and no digest.
static void run_merge(prospect::index& ix, const step& s) {
	if (s.merge_to) {
		ix.force_merge(s.merge_to);
	} else {
		ix.maybe_merge();
	}
}

static document make_doc(std::mt19937_64& r, const std::string& key) {
	static const char* words[] = {"the", "of", "lord", "rings", "garden", "eden", "time", "kids"};
	document d;
	d.key = key;
	std::set<std::string> used;
	const std::size_t n = 1 + r() % 4;
	std::uint32_t pos = 0;
	for (std::size_t i = 0; i < n; ++i) {
		std::string w = words[r() % 8];
		if (!used.insert(w).second) { continue; }
		term_entry t{w, 1 + std::uint32_t(r() % 3), {}};
		for (std::uint32_t k = 0; k < t.wdf; ++k) { t.positions.push_back(pos += 1 + std::uint32_t(r() % 3)); }
		d.length += t.wdf;
		d.terms.push_back(std::move(t));
	}
	d.stored = "doc " + std::to_string(r() % 1000) + std::string(r() % 40, 'x');
	return d;
}

static std::vector<step> workload(std::uint64_t seed, std::size_t steps) {
	std::mt19937_64 r(seed);
	std::vector<step> out;
	auto key = [&] { return "Q" + std::to_string(r() % 25); };
	for (std::size_t i = 0; i < steps; ++i) {
		step s;
		const auto pick = r() % 100;
		if (pick < 4) {
			s.type = step::merge;
			s.merge_to = r() % 3;
		} else if (pick < 8) {
			s.type = step::refresh;
		} else if (pick < 16) {
			s.type = step::commit;
		} else if (pick < 18) {
			s.type = step::checkpoint;
		} else if (pick < 20) {
			s.type = step::release;  // the oldest checkpoint still pinned
		} else {
			const std::size_t ops = 1 + r() % 3;
			for (std::size_t k = 0; k < ops; ++k) {
				operation o;
				switch (r() % 9) {
				case 0: o.type = operation::add; o.doc = make_doc(r, r() % 3 ? key() : std::string()); break;
				case 1:
				case 2: o.type = operation::replace_key; o.key = key(); o.doc = make_doc(r, o.key); break;
				case 3: o.type = operation::delete_key; o.key = key(); break;
				case 4: o.type = operation::replace_docid; o.docid = 1 + r() % 40; o.doc = make_doc(r, r() % 2 ? key() : std::string()); break;
				case 5: o.type = operation::delete_docid; o.docid = 1 + r() % 40; break;
				case 6: o.type = operation::set_meta; o.meta_key = "m" + std::to_string(r() % 4); o.meta_value = std::to_string(r()); break;
				case 7: o.type = operation::delete_meta; o.meta_key = "m" + std::to_string(r() % 4); break;
				default: o.type = operation::replace_key; o.key = key(); o.doc = make_doc(r, o.key); o.expected_version = r() % 3; break;
				}
				s.t.ops.push_back(std::move(o));
			}
			if (r() % 20 == 0) { s.t.set_schema = r() % 3; }
			if (r() % 20 == 0) { s.t.expected_schema = r() % 3; }
		}
		out.push_back(std::move(s));
	}
	return out;
}

static transaction tx_of(operation o) {
	transaction t;
	t.ops.push_back(std::move(o));
	return t;
}

int main() {
	index::options keyed;
	keyed.key_prefix = "Q";
	// PROSPECT_LIFECYCLE_REORDER: every merge reorders, with test-sized thresholds, so each
	// crash point, replay, and background schedule below covers reordering merges (T5).
	const bool reorder_all = std::getenv("PROSPECT_LIFECYCLE_REORDER") != nullptr;
	if (reorder_all) {
		keyed.reordering = index::options::reorder_mode::merges;
		keyed.reorder_min_docs = 0;
		keyed.reorder_settings.min_df = 2;
		keyed.reorder_settings.bisection.min_partition = 4;
	}
	// T2: replays under different schedules agree on verdicts and digests, the digest after
	// every transaction included (the first ten seeds); over a keyed index, and over a
	// keyless one, whose documents keep version 0 however flushes fall (F9, F13-R18).
	const index::options keyless = [&] {
		index::options o = keyed;
		o.key_prefix.clear();
		return o;
	}();
	for (int profile = 0; profile < 2; ++profile)
	for (std::uint64_t seed = 0; seed < (profile ? 10u : 40u); ++seed) {
		const index::options& opts = profile ? keyless : keyed;
		const auto w = workload(seed, 200);
		std::vector<std::vector<std::uint64_t>> verdicts[3];
		std::vector<std::uint64_t> trace[3];  // the digest after every transaction (D12)
		std::uint64_t digests[3];
		for (int schedule = 0; schedule < 3; ++schedule) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = index::create(io, opts, uuid_a);
			std::uint64_t at = 0;
			for (const auto& s : w) {
				if (s.type == step::merge) {
					run_merge(ix, s);
					continue;
				}
				if (s.type != step::apply) {
					if (schedule == 0) { ix.commit(); }  // the workload's own points become commits
					continue;
				}
				auto v = ix.apply(s.t, {++at, 1});
				std::vector<std::uint64_t> record{std::uint64_t(v.result), v.op, v.current_version};
				record.insert(record.end(), v.docids.begin(), v.docids.end());
				record.insert(record.end(), v.versions.begin(), v.versions.end());
				verdicts[schedule].push_back(record);
				if (schedule == 1) { ix.refresh(); }  // a flush after every transaction
				if (seed < 10) { trace[schedule].push_back(ix.digest()); }
			}
			digests[schedule] = ix.digest();
			if (schedule == 2) {
				// No flush until the end; then reopening keeps the digest.
				ix.commit();
				CHECK(ix.digest() == digests[2]);
				ix.close();
				ix = index::open(io, opts);
				CHECK(ix.digest() == digests[2]);
				CHECK(ix.manifest().applied == files::position({at, 1}));
			}
		}
		CHECK(verdicts[0] == verdicts[1] && verdicts[1] == verdicts[2]);
		CHECK(trace[0] == trace[1] && trace[1] == trace[2]);
		CHECK(digests[0] == digests[1] && digests[1] == digests[2]);
	}
	// T2: replay after recovery and after install. A writer that stops at a commit and
	// replays the whole log skips what it applied and ends where an uninterrupted run does.
	for (std::uint64_t seed = 0; seed < 20; ++seed) {
		std::vector<transaction> log;
		for (const auto& s : workload(seed + 300, 200)) {
			if (s.type == step::apply) { log.push_back(s.t); }
		}
		auto record = [](const verdict& v) {
			std::vector<std::uint64_t> r{std::uint64_t(v.result), v.op, v.current_version};
			r.insert(r.end(), v.docids.begin(), v.docids.end());
			r.insert(r.end(), v.versions.begin(), v.versions.end());
			return r;
		};
		std::vector<std::vector<std::uint64_t>> want;
		std::uint64_t digest;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = index::create(io, keyed, uuid_a);
			for (std::size_t i = 0; i < log.size(); ++i) { want.push_back(record(ix.apply(log[i], {i + 1, 1}))); }
			ix.commit();
			digest = ix.digest();
		}
		const std::size_t half = log.size() / 2;
		for (int via_install = 0; via_install < 2; ++via_install) {
			faultfs::filesystem fs, other;
			faultfs::io io(fs), other_io(other);
			auto ix = index::create(io, keyed, uuid_a);
			for (std::size_t i = 0; i < half; ++i) { ix.apply(log[i], {i + 1, 1}); }
			ix.commit();
			if (via_install) {
				const std::uint64_t cp = ix.checkpoint();
				auto b = index::create(other_io, keyed, uuid_b);
				b.install(io, cp);
				ix = std::move(b);
			} else {
				ix.close();
				ix = index::open(io, keyed);
			}
			for (std::size_t i = 0; i < log.size(); ++i) {
				const auto v = ix.apply(log[i], {i + 1, 1});
				if (i < half) {
					CHECK(v.result == verdict::skipped);
				} else {
					CHECK(record(v) == want[i]);
				}
			}
			ix.commit();
			CHECK(ix.digest() == digest);
		}
	}
	// T2: a refresh between a delete and a re-add, and a merge dropping the deleted copy
	// before its key or docid returns, under four schedules: equal verdicts and digests.
	{
		std::mt19937_64 r(99);
		std::vector<transaction> log;
		auto put = [&](operation::kind type, const std::string& key, std::uint64_t docid = 0) {
			operation o;
			o.type = type;
			o.key = key;
			o.docid = docid;
			if (type == operation::add || type == operation::replace_key || type == operation::replace_docid) { o.doc = make_doc(r, key); }
			log.push_back(tx_of(std::move(o)));
		};
		for (int k = 0; k < 6; ++k) { put(operation::add, "Q" + std::to_string(k)); }
		put(operation::delete_key, "Q1");
		put(operation::replace_key, "Q1");
		put(operation::delete_docid, "", 3);
		put(operation::replace_docid, "Q2", 3);
		put(operation::delete_key, "Q4");
		put(operation::add, "Q4");
		put(operation::delete_docid, "", 6);
		put(operation::add, "Q5");
		std::vector<std::vector<std::uint64_t>> verdicts[4];
		std::uint64_t digests[4];
		for (int schedule = 0; schedule < 4; ++schedule) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = index::create(io, keyed, uuid_a);
			for (const auto& t : log) {
				const bool removes = t.ops[0].type == operation::delete_key || t.ops[0].type == operation::delete_docid;
				const auto v = ix.apply(t, {});
				std::vector<std::uint64_t> rec{std::uint64_t(v.result), v.current_version};
				rec.insert(rec.end(), v.docids.begin(), v.docids.end());
				rec.insert(rec.end(), v.versions.begin(), v.versions.end());
				verdicts[schedule].push_back(rec);
				if (schedule >= 1) { ix.refresh(); }
				if ((schedule == 2 && removes) || schedule == 3) { ix.force_merge(1); }  // drops deleted copies
			}
			ix.commit();
			digests[schedule] = ix.digest();
			ix.close();
			ix = index::open(io, keyed);
			CHECK(ix.digest() == digests[schedule]);
		}
		for (int i = 1; i < 4; ++i) { CHECK(verdicts[i] == verdicts[0] && digests[i] == digests[0]); }
		std::size_t accepted = 0;
		for (const auto& v : verdicts[0]) { accepted += v[0] == verdict::accepted; }
		CHECK(accepted == log.size());
	}
	// Checkpoints and install.
	for (std::uint64_t seed = 0; seed < 30; ++seed) {
		faultfs::filesystem fa, fb;
		faultfs::io ia(fa), ib(fb);
		auto a = index::create(ia, keyed, uuid_a);
		auto b = index::create(ib, keyed, uuid_b);
		for (const auto& s : workload(seed, 120)) {
			if (s.type == step::apply) { a.apply(s.t, {}); }
		}
		const std::uint64_t cp = a.checkpoint();
		const std::uint64_t want = a.digest();
		for (const auto& s : workload(seed + 1000, 60)) {
			if (s.type == step::apply) {
				a.apply(s.t, {});  // the source moves on; the checkpoint stays pinned
				b.apply(s.t, {});
			}
		}
		a.commit();
		b.commit();
		const std::uint64_t old_b = b.digest();
		if (seed % 2) {
			b.install(ia, cp);
			CHECK(b.digest() == want);
			b.close();
			b = index::open(ib, keyed);
			CHECK(b.digest() == want);
			a.release(cp);
		} else {
			// Power loss during install: the old content or the installed one.
			fb.power_off_after(seed * 3 % 40);
			try {
				b.install(ia, cp);
			} catch (const std::exception&) {
			}
			b.close();
			fb.crash(seed);
			b = index::open(ib, keyed);
			const std::uint64_t got = b.digest();
			CHECK(got == old_b || got == want);
		}
	}
	// T5: disk full. A write that runs out of space leaves the index usable: freeing space
	// and retrying the same step reaches the same digest as a run that never ran out. An
	// fsync that fails with ENOSPC fences; reopening returns the last acknowledged commit.
	for (std::uint64_t seed = 0; seed < 40; ++seed) {
		const auto w = workload(seed + 500, 150);
		std::uint64_t reference;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = index::create(io, keyed, uuid_a);
			for (const auto& s : w) {
				if (s.type == step::apply) {
					ix.apply(s.t, {});
				} else if (s.type == step::merge) {
					run_merge(ix, s);
				} else if (s.type == step::refresh) {
					ix.refresh();
				} else {
					ix.commit();
				}
			}
			ix.commit();
			reference = ix.digest();
		}
		std::mt19937_64 r(seed);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = index::create(io, keyed, uuid_a);
		std::size_t full = 0;
		for (const auto& s : w) {
			if (s.type == step::apply) {
				ix.apply(s.t, {});
				continue;
			}
			if (r() % 2) { fs.set_capacity(fs.used() + r() % 2000); }  // nearly full
			for (;;) {
				try {
					if (s.type == step::refresh) {
						ix.refresh();
					} else if (s.type == step::merge) {
						run_merge(ix, s);
					} else {
						ix.commit();
					}
					break;
				} catch (const std::system_error& e) {
					CHECK(e.code().value() == ENOSPC);
					++full;
					fs.set_capacity(~std::uint64_t(0));  // space returns; the same step again
				}
			}
			fs.set_capacity(~std::uint64_t(0));
		}
		ix.commit();
		CHECK(ix.digest() == reference);
		CHECK(full > 0 || seed % 7);  // most seeds hit a full disk at least once
		// ENOSPC at fsync time fences; reopening gives the last acknowledged commit.
		const std::uint64_t acked = ix.digest();
		operation o;
		o.type = operation::set_meta;
		o.meta_key = "after";
		o.meta_value = "x";
		ix.apply(tx_of(o), {});
		fs.add_fault({faultfs::op::sync, 1, ENOSPC, false});
		bool fenced = false;
		try {
			ix.commit();
		} catch (const index_fenced&) {
			fenced = true;
		}
		CHECK(fenced);
		fs.clear_faults();
		ix.close();
		ix = index::open(io, keyed);
		CHECK(ix.digest() == acked);
	}
	// recover --to under power loss at every mutation: reopening finds the old head or
	// the chosen generation, never a manifest between them.
	{
		const auto w = workload(4242, 120);
		auto replay = [&](faultfs::io& io, std::map<std::uint64_t, std::uint64_t>& digests) {
			auto ix = index::create(io, keyed, uuid_a);
			for (const auto& s : w) {
				switch (s.type) {
				case step::apply: ix.apply(s.t, {}); break;
				case step::refresh: ix.refresh(); break;
				case step::merge: run_merge(ix, s); break;
				case step::commit:
				case step::checkpoint:
					ix.commit();
					digests[ix.manifest().generation] = ix.digest();
					break;
				case step::release: break;
				}
			}
			ix.close();
		};
		std::map<std::uint64_t, std::uint64_t> digests;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			replay(io, digests);
		}
		CHECK(digests.size() >= 4);
		const auto head = digests.rbegin()->first;
		std::size_t tried = 0;
		for (std::uint64_t to : {head - 1, head - 2}) {
			std::uint64_t total;
			{
				faultfs::filesystem fs;
				faultfs::io io(fs);
				std::map<std::uint64_t, std::uint64_t> d;
				replay(io, d);
				const std::uint64_t base = fs.mutations();
				CHECK(index::open(io, keyed, to).digest() == digests[to]);
				total = fs.mutations() - base;
			}
			for (std::uint64_t at = 0; at <= total; ++at, ++tried) {
				faultfs::filesystem fs;
				faultfs::io io(fs);
				std::map<std::uint64_t, std::uint64_t> d;
				replay(io, d);
				fs.power_off_after(at);
				try {
					auto ix = index::open(io, keyed, to);
				} catch (const std::exception&) {
				}
				fs.crash(at * 7919 + to);
				const std::uint64_t got = index::open(io, keyed).digest();
				CHECK(got == digests[head] || got == digests[to]);
				if (got != digests[head] && got != digests[to]) { std::fprintf(stderr, "  recover --to %llu, crash point %llu\n", (unsigned long long)to, (unsigned long long)at); }
			}
		}
		CHECK(tried > 0);
	}
	// The budget T1 uses flushes the buffer as it fills: the same workload holds fewer
	// buffered documents at once under it than without it.
	{
		std::size_t peak[2] = {0, 0};
		for (int budgeted = 0; budgeted < 2; ++budgeted) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			byte_budget mem(32 * 1024);
			index::options o = keyed;
			if (budgeted) {
				o.budget = &mem;
				o.buffer_share = 4 * 1024;
			}
			auto ix = index::create(io, o, uuid_a);
			for (const auto& s : workload(7778, 150)) {
				if (s.type == step::apply) { ix.apply(s.t, {}); }
				peak[budgeted] = std::max(peak[budgeted], ix.buffered_count());
			}
		}
		CHECK(peak[1] < peak[0] / 2);
	}
	// install under power loss at every mutation, on a checkpoint with several segments,
	// live docs, and metadata; every third point loses power again during the recovery.
	// Reopening finds the target's old content or the installed one.
	{
		faultfs::filesystem sfs;
		faultfs::io sio(sfs);
		auto source = index::create(sio, keyed, uuid_a);
		for (const auto& s : workload(9090, 160)) {
			if (s.type == step::apply) { source.apply(s.t, {}); }
			if (s.type == step::refresh) { source.refresh(); }
		}
		const std::uint64_t cp = source.checkpoint();
		const std::uint64_t installed = source.digest();
		CHECK(source.segments().size() >= 2);
		auto prepare = [&](faultfs::io& io) {
			auto ix = index::create(io, keyed, uuid_b);
			for (const auto& s : workload(9091, 40)) {
				if (s.type == step::apply) { ix.apply(s.t, {}); }
			}
			ix.commit();
			return ix;
		};
		std::uint64_t total, old;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = prepare(io);
			old = ix.digest();
			const std::uint64_t base = fs.mutations();
			ix.install(sio, cp);
			total = fs.mutations() - base;
			CHECK(ix.digest() == installed);
		}
		for (std::uint64_t at = 0; at <= total; ++at) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			{
				auto ix = prepare(io);
				fs.power_off_after(at);
				try {
					ix.install(sio, cp);
				} catch (const std::exception&) {
				}
			}
			fs.crash(at * 31 + 5);
			if (at % 3 == 0) {
				fs.power_off_after(at % 7);
				try {
					auto ix = index::open(io, keyed);
				} catch (const std::exception&) {
				}
				fs.crash(at * 37 + 11);
			}
			const std::uint64_t got = index::open(io, keyed).digest();
			CHECK(got == old || got == installed);
		}
		CHECK(total > 10);
	}
	// create under power loss at every mutation, under several crash seeds (condition 2):
	// the directory then opens as the empty index, or as one that was never created, and
	// create runs again over it. A directory with a segment but no manifest stays corrupt;
	// create refuses an index, also one with no lock file, and an index whose
	// documents were all deleted does not read as never created when its manifests are lost.
	{
		std::mt19937_64 cr(5);
		auto add_op = [&](const std::string& key) {
			operation o;
			o.type = operation::replace_key;
			o.key = key;
			o.doc = make_doc(cr, key);
			return o;
		};
		auto throws = [](const auto& f) {
			try {
				f();
			} catch (...) {
				return true;
			}
			return false;
		};
		std::uint64_t total, empty;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			const std::uint64_t base = fs.mutations();
			auto ix = index::create(io, keyed, uuid_a);
			total = fs.mutations() - base;
			empty = ix.digest();
		}
		std::size_t reran = 0, crashes = 0;
		for (std::uint64_t at = 0; at <= total; ++at) {
			for (std::uint64_t seed = 0; seed < 50; ++seed, ++crashes) {
				faultfs::filesystem fs;
				faultfs::io io(fs);
				fs.power_off_after(at);
				try {
					auto ix = index::create(io, keyed, uuid_a);
				} catch (const std::exception&) {
				}
				fs.crash(at * 41 + seed * 7919 + 3);
				std::optional<prospect::index> ix;
				try {
					ix.emplace(index::open(io, keyed));
					CHECK(ix->digest() == empty);
				} catch (const index_not_created&) {
					++reran;
					ix.emplace(index::create(io, keyed, uuid_a));
				}
				CHECK(ix->apply(transaction{{add_op("Qk")}}, {}).result == verdict::accepted);
				ix->commit();
				ix->close();
				CHECK(index::open(io, keyed).digest() != empty);
			}
		}
		CHECK(total >= 5 && reran > 0);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		{
			auto ix = index::create(io, keyed, uuid_a);
			ix.apply(transaction{{add_op("Qk")}}, {});
			ix.commit();
		}
		CHECK(throws([&] { index::create(io, keyed, uuid_a); }));  // an index is there
		for (const auto& n : fs.list()) {
			if (n.starts_with("manifest-")) { io.remove(n); }
		}
		bool corrupt = false;
		try {
			index::open(io, keyed);
		} catch (const corrupt_index&) {
			corrupt = true;
		}
		CHECK(corrupt);  // segments without a manifest
		CHECK(throws([&] { index::create(io, keyed, uuid_a); }));
		{
			faultfs::filesystem lfs;
			faultfs::io lio(lfs);
			{
				auto ix = index::create(lio, keyed, uuid_a);
				for (int i = 0; i < 4; ++i) {
					ix.apply(transaction{{add_op("Q" + std::to_string(i))}}, {});
					ix.commit();
				}
			}
			for (const auto& n : lfs.list()) { CHECK(n != "owner.lock"); }  // the lock is the directory's (D2)
			CHECK(throws([&] { index::create(lio, keyed, uuid_a); }));  // an index with no lock file
		}
		{
			faultfs::filesystem dfs;
			faultfs::io dio(dfs);
			{
				auto ix = index::create(dio, keyed, uuid_a);
				ix.apply(transaction{{add_op("Qd")}}, {});
				operation m;
				m.type = operation::set_meta;
				m.meta_key = "m";
				m.meta_value = "v";
				ix.apply(transaction{{m}}, {});
				ix.commit();
				operation del;
				del.type = operation::delete_key;
				del.key = "Qd";
				ix.apply(transaction{{del}}, {});
				ix.commit();
				CHECK(ix.segments().empty());
				for (int i = 0; i < 3; ++i) { ix.commit(); }  // past the retained predecessors: no segment left
				ix.reclaim(~std::uint64_t(0));
			}
			for (const auto& n : dfs.list()) {
				CHECK(!n.starts_with("seg-"));  // only metadata beside the manifests
				if (n.starts_with("manifest-")) { dio.remove(n); }
			}
			bool uncreated = false;
			try {
				index::open(dio, keyed);
			} catch (const index_not_created&) {
				uncreated = true;
			} catch (const corrupt_index&) {
			}
			CHECK(!uncreated && throws([&] { index::create(dio, keyed, uuid_a); }));
		}
		std::printf("  create: %llu crash points under %zu crashes, %zu created again\n", (unsigned long long)total + 1, crashes, reran);
	}
	// T1: power loss at every mutation point of seeded workloads, with merges on the
	// writer's thread, then with background merges on a stepping executor (condition 7):
	// queued merge work runs a piece at a time (to its next write chunk or rate-limiter
	// wait) at seeded points between the writer's steps, so deletes and replaces land
	// between a merge's scheduling and its publication, and every mutation of the writer
	// and of the merges is a crash point. The background runs' commit points must equal an
	// inline-merging twin's.
	std::size_t target = 10000, bg_target = 2000;
	if (const char* env = std::getenv("PROSPECT_CRASH_POINTS")) { target = std::strtoull(env, nullptr, 10); }
	if (const char* env = std::getenv("PROSPECT_BG_CRASH_POINTS")) { bg_target = std::strtoull(env, nullptr, 10); }
	struct tally {
		std::size_t points = 0, workloads = 0, recovery_points = 0;
	};
	auto t1 = [&](bool background, std::size_t want, tally& n) {
		for (std::uint64_t seed = 0; n.points < want; ++seed, ++n.workloads) {
			const auto w = workload(seed + (background ? 99991 : 7777), 150);
			// One run's surroundings, fresh for each run so runs replay alike: the memory
			// budget, and for background merges the executor and its rate limiter.
			struct run_env {
				byte_budget mem{32 * 1024};
				std::unique_ptr<testing::stepping_executor> exec;
				std::unique_ptr<rate_limiter> rate;
				explicit run_env(bool background) {
					if (background) {
						exec = std::make_unique<testing::stepping_executor>();
						rate = std::make_unique<rate_limiter>(4096);  // 4 KiB chunks, each a step
					}
				}
			};
			// Odd workloads run under a small memory budget, so the buffer also flushes when
			// it passes its share. Background runs merge after every flush, by small tiers.
			auto options_for = [&](run_env& env) {
				index::options o = keyed;
				if (seed % 2) {
					o.budget = &env.mem;
					o.buffer_share = 4 * 1024;
				}
				if (background) {
					o.auto_merge = true;
					o.policy.segments_per_tier = 2;
					o.policy.floor_bytes = 1;
					o.background = env.exec.get();
					o.rate = env.rate.get();
				}
				return o;
			};
			using point = std::pair<std::uint64_t, files::position>;  // (digest, applied position)
			// Runs the workload at log positions, releasing the oldest pinned checkpoint at
			// each release step, and with an executor, stepping it 0-3 times after each step.
			// With `allowed`, it tracks the commit points recovery may return: the last
			// acknowledged and any attempted after it; with `trace`, every commit point.
			auto run_workload = [&](prospect::index& ix, testing::stepping_executor* exec, std::set<point>* allowed, std::vector<point>* trace) {
				std::uint64_t log = 0;
				std::deque<std::uint64_t> pins;
				std::mt19937_64 steps(seed * 2654435761u + 1);
				for (const auto& s : w) {
					switch (s.type) {
					case step::apply: ix.apply(s.t, {++log, 1}); break;
					case step::refresh: ix.refresh(); break;
					case step::merge: run_merge(ix, s); break;
					case step::commit:
					case step::checkpoint:
					case step::release: {  // each commits
						if (s.type == step::release && pins.empty()) { break; }
						const point p{ix.digest(), ix.manifest().applied};
						if (allowed) { allowed->insert(p); }  // attempted
						if (trace) { trace->push_back(p); }
						if (s.type == step::commit) {
							ix.commit();
						} else if (s.type == step::checkpoint) {
							pins.push_back(ix.checkpoint());
						} else {
							ix.release(pins.front());
							pins.pop_front();
						}
						if (allowed) { *allowed = {p}; }  // acknowledged: nothing older is allowed
						break;
					}
					}
					if (exec) {
						for (auto k = steps() % 4; k-- > 0;) { exec->step(); }
					}
				}
			};
			// A dry run counts the workload's mutations and gives the uninterrupted digest;
			// with background merges, an inline-merging twin must reach the same commit points.
			std::uint64_t total, final_digest;
			{
				faultfs::filesystem fs;
				faultfs::io io(fs);
				run_env env(background);
				auto ix = index::create(io, options_for(env), uuid_a);
				const std::uint64_t base = fs.mutations();
				std::vector<point> trace;
				run_workload(ix, env.exec.get(), nullptr, &trace);
				total = fs.mutations() - base;
				final_digest = ix.digest();
				if (background) {
					faultfs::filesystem tfs;
					faultfs::io tio(tfs);
					run_env inline_env(false);
					auto o = options_for(inline_env);
					o.background = nullptr;
					o.rate = nullptr;
					auto twin = index::create(tio, o, uuid_a);
					std::vector<point> twin_trace;
					run_workload(twin, nullptr, nullptr, &twin_trace);
					CHECK(trace == twin_trace && twin.digest() == final_digest);
				}
			}
			// The state power loss after `at` mutations leaves; returns the allowed commit
			// points. The index ends before the crash, with the power still off, so the merges
			// it stops change nothing.
			auto crashed = [&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t at) {
				std::set<point> allowed;
				{
					run_env env(background);
					auto ix = index::create(io, options_for(env), uuid_a);
					allowed = {{ix.digest(), ix.manifest().applied}};
					fs.power_off_after(at);
					try {
						run_workload(ix, env.exec.get(), &allowed, nullptr);
					} catch (const std::exception&) {
					}
				}
				fs.crash(seed * 1000003 + at);
				return allowed;
			};
			// Opens after the crash: the recovered pair is an allowed commit point, replaying
			// the whole log reaches the uninterrupted digest (condition 1), and the index keeps
			// working: a full merge, a commit, and a reopen to the same digest (condition 3).
			auto recovered = [&](faultfs::io& io, const std::set<point>& allowed, std::uint64_t at) {
				try {
					auto ix = index::open(io, keyed);
					const point got{ix.digest(), ix.manifest().applied};
					CHECK(allowed.count(got));
					if (!allowed.count(got)) { std::fprintf(stderr, "  workload %llu crash point %llu%s\n", (unsigned long long)seed, (unsigned long long)at, background ? " (background merges)" : ""); }
					std::uint64_t log = 0;
					for (const auto& s : w) {
						if (s.type == step::apply) { ix.apply(s.t, {++log, 1}); }
					}
					CHECK(ix.digest() == final_digest);
					ix.force_merge(1);
					ix.commit();
					const std::uint64_t d = ix.digest();
					ix.close();
					CHECK(index::open(io, keyed).digest() == d);
				} catch (const std::exception& e) {
					CHECK(!"inconsistent open after power loss");
					std::fprintf(stderr, "  workload %llu crash point %llu%s: %s\n", (unsigned long long)seed, (unsigned long long)at, background ? " (background merges)" : "", e.what());
				}
			};
			for (std::uint64_t at = 0; at <= total && n.points < want; ++at, ++n.points) {
				{
					faultfs::filesystem fs;
					faultfs::io io(fs);
					const auto allowed = crashed(fs, io, at);
					if (at % 3 == 0) {
						// A second power loss during the recovery that follows.
						fs.power_off_after(at % 7);
						try {
							auto ix = index::open(io, keyed);
						} catch (const std::exception&) {
						}
						fs.crash(seed * 1000033 + at);
					}
					recovered(io, allowed, at);
				}
				// At every 50th point, a second power loss after each mutation of the recovery.
				if (at % 50 == 0) {
					std::uint64_t steps;
					{
						faultfs::filesystem fs;
						faultfs::io io(fs);
						crashed(fs, io, at);
						const std::uint64_t base = fs.mutations();
						try {
							auto ix = index::open(io, keyed);
						} catch (const std::exception&) {
						}
						steps = fs.mutations() - base;
					}
					for (std::uint64_t k = 0; k < steps; ++k, ++n.recovery_points) {
						faultfs::filesystem fs;
						faultfs::io io(fs);
						const auto allowed = crashed(fs, io, at);
						fs.power_off_after(k);
						try {
							auto ix = index::open(io, keyed);
						} catch (const std::exception&) {
						}
						fs.crash(seed * 1000039 + at * 31 + k);
						recovered(io, allowed, at);
					}
				}
			}
		}
	};
	// In reorder mode the runs above must really have reordered: a forced merge of a
	// workload's documents writes a permuted segment.
	if (reorder_all) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = index::create(io, keyed, uuid_a);
		std::mt19937_64 r(5);
		for (int i = 0; i < 200; ++i) {
			operation o;
			o.type = operation::add;
			o.doc = make_doc(r, "");
			(void)ix.apply(tx_of(std::move(o)), {});
			if (i % 40 == 39) { ix.refresh(); }
		}
		ix.force_merge();
		CHECK(ix.segments().size() == 1 && !ix.segments()[0].seg->core().docid_ordered());
	}
	tally inline_merges, background_merges;
	t1(false, target, inline_merges);
	t1(true, bg_target, background_merges);
	std::printf("lifecycle: %zu crash points over %zu workloads, %zu inside recoveries; with background merges, %zu over %zu, %zu inside recoveries: %s\n",
		inline_merges.points, inline_merges.workloads, inline_merges.recovery_points, background_merges.points, background_merges.workloads,
		background_merges.recovery_points, failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
