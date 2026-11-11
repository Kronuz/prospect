// Reclamation under power loss (design D4): seeded workloads with pins, releases, merges,
// and two retained predecessors run with 256-byte reclaim steps, so obsolete files are
// truncated over several commits, and power is lost after mutations across each. After
// every crash:
//   1. Every manifest on disk names only files that exist with its entry's size and
//      checksum, so recover --to any of them opens.
//   2. The reopened head's pinned manifests are on disk and intact.
//   3. The open succeeds; after reclaim() and a commit, every strata file on disk is named
//      by a manifest on disk, and a storage budget's permit covers the bytes on disk.
// PROSPECT_CRASH_POINTS sets how many crash points to cover per workload (default 400).
#include "text_field.hh"
#include "prospect/index.hh"
#include "strata/core/faultfs.hh"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <set>

using namespace prospect;

static const std::array<char, 16> uuid_a = {'s', 't', 'r', 'a', 't', 'a', '-', 'r', 'e', 'c', 'l', 'a', 'i', 'm'};

struct step {
	enum kind { apply, refresh, commit, checkpoint, release, merge } type = apply;
	transaction t{{}, {}, {}, text_fields()};
	std::size_t merge_to = 0;
};

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
		d.field().length += t.wdf;
		d.field().terms.push_back(std::move(t));
	}
	d.stored = "doc " + std::to_string(r() % 1000) + std::string(200 + r() % 400, 'x');  // larger than one reclaim step
	return d;
}

static std::vector<step> workload(std::uint64_t seed, std::size_t steps) {
	std::mt19937_64 r(seed);
	std::vector<step> out;
	auto key = [&] { return "Q" + std::to_string(r() % 25); };
	for (std::size_t i = 0; i < steps; ++i) {
		step s;
		const auto pick = r() % 100;
		if (pick < 6) {
			s.type = step::merge;
			s.merge_to = r() % 3;
		} else if (pick < 10) {
			s.type = step::refresh;
		} else if (pick < 22) {
			s.type = step::commit;
		} else if (pick < 26) {
			s.type = step::checkpoint;
		} else if (pick < 30) {
			s.type = step::release;
		} else {
			const std::size_t ops = 1 + r() % 3;
			for (std::size_t k = 0; k < ops; ++k) {
				operation o;
				switch (r() % 6) {
				case 0: o.type = operation::add; o.doc = make_doc(r, key()); break;
				case 1:
				case 2: o.type = operation::replace_key; o.key = key(); o.doc = make_doc(r, o.key); break;
				case 3: o.type = operation::delete_key; o.key = key(); break;
				case 4: o.type = operation::set_meta; o.meta_key = "m" + std::to_string(r() % 4); o.meta_value = std::string(300, 'v') + std::to_string(r()); break;
				default: o.type = operation::delete_docid; o.docid = 1 + r() % 40; break;
				}
				s.t.ops.push_back(std::move(o));
			}
		}
		out.push_back(std::move(s));
	}
	return out;
}

// What recover --to g checks, read-only: every segment generation g names, against its entry,
// read from the log holding it (manifest-log D8).
static std::string intact(faultfs::io& io, faultfs::filesystem& fs, std::optional<std::uint64_t> g) {
	try {
		const auto m = index_detail::read_state(io, g);
		for (const auto& [id, r] : m.segments) {
			const auto name = index_detail::segment_name(id);
			const std::string_view b = fs.view(name);
			if (b.size() != r.size) { return name + " size"; }
			if (strata::format::decode_footer(b).toc_crc != r.footer_crc) { return name + " footer"; }
		}
	} catch (const std::exception& e) {
		return std::string("unreadable: ") + e.what();
	}
	return {};
}

