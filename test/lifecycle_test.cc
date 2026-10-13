// The Phase 3 lifecycle gate rows on faultfs (design T1, T2):
//   - Replays: the same transactions under different flush and commit schedules give the
//     same verdicts and digests, and reopening keeps the digest.
//   - Checkpoints and install: an installed checkpoint has the source's digest, and a
//     crash during install opens the old content or the installed one.
//   - Power loss at every mutation of seeded workloads: reopening succeeds and its digest
//     is one of the commit points from the last acknowledged commit to the last attempted.
// STRATA_CRASH_POINTS sets how many crash points to cover (default 10,000).
#include "strata/faultfs.hh"
#include "strata/index.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace strata;

static const std::array<char, 16> uuid_a = {'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a'};
static const std::array<char, 16> uuid_b = {'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b'};

// One step of a workload: a transaction, or a refresh, commit, or checkpoint.
struct step {
	enum kind { apply, refresh, commit, checkpoint, merge } type = apply;
	transaction t;
	std::size_t merge_to = 0;  // merge: at most this many segments, or 0 for the policy's merges
};

// Merges as a step asks; they change no verdict and no digest.
static void run_merge(strata::index& ix, const step& s) {
	if (s.merge_to) {
		ix.force_merge(s.merge_to);
	} else {
		ix.maybe_merge();
	}
}

static document make_doc(std::mt19937_64& r, const std::string& key) {
	static const char* words[] = {"the", "of", "lord", "rings", "garden", "eden", "time", "kids"};
	document d;
	if (!key.empty()) { d.terms.push_back({key, 0, {}}); }
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
	// T2: replays under different schedules agree on verdicts and digests.
	for (std::uint64_t seed = 0; seed < 40; ++seed) {
		const auto w = workload(seed, 200);
		std::vector<std::vector<std::uint64_t>> verdicts[3];
		std::uint64_t digests[3];
		for (int schedule = 0; schedule < 3; ++schedule) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = index::create(io, keyed, uuid_a);
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
			}
			digests[schedule] = ix.digest();
			if (schedule == 2) {
				// No flush until the end; then reopening keeps the digest.
				ix.commit();
				CHECK(ix.digest() == digests[2]);
				ix.close();
				ix = index::open(io, keyed);
				CHECK(ix.digest() == digests[2]);
				CHECK(ix.manifest().applied == files::position({at, 1}));
			}
		}
		CHECK(verdicts[0] == verdicts[1] && verdicts[1] == verdicts[2]);
		CHECK(digests[0] == digests[1] && digests[1] == digests[2]);
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
	// T1: power loss at every mutation point of seeded workloads.
	std::size_t target = 10000;
	if (const char* env = std::getenv("STRATA_CRASH_POINTS")) { target = std::strtoull(env, nullptr, 10); }
	std::size_t points = 0, workloads = 0;
	for (std::uint64_t seed = 0; points < target; ++seed, ++workloads) {
		const auto w = workload(seed + 7777, 150);
		// A dry run counts the workload's mutations.
		std::uint64_t total;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = index::create(io, keyed, uuid_a);
			const std::uint64_t base = fs.mutations();
			for (const auto& s : w) {
				switch (s.type) {
				case step::apply: ix.apply(s.t, {}); break;
				case step::refresh: ix.refresh(); break;
				case step::commit: ix.commit(); break;
				case step::checkpoint: ix.checkpoint(); break;
				case step::merge: run_merge(ix, s); break;
				}
			}
			total = fs.mutations() - base;
		}
		for (std::uint64_t at = 0; at <= total && points < target; ++at, ++points) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			std::set<std::uint64_t> allowed;
			{
				auto ix = index::create(io, keyed, uuid_a);
				allowed = {ix.digest()};
				fs.power_off_after(at);
				try {
					for (const auto& s : w) {
						switch (s.type) {
						case step::apply: ix.apply(s.t, {}); break;
						case step::refresh: ix.refresh(); break;
						case step::merge: run_merge(ix, s); break;
						case step::commit:
						case step::checkpoint: {
							const std::uint64_t d = ix.digest();
							allowed.insert(d);  // attempted
							if (s.type == step::commit) {
								ix.commit();
							} else {
								ix.checkpoint();
							}
							allowed = {d};  // acknowledged: nothing older is allowed
							break;
						}
						}
					}
				} catch (const std::exception&) {
				}
			}
			fs.crash(seed * 1000003 + at);
			if (at % 3 == 0) {
				// A second power loss during the recovery that follows.
				fs.power_off_after(at % 7);
				try {
					auto ix = index::open(io, keyed);
				} catch (const std::exception&) {
				}
				fs.crash(seed * 1000033 + at);
			}
			try {
				auto ix = index::open(io, keyed);
				const std::uint64_t got = ix.digest();
				CHECK(allowed.count(got));
				if (!allowed.count(got)) { std::fprintf(stderr, "  workload %llu crash point %llu\n", (unsigned long long)seed, (unsigned long long)at); }
			} catch (const std::exception& e) {
				CHECK(!"inconsistent open after power loss");
				std::fprintf(stderr, "  workload %llu crash point %llu: %s\n", (unsigned long long)seed, (unsigned long long)at, e.what());
			}
		}
	}
	std::printf("lifecycle: %zu crash points over %zu workloads: %s\n", points, workloads, failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