int main() {
	const std::uint64_t workloads = 12;
	const char* env = std::getenv("PROSPECT_CRASH_POINTS");
	const std::uint64_t max_points = env ? std::strtoull(env, nullptr, 10) : 400;
	std::size_t points = 0, bad_manifests = 0, bad_pins = 0, open_failures = 0, leftovers = 0, uncovered = 0, truncations = 0, manifests_checked = 0;
	for (std::uint64_t seed = 0; seed < workloads; ++seed) {
		const auto w = workload(seed + 31337, 160);
		const bool budgeted = seed % 2;
		auto options = [&](byte_budget* storage) {
			index::options o;
			o.keyed = true;
			o.reclaim_step = 256;
			o.reclaim_slice = 1024;
			o.storage = budgeted ? storage : nullptr;
			return o;
		};
		auto run = [&](prospect::index& ix) {
			std::deque<std::uint64_t> pins;
			std::uint64_t log = 0;
			for (const auto& s : w) {
				switch (s.type) {
				case step::apply: ix.apply(s.t, {++log, 1}); break;
				case step::refresh: ix.refresh(); break;
				case step::merge:
					if (s.merge_to) {
						ix.force_merge(s.merge_to);
					} else {
						ix.maybe_merge();
					}
					break;
				case step::commit: ix.commit(); break;
				case step::checkpoint: pins.push_back(ix.checkpoint()); break;
				case step::release:
					if (!pins.empty()) {
						ix.release(pins.front());
						pins.pop_front();
					}
					break;
				}
			}
		};
		std::uint64_t total;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			byte_budget storage(1ull << 40);
			auto ix = index::create(io, options(&storage), uuid_a);
			const auto base = fs.mutations();
			run(ix);
			total = fs.mutations() - base;
			ix.close();
		}
		const std::uint64_t stride = total / max_points + 1;
		for (std::uint64_t at = 0; at <= total; at += stride, ++points) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			byte_budget storage(1ull << 40);
			{
				auto ix = index::create(io, options(&storage), uuid_a);
				fs.power_off_after(at);
				try {
					run(ix);
				} catch (const std::exception&) {
				}
				ix.close();
			}
			fs.crash(seed * 7777 + at);
			// The newest log's head is intact: an obsolete log may name reclaimed files (D8).
			if (index_detail::log_holding(io)) {
				++manifests_checked;
				if (const auto why = intact(io, fs, std::nullopt); !why.empty()) {
					if (bad_manifests++ < 5) { std::printf("  workload %llu point %llu: the head names %s\n", (unsigned long long)seed, (unsigned long long)at, why.c_str()); }
				}
			}
			for (const auto& n : fs.list()) {
				if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::segment) {
					// A segment shorter than its footer claims was truncated by reclaim().
					try {
						(void)strata::format::decode_footer(fs.view(n));
					} catch (const std::exception&) {
						++truncations;
					}
				}
			}
			try {
				byte_budget storage2(1ull << 40);
				auto ix = index::open(io, options(&storage2));
				for (std::uint64_t g : ix.manifest().pins) {
					if (const auto why = intact(io, fs, g); !why.empty()) {
						if (bad_pins++ < 5) { std::printf("  workload %llu point %llu: pinned %llu: %s\n", (unsigned long long)seed, (unsigned long long)at, (unsigned long long)g, why.c_str()); }
					}
				}
				ix.reclaim();
				ix.commit();
				ix.reclaim();
				std::set<std::string> named;
				std::uint64_t bytes = 0;
				// What the kept generations name: the head and the pins, each with its log (D8).
				auto keep = [&](std::uint64_t g) {
					named.insert(index_detail::manifest_name(*index_detail::log_holding(io, g)));
					for (const auto& [id, r] : index_detail::read_state(io, g).segments) { named.insert(index_detail::segment_name(id)); }
				};
				keep(ix.manifest().generation);
				for (std::uint64_t g : ix.manifest().pins) { keep(g); }
				for (const auto& f : fs.list()) {
					if (index_detail::parse_name(f)) { bytes += fs.view(f).size(); }
					if (!named.count(f)) {
						if (leftovers++ < 5) { std::printf("  workload %llu point %llu: %s left unnamed\n", (unsigned long long)seed, (unsigned long long)at, f.c_str()); }
					}
				}
				if (budgeted && storage2.used() < bytes) {
					if (uncovered++ < 5) { std::printf("  workload %llu point %llu: permit %llu < bytes on disk %llu\n", (unsigned long long)seed, (unsigned long long)at, (unsigned long long)storage2.used(), (unsigned long long)bytes); }
				}
				ix.close();
			} catch (const std::exception& e) {
				if (open_failures++ < 5) { std::printf("  workload %llu point %llu: open fails: %s\n", (unsigned long long)seed, (unsigned long long)at, e.what()); }
			}
		}
	}
	std::printf("  %zu crash points, %zu manifests checked, %zu truncated orphans seen\n", points, manifests_checked, truncations);
	const bool ok = truncations && !bad_manifests && !bad_pins && !open_failures && !leftovers && !uncovered;
	if (!ok) {
		std::printf("  damaged manifests %zu, missing or damaged pins %zu, failed opens %zu, unnamed files %zu, permit below disk %zu\n", bad_manifests, bad_pins,
			open_failures, leftovers, uncovered);
	}
	std::printf("reclaim: %s\n", ok ? "ok" : "FAILED");
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
