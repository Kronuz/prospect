// An index: one shard's segments, live docs, metadata, and manifests in one directory,
// with one writer (Phase 3 design). Transactions apply atomically on the owner core;
// buffered documents flush to segments sorted by docid; commit() makes everything
// applied so far durable through the commit protocol; open() recovers the last durable
// manifest and fails closed when a file it names is missing or differs.
#pragma once

#include "strata/budget.hh"
#include "strata/check.hh"
#include "strata/executor.hh"
#include "strata/files.hh"
#include "strata/merge.hh"
#include "strata/merge_policy.hh"
#include "strata/rate_limiter.hh"
#include "strata/reader.hh"
#include "strata/reorder.hh"
#include "strata/tuple_buffer.hh"
#include "strata/writer.hh"

#include "completion/io.h"
#include "hashes.hh"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <deque>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace strata {

template <class Layout>
struct basic_operation {
	enum kind : std::uint8_t { add, replace_key, delete_key, replace_docid, delete_docid, set_meta, delete_meta };
	kind type = add;
	std::uint64_t docid = 0;                       // replace_docid, delete_docid
	std::string key;                               // replace_key, delete_key: the key term
	std::optional<std::uint64_t> expected_version;  // 0: must be absent
	basic_document<Layout> doc;                    // add, replace_*
	std::string meta_key, meta_value;              // set_meta, delete_meta
};
using operation = basic_operation<search_layout>;

template <class Layout>
struct basic_transaction {
	std::vector<basic_operation<Layout>> ops;
	std::optional<std::uint64_t> expected_schema;  // the schema version it was built against
	std::optional<std::uint64_t> set_schema;       // the schema version it sets
};
using transaction = basic_transaction<search_layout>;

// Apply's answer. A rejected transaction left no trace.
struct verdict {
	enum outcome : std::uint8_t { accepted, version_conflict, schema_conflict, invalid, skipped, no_space };
	outcome result = accepted;
	std::size_t op = 0;                  // the rejecting operation
	std::uint64_t current_version = 0;   // on a version conflict
	std::vector<std::uint64_t> docids;   // per document operation, as assigned
	std::vector<std::uint64_t> versions;  // per document operation, as written
};

// The writer stopped after a durability failure; reopen to continue (design D2).
struct index_fenced : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// The directory holds no committed index: no manifest and no segment, only what an
// interrupted create leaves. create() runs again over it.
struct index_not_created : std::runtime_error {
	using std::runtime_error::runtime_error;
};

namespace index_detail {

inline std::string segment_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".sx"; }
inline std::string live_name(std::uint64_t id, std::uint64_t gen) { return "seg-" + std::to_string(id) + "." + std::to_string(gen) + ".live"; }
inline std::string spill_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".spill"; }
inline std::string forward_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".fwd"; }
inline std::string records_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".rec"; }
inline std::string meta_name(std::uint64_t gen) { return "meta-" + std::to_string(gen) + ".kv"; }
inline std::string manifest_name(std::uint64_t n) { return "manifest-" + std::to_string(n); }

// Parses a decimal field: "123" with no sign, no leading zero, no overflow.
inline std::optional<std::uint64_t> number(std::string_view s) {
	if (s.empty() || (s.size() > 1 && s[0] == '0')) { return std::nullopt; }
	std::uint64_t v = 0;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc() || p != s.data() + s.size()) { return std::nullopt; }
	return v;
}

// The kind and numbers of one of strata's file names, or nothing for other names.
struct parsed_name {
	enum kind { segment, live, meta, manifest, manifest_tmp, spill, scratch } type;
	std::uint64_t a = 0, b = 0;
};
inline std::optional<parsed_name> parse_name(std::string_view n) {
	auto strip = [&](std::string_view prefix, std::string_view suffix) -> std::optional<std::string_view> {
		if (n.size() < prefix.size() + suffix.size() || !n.starts_with(prefix) || !n.ends_with(suffix)) { return std::nullopt; }
		return n.substr(prefix.size(), n.size() - prefix.size() - suffix.size());
	};
	if (auto m = strip("manifest-", ".tmp")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::manifest_tmp, *v}; }
	}
	if (auto m = strip("manifest-", "")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::manifest, *v}; }
	}
	if (auto m = strip("meta-", ".kv")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::meta, *v}; }
	}
	if (auto m = strip("seg-", ".sx")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::segment, *v}; }
	}
	if (auto m = strip("seg-", ".spill")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::spill, *v}; }
	}
	for (const char* suffix : {".fwd", ".rec"}) {
		if (auto m = strip("seg-", suffix)) {
			if (auto v = number(*m)) { return parsed_name{parsed_name::scratch, *v}; }
		}
	}
	if (auto m = strip("seg-", ".live")) {
		const auto dot = m->find('.');
		if (dot != std::string_view::npos) {
			auto id = number(m->substr(0, dot));
			auto gen = number(m->substr(dot + 1));
			if (id && gen) { return parsed_name{parsed_name::live, *id, *gen}; }
		}
	}
	return std::nullopt;
}

inline std::string read_all(kronuz::journal::File& f) {
	const std::uint64_t size = f.size();
	std::string out(size, '\0');
	for (std::uint64_t at = 0; at < size;) {
		const std::size_t n = f.read_at(at, std::span<char>(out.data() + at, size - at));
		if (n == 0) { throw corrupt_index("short read"); }
		at += n;
	}
	return out;
}

// Writes a file from its start, gathered a MiB at a time.
class file_sink final : public output {
public:
	explicit file_sink(kronuz::journal::File& f) : f_(f) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (pending_.size() + size > gather) { drain(); }
		if (size >= gather) {
			put(std::string_view(data, size));
		} else {
			pending_.append(data, size);
		}
	}
	void finish() { drain(); }
	std::uint64_t size() const noexcept { return at_ + pending_.size(); }

private:
	static constexpr std::size_t gather = 1 << 20;
	void drain() {
		put(pending_);
		pending_.clear();
	}
	void put(std::string_view bytes) {
		while (!bytes.empty()) {
			const std::size_t n = f_.write_at(at_, bytes);
			if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
			at_ += n;
			bytes.remove_prefix(n);
		}
	}
	kronuz::journal::File& f_;
	std::string pending_;
	std::uint64_t at_ = 0;
};

inline void write_all(kronuz::journal::File& f, std::string_view bytes) {
	for (std::uint64_t at = 0; at < bytes.size();) {
		const std::size_t n = f.write_at(at, bytes.substr(at));
		if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
		at += n;
	}
}

// 32-bit values by 64-bit hash in one array, at most half full, probed linearly; entries may
// share a hash, so lookups take a test of the value. Removal shifts the entries after it
// back, so there are no tombstones.
class hash_slots {
public:
	template <class Eq>
	std::uint32_t* find(std::uint64_t h, Eq&& eq) noexcept {
		const std::size_t i = index_of(h, eq);
		return i == npos ? nullptr : &slots_[i].value;
	}
	template <class Eq>
	const std::uint32_t* find(std::uint64_t h, Eq&& eq) const noexcept {
		const std::size_t i = index_of(h, eq);
		return i == npos ? nullptr : &slots_[i].value;
	}
	// A new entry; the caller found none equal.
	void insert(std::uint64_t h, std::uint32_t value) {
		if (2 * (size_ + 1) > slots_.size()) { grow(); }
		place(h, value);
		++size_;
	}
	template <class Eq>
	bool erase(std::uint64_t h, Eq&& eq) noexcept {
		std::size_t i = index_of(h, eq);
		if (i == npos) { return false; }
		for (std::size_t j = i;;) {
			j = (j + 1) & mask_;
			if (slots_[j].value == empty) { break; }
			const std::size_t k = home(slots_[j].hash);
			// slots_[j] moves back to i unless its home lies cyclically in (i, j].
			if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) { continue; }
			slots_[i] = slots_[j];
			i = j;
		}
		slots_[i].value = empty;
		--size_;
		return true;
	}
	void clear() noexcept {
		std::vector<slot>().swap(slots_);
		size_ = 0, mask_ = 0;
	}
	std::size_t size() const noexcept { return size_; }
	std::uint64_t bytes() const noexcept { return slots_.capacity() * sizeof(slot); }

private:
	static constexpr std::uint32_t empty = ~std::uint32_t(0);
	static constexpr std::size_t npos = ~std::size_t(0);
	struct slot {
		std::uint64_t hash;
		std::uint32_t value;
	};
	template <class Eq>
	std::size_t index_of(std::uint64_t h, Eq& eq) const noexcept {
		if (!size_) { return npos; }
		for (std::size_t i = home(h);; i = (i + 1) & mask_) {
			const slot& s = slots_[i];
			if (s.value == empty) { return npos; }
			if (s.hash == h && eq(s.value)) { return i; }
		}
	}
	std::size_t home(std::uint64_t h) const noexcept { return std::size_t((h * 0x9E3779B97F4A7C15ull) >> 32) & mask_; }
	void place(std::uint64_t h, std::uint32_t value) noexcept {
		std::size_t i = home(h);
		while (slots_[i].value != empty) { i = (i + 1) & mask_; }
		slots_[i] = {h, value};
	}
	void grow() {
		std::vector<slot> old(std::max<std::size_t>(16, 2 * slots_.size()), slot{0, empty});
		old.swap(slots_);
		mask_ = slots_.size() - 1;
		for (const slot& s : old) {
			if (s.value != empty) { place(s.hash, s.value); }
		}
	}
	std::vector<slot> slots_;
	std::size_t size_ = 0, mask_ = 0;
};

}  // namespace index_detail

struct index_options {
	std::string key_prefix;                 // the host's key-term prefix (Xapiand's "Q"); empty: no keys
	std::size_t retained_predecessors = 2;  // manifests kept behind the current one (I14)
	// The node's indexing budget, if any, and this index's share of it: the buffer flushes
	// when the budget is under pressure, when the buffer passes its share, or when the
	// version map passes a quarter of the share (D17).
	byte_budget* budget = nullptr;
	std::uint64_t buffer_share = ~std::uint64_t(0);
	// The node's storage, if any (T5): the index holds its files' bytes and reserves each
	// transaction's estimated bytes at admission and each merge's sources' live bytes
	// before it starts; a transaction that does not fit is rejected with no_space.
	// Both budgets must outlive every index that draws on them.
	byte_budget* storage = nullptr;
	// Merges after each flush by `policy` when set; otherwise only merge() and force_merge().
	bool auto_merge = false;
	// Background merges (D11, D18): the node's executor, if any, runs the policy's merges,
	// which publish at the next refresh or commit; without one they run on the caller's
	// thread. `rate` limits their writes. A merge waits up to `merge_memory_wait` (the
	// executor's nanoseconds) for room on `budget`, then draws past it as a flush does.
	// Both must outlive every index that uses them.
	executor* background = nullptr;
	rate_limiter* rate = nullptr;
	std::uint64_t merge_memory_wait = 5'000'000'000;
	// Writer threads for inversion (D18): a flush splits the buffer's docid-sorted documents
	// into up to this many contiguous runs and inverts and encodes each on its own thread,
	// one segment per run. Files, ids, and accounting stay on the owner thread.
	std::size_t flush_threads = 1;
	std::size_t flush_min_docs = 4096;  // per run: smaller buffers flush as one segment
	// Obsolete data files leave in steps: truncated from the end `reclaim_step` bytes at a
	// time, then removed, so freeing a large file never stalls one call. Each commit
	// reclaims about `reclaim_slice` bytes, reclaim() the rest, and admission drains them
	// when space is short.
	std::uint64_t reclaim_step = 16 << 20;
	std::uint64_t reclaim_slice = 64 << 20;
	merge_policy policy;
	// Reordering at merge (docs/design/reordering.md D4): `force_merge` reorders the merges
	// force_merge() runs, as Lucene's BP reorderer does; `merges` also every merge whose
	// output holds at least `reorder_min_docs` live documents.
	enum class reorder_mode { off, force_merge, merges };
	reorder_mode reordering = reorder_mode::force_merge;
	std::uint64_t reorder_min_docs = 262144;
	strata::reorder::settings reorder_settings;
	// The seed of the segments' identities (F13-R41); none: a random one per open. Tests that
	// compare two indexes' files give both the same seed.
	std::optional<std::uint64_t> identity_seed;
	// A directory on a filesystem not known local (a network or FUSE mount), or one whose
	// lock is unsupported: `warn` opens it and reports it (basic_index::filesystem_warning);
	// `refuse` refuses with errc::not_supported, except a FUSE mount's type (F20). The
	// filesystem is IO::directory_filesystem's: an IO that does not classify is not local,
	// so a wrapper must forward it for `refuse` to see the real filesystem.
	enum class nonlocal_policy { warn, refuse };
	nonlocal_policy nonlocal = nonlocal_policy::warn;
};

// Why an index's directory lock may not exclude every other writer: its filesystem is not
// known local, so the lock excludes local writers only, or it could not be locked at all.
struct filesystem_warning {
	std::string filesystem;
	bool local = false;
	bool locked = false;  // false: two writers are not excluded
};

// An index of a layout's segments (layers.md L2); `index` is the search layout's.
template <class Layout>
class basic_index {
public:
	using options = index_options;
	using document = basic_document<Layout>;
	using operation = basic_operation<Layout>;
	using transaction = basic_transaction<Layout>;
	using segment = basic_segment<Layout>;

	// Creates an empty index and commits its first manifest, in an empty directory or over
	// what an interrupted create left (removed first); any other strata file is refused.
	// The owner lock is the directory's own (D2, lock_directory): busy is EWOULDBLOCK, and
	// a refusal writes nothing. The directory's entry in its parent is the host's
	// to make durable; one parent sync can cover many creates (SI-R21).
	static basic_index create(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) {
		basic_index ix = create_deferred(io, opts, uuid);
		ix.commit();  // generation 1 (D3-C)
		return ix;
	}

	// Opens the highest durable manifest (or `to`, committed again as the newest), fails
	// closed when a file it names is missing or differs, then deletes orphans.
	static basic_index open(kronuz::journal::IO& io, const options& opts = {}, std::optional<std::uint64_t> to = std::nullopt) {
		basic_index ix(io);
		ix.lock_directory(opts);
		ix.opts_ = opts;
		ix.seed_identities();
		if (opts.budget) { ix.permit_ = opts.budget->open(); }
		if (opts.storage) { ix.storage_permit_ = opts.storage->open(); }
		ix.start_merges();
		ix.recover(to);
		return ix;
	}

	basic_index(basic_index&&) noexcept = default;
	basic_index& operator=(basic_index&&) noexcept = default;
	~basic_index() { group_.reset(); }

	// Releases the owner lock and drops all state; uncommitted work is lost. Background
	// merges queued are dropped and running ones stopped first. The index is fenced
	// afterwards.
	void close() {
		group_.reset();
		pending_.clear();
		segs_.clear();
		clear_buffer();
		key_slots_.clear();
		buffer_bytes_ = 0;
		permit_ = {};
		storage_permit_ = {};
		lock_.reset();
		fenced_ = true;
	}

	// --- writer ---------------------------------------------------------------------------

	// Applies a transaction at a log position. Positions at or below the applied one are
	// skipped (replay); a rejected transaction changes nothing.
	verdict apply(const transaction& t, files::position at) {
		live();
		// Stage 1 waits for room: the flush happens before this transaction, so a flush
		// failure never loses an accepted transaction's verdict.
		if (needs_flush() || !fits(t)) { refresh(); }
		verdict v;
		if (at != files::position{} && at <= m_.applied) {
			v.result = verdict::skipped;
			return v;
		}
		// Storage admission: the transaction's estimated bytes (and a segment's fixed bytes
		// when it starts a buffer), then any growth of the commit allowance it causes (a
		// first delete in a segment adds a live-docs file), or no_space.
		const std::uint64_t reserve = storage_permit_ ? estimate_bytes(t) + (buffer_size() == 0 ? segment_overhead : 0) : 0;
		if (reserve && !storage_permit_.try_grow(reserve) && !(drain() && storage_permit_.try_grow(reserve))) {
			v.result = verdict::no_space;
			return v;
		}
		undo_.clear();
		v = evaluate(t);
		std::uint64_t allowance = allowance_;
		if (v.result == verdict::accepted && storage_permit_) {
			allowance = commit_allowance();
			if (allowance > allowance_ && !storage_permit_.try_grow(allowance - allowance_)) { v.result = verdict::no_space; }
		}
		if (v.result != verdict::accepted) {
			rollback();
			pending_docs_.clear();
			storage_permit_.shrink(reserve);
		} else {
			reserved_ += reserve;
			allowance_ = std::max(allowance, allowance_);
			invert_pending();
			charge();
			if (t.set_schema) { set_schema(*t.set_schema); }
			++applied_since_commit_;
		}
		undo_.clear();
		if (at != files::position{}) { m_.applied = at; }
		return v;
	}

	// Writes the buffer as a segment (searchable, not yet durable). ENOSPC leaves the
	// buffer in place to retry once space returns; other failures fence.
	void refresh() {
		live();
		guarded([&] { flush(); });
		publish_merges(false, false);
		if (opts_.auto_merge) { maybe_merge(); }
	}

	// Merges the given segments into one (searchable, not yet durable), waiting for it: their
	// live documents in docid order, deleted ones dropped. Nothing is written when none is
	// live; no room for the output throws, after the merge leaves no trace.
	void merge(const std::vector<std::uint64_t>& ids) {
		live();
		publish_merges(true, false);
		schedule(ids);
		publish_merges(true, true);
	}

	// Merges until at most `max_segments` remain and none holds deletes, by the policy's
	// forced merges. With an executor it waits for every background merge, then for its own.
	void force_merge(std::size_t max_segments = 1) {
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_merges(std::move(c), max_segments); }, true, true);
	}

	// Merges away the segments whose deleted fraction passes the policy's
	// force_merge_deletes_pct_allowed, waiting like force_merge().
	void force_merge_deletes() {
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_deletes_merges(std::move(c)); }, true);
	}

	// Runs the policy's merges until it asks for none, or until storage admission has no
	// room for the next; those wait for space. With an executor it hands one round to it,
	// leaving out segments already under merge or damaged, and returns.
	void maybe_merge() {
		try {
			run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_merges(std::move(c)); }, false);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
		}
	}

	// Waits for this index's background merges and publishes them. One that ran out of
	// space is discarded, its sources left for a later merge; any other failure fences.
	void wait_merges() {
		live();
		publish_merges(true, false);
	}
	// Background merges handed to the executor and not yet published.
	std::size_t merges_pending() const noexcept { return pending_.size(); }
	// Whether a background merge found a damaged segment; merges skip it until heal().
	bool needs_heal() const noexcept {
		return std::any_of(segs_.begin(), segs_.end(), [](const auto& s) { return s->damaged; });
	}

	// Makes everything applied durable (design D1), then reclaims what retention allows.
	// ENOSPC from a write before that file's sync leaves the index as it was (the partial
	// file removed); any other failure, and any failed sync, rename, or directory sync,
	// fences the writer.
	void commit() {
		live();
		guarded([&] { flush(); });
		publish_merges(false, false);
		if (opts_.auto_merge) { maybe_merge(); }
		guarded([&] { commit_files(); });
		retain();
		reclaim(opts_.reclaim_slice);
	}

	// Frees queued obsolete files, about `bytes` of them, each truncated from its end in
	// `reclaim_step` steps and then removed. Returns the bytes freed.
	std::uint64_t reclaim(std::uint64_t bytes = ~std::uint64_t(0)) {
		live();
		std::uint64_t freed = 0;
		while (!reclaim_.empty() && freed < bytes) {
			const std::string name = reclaim_.front();
			auto it = files_.find(name);
			std::uint64_t size = it == files_.end() ? 0 : it->second;
			std::unique_ptr<kronuz::journal::File> f;
			std::uint64_t cut = 0;
			try {
				if (size > opts_.reclaim_step) {
					// Truncated bytes count as free once the file's sync makes the truncation durable.
					f = io_->open_existing(name);
					while (size > opts_.reclaim_step && freed + cut < bytes) {
						size -= opts_.reclaim_step;
						f->truncate(size);
						track(name, size);
						truncated_ += opts_.reclaim_step;
						cut += opts_.reclaim_step;
					}
					f->sync();
					truncated_ -= cut;
					freed += cut;
					if (size > opts_.reclaim_step) { break; }
				}
				remove_file(name);
			} catch (const std::system_error& e) {
				// A failed round releases what it counted as truncated; the file's size is
				// whatever the truncations left.
				if (cut) {
					truncated_ -= cut;
					try {
						track(name, f->size());
					} catch (const std::system_error&) {
					}
				}
				// The commits stand; the file stays queued for a later round, unless it is gone.
				if (e.code().value() != ENOENT) { break; }
				forget_file(name);
			}
			freed += size;
			queued_.erase(name);
			reclaim_.pop_front();
		}
		settle_storage();
		return freed;
	}
	std::size_t reclaim_pending() const noexcept { return reclaim_.size(); }

	// The kept manifest (a pin or a retained predecessor) whose read failed in the last
	// retention round, which then removed nothing; retention resumes once it reads or the
	// pin is released. A host reports it: a damaged pinned checkpoint stops reclamation.
	std::optional<std::uint64_t> retention_blocked() const noexcept { return retention_blocked_; }

	// Set when the directory's lock may not exclude every other writer: under `warn`, a
	// filesystem not known local, its classification failed, or its lock unsupported; under
	// `refuse`, a FUSE mount (never refused by its type). A host reports it.
	const std::optional<strata::filesystem_warning>& filesystem_warning() const noexcept { return fs_warning_; }

	// Reads that failed or failed their checks once and passed when read again.
	std::uint64_t media_retries() const noexcept { return media_retries_; }

	// After a search meets corrupt_segment: verifies every committed segment in memory, and
	// reads one that fails again from its file, verified whole, in its place. Returns the
	// ids whose file is damaged too, for the host to fence and repair from a checkpoint or
	// a replica. Segments a background merge found damaged are among those checked, and
	// merges take them again once they pass. Like a merge, it invalidates segment views.
	std::vector<std::uint64_t> heal() {
		live();
		std::vector<std::uint64_t> damaged;
		for (auto& s : segs_) {
			if (s->unsynced) {  // built in memory, never read from disk
				if (s->damaged) { damaged.push_back(s->ref.id); }
				continue;
			}
			try {
				s->seg.verify_all();
				s->damaged = false;
				continue;
			} catch (const corrupt_segment&) {
			}
			try {
				auto fresh = read_checked([&] { return load_segment(*io_, m_, s->ref, true); }, nullptr);
				fresh->deleted = std::move(s->deleted);
				fresh->dead = s->dead;
				fresh->deletes_dirty = s->deletes_dirty;
				fresh->ref = s->ref;
				s = std::move(fresh);
				++media_retries_;
			} catch (const corrupt_index&) {
				damaged.push_back(s->ref.id);
			} catch (const corrupt_segment&) {
				damaged.push_back(s->ref.id);
			} catch (const std::system_error&) {
				damaged.push_back(s->ref.id);
			}
		}
		return damaged;
	}

	// --- reading --------------------------------------------------------------------------

	struct segment_view {
		std::uint64_t id;
		const segment* seg;
		const std::vector<std::uint64_t>* deleted;  // bit per ordinal; null when none is deleted
		std::uint64_t dead = 0;                      // bits set in deleted
		bool live(ordinal_t ord) const { return !deleted || !(((*deleted)[ord >> 6] >> (ord & 63)) & 1); }
	};
	// Null bitmaps for segments without deletes, so searches skip the live check without
	// scanning the bitmap.
	std::vector<segment_view> segments() const {
		std::vector<segment_view> out;
		for (const auto& s : segs_) { out.push_back({s->ref.id, &s->seg, s->dead ? &s->deleted : nullptr, s->dead}); }
		return out;
	}
	const files::manifest& manifest() const noexcept { return m_; }
	std::uint64_t last_docid() const noexcept { return m_.last_docid; }
	std::uint64_t schema_version() const noexcept { return m_.schema_version; }
	const std::map<std::string, std::string>& metadata() const noexcept { return meta_; }
	std::size_t buffered_count() const noexcept { return buffer_size(); }
	// The replicated log's term that produces this replica's segments from now on (F6,
	// F13-R41; durability D8): the host sets it when its leader's term changes; 0 standalone.
	void set_producer_term(std::uint64_t term) noexcept { producer_term_ = term; }

	// --- deterministic core -------------------------------------------------------------

	// The logical-content digest (design D12; format 13's split, L8 and F21): the sum modulo
	// 2^64 of xxHash64 over records. The core's D record per live document holds its docid,
	// version, and key; each configured plugin adds its own records; the core adds
	// `lastdocid`, the schema version, and each metadata entry. Equal content gives equal
	// digests whatever its segment layout. It checks at most one live copy per docid and per
	// key, every docid at or below `lastdocid`, and throws std::logic_error otherwise.
	std::uint64_t digest() const {
		std::uint64_t sum = 0;
		auto emit = [&](std::string_view rec) { sum += xxh64::hash(rec); };
		std::set<std::uint64_t> docids;
		std::set<std::string> keys;
		auto document_record = [&](std::uint64_t docid, std::uint64_t version, std::string_view key) {
			if (docid == 0 || docid > m_.last_docid || !docids.insert(docid).second) { throw std::logic_error("strata: digest: docid invariant"); }
			if (!key.empty() && !keys.insert(std::string(key)).second) { throw std::logic_error("strata: digest: key invariant"); }
			digest_record rec('D');
			rec.u64(docid);
			rec.u64(version);
			rec.bytes(key);
			emit(rec.data());
		};
		const format::structure_bits configured = configured_structures();
		for (const auto& sp : segs_) {
			const auto& s = *sp;
			auto live = [&](ordinal_t o) { return !((s.deleted[o >> 6] >> (o & 63)) & 1); };
			const core_segment& core = s.seg.core();
			std::vector<std::string> key_of(core.doc_count());  // walked through the keys reader's checks (L3, MR2)
			s.seg.template get<keys_plugin>().each_key([&](std::string_view key, ordinal_t o) { key_of[o] = key; });
			for (ordinal_t o = 0; o < core.doc_count(); ++o) {
				if (live(o)) { document_record(core.docid(o), s.seg.template get<keys_plugin>().version(o), key_of[o]); }
			}
			if (configured & inverted_plugin::structure) { inverted_plugin::records(s.seg.template get<inverted_plugin>(), core, live, emit); }
			if (configured & stored_plugin::structure) { stored_plugin::records(s.seg.template get<stored_plugin>(), core, live, emit); }
		}
		if (active_) {  // the inverter's buffer: the same records as its documents' (F21)
			const auto& a = *active_;
			for (ordinal_t o = 0; o < a.buf.doc_count(); ++o) {
				if (a.is_dead(o)) { continue; }
				document_record(a.buf.docid(o), a.buf.version(o), a.buf.key(o));
				if (configured & inverted_plugin::structure) { inverted_plugin::length_record(a.buf.docid(o), a.buf.length(o), emit); }
				if (configured & stored_plugin::structure) { stored_plugin::record(a.buf.docid(o), a.buf.stored(o), emit); }
			}
			if (configured & inverted_plugin::structure) {
				a.buf.scan([&](std::string_view term, ordinal_t o, std::uint32_t wdf, std::span<const std::uint32_t> positions) {
					if (!a.is_dead(o)) { inverted_plugin::posting_record(a.buf.docid(o), term, wdf, positions, emit); }
				});
			}
		}
		for (const auto& b : buffer_) {
			if (b.dead) { continue; }
			document_record(b.docid, b.version, b.doc.key);
			if (configured & inverted_plugin::structure) { inverted_plugin::records(b.docid, b.doc, emit); }
			if (configured & stored_plugin::structure) { stored_plugin::records(b.docid, b.doc, emit); }
		}
		digest_record last('L');
		last.u64(m_.last_docid);
		emit(last.data());
		digest_record schema('S');
		schema.u64(m_.schema_version);
		emit(schema.data());
		for (const auto& [k, v] : meta_) {
			digest_record m('M');
			m.bytes(k);
			m.bytes(v);
			emit(m.data());
		}
		return sum;
	}

	// The structures this index is configured with, from the manifest (F20, F13-R28). Flushes
	// and merges write them; the digest records them (F21).
	format::structure_bits configured_structures() const noexcept { return m_.configured; }

	// Checks every segment whole (check_segment) and the digest's invariants: one live copy
	// per docid and per key, every docid at or below lastdocid. Throws at the first problem.
	check_report check() const {
		check_report r;
		for (const auto& s : segs_) { check_segment(s->seg, &s->deleted, r, m_.key_prefix); }
		(void)digest();
		return r;
	}

	// Commits and pins the manifest; the generation names the checkpoint (design D4).
	std::uint64_t checkpoint() {
		live();
		commit();
		const std::uint64_t generation = m_.generation;
		// The pin lives in the next manifest; committing again makes it durable.
		pending_pin_ = generation;
		commit();
		return generation;
	}

	// Unpins a checkpoint; retention reclaims it once nothing else keeps it.
	void release(std::uint64_t generation) {
		live();
		pending_unpin_ = generation;
		commit();
	}

	// Replaces this index's content with the checkpoint `generation` of the index at
	// `source` (design D5). The checkpoint is read and checked whole, as an open checks its
	// head, before anything here changes: a bad checkpoint or a failure reading it throws
	// and leaves this index as it was, unfenced. Storage admission reserves the copy
	// (ENOSPC otherwise). The files are written under fresh local names and commit as a
	// manifest above every existing generation; the buffer, pending deletes, and version
	// map are discarded, and the checkpoint's last docid, schema version, applied position,
	// and UUID are adopted. A crash at any point opens the old content or the installed one.
	void install(kronuz::journal::IO& source, std::uint64_t generation) {
		live();
		files::manifest theirs;
		{
			auto f = source.open_existing(index_detail::manifest_name(generation));
			theirs = files::decode_manifest_for(index_detail::read_all(*f), readable());
		}
		if (theirs.generation != generation) { throw corrupt_index("install: manifest generation differs from its name"); }
		check_readable(theirs);
		auto got = load_files(source, theirs, true, &media_retries_);
		// The copy, re-encoded live docs (16 bytes for their new names' fields), and the
		// commit's metadata file and manifest.
		std::uint64_t need = theirs.meta.size + 96 + files::encode(theirs).size() + 160 * theirs.segments.size();
		for (const auto& s : got.segments) { need += s->ref.size + (s->ref.live.generation ? s->ref.live.size + 16 : 0); }
		if (storage_permit_ && !storage_permit_.try_grow(need) && !(drain() && storage_permit_.try_grow(need))) {
			throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for an install");
		}
		hold held{this};
		if (storage_permit_) { held.add(need); }
		abandon_merges();  // their sources leave with the old content
		guarded([&] {
			for (auto& s : got.segments) {
				files::segment_ref mine = s->ref;
				mine.id = m_.next_segment++;
				mine.live = {};
				write_new(index_detail::segment_name(mine.id), s->bytes());
				if (s->ref.live.generation) {
					files::live_docs l;
					l.segment = mine.id;
					l.generation = m_.next_live++;
					l.doc_count = s->seg.core().doc_count();
					for (ordinal_t o = 0; o < s->seg.core().doc_count(); ++o) {
						if ((s->deleted[o >> 6] >> (o & 63)) & 1) { l.deleted.push_back(o); }
					}
					l.live_count = s->ref.live_count;
					l.live_length = s->ref.live_length;
					const std::string lb = files::encode(l);
					write_new(index_detail::live_name(l.segment, l.generation), lb);
					mine.live = {l.generation, lb.size(), files::trailing_crc(lb)};
				}
				s->ref = mine;
			}
			segs_ = std::move(got.segments);
			clear_buffer();
			key_slots_.clear();
			buffer_bytes_ = 0;
			reserved_ = 0;  // the discarded buffer's reservations
			if (permit_) { permit_.settle(); }
			meta_ = std::move(got.meta);
			meta_dirty_ = true;
			m_.uuid = theirs.uuid;
			m_.last_docid = theirs.last_docid;
			m_.schema_version = theirs.schema_version;
			m_.applied = theirs.applied;
			m_.key_prefix = theirs.key_prefix;
			m_.configured = theirs.configured;
			m_.pinned.clear();
			applied_since_commit_ = 1;
			commit_files();
		});
		retain();
	}

	// The live document's (docid, version) for a key, if any.
	std::optional<std::pair<std::uint64_t, std::uint64_t>> find_key(std::string_view key) const {
		if (auto e = lookup_key(key)) { return std::pair{e->docid, e->version}; }
		return std::nullopt;
	}
	bool has_docid(std::uint64_t docid) const { return locate_docid(docid).found; }

private:
	friend struct index_test_access;  // tests reach invariants no public call can break

	// Runs the merges `find` asks for, asking again after each round, until it asks none.
	// With an executor they run there, on segments not under merge or damaged: one round
	// without `wait`, otherwise every round, each waited for and published.
	template <class Find>
	void run_merges(Find find, bool wait, bool forced = false) {
		live();
		if (wait) { publish_merges(true, false); }
		for (;;) {
			std::vector<merge_policy::candidate> c;
			for (const auto& s : segs_) {
				if (merging(*s) || s->damaged) { continue; }
				std::uint64_t deleted = 0;
				for (auto w : s->deleted) { deleted += std::uint64_t(std::popcount(w)); }
				c.push_back({s->ref.id, s->ref.size, s->seg.core().doc_count(), deleted});
			}
			const auto merges = find(std::move(c));
			if (merges.empty()) { return; }
			for (const auto& ids : merges) { schedule(ids, forced); }
			if (!wait && !own_exec_) { return; }  // the index's own executor runs them now
			publish_merges(true, true);
		}
	}

	explicit basic_index(kronuz::journal::IO& io) : io_(&io) {}

	// Merges run on the host's executor, or on the index's own inline one, synchronously
	// on the writer's thread, when it passes none: one merge implementation either way.
	void start_merges() {
		exec_ = opts_.background;
		if (!exec_) {
			own_exec_ = std::make_unique<inline_executor>();
			exec_ = own_exec_.get();
		}
		group_ = exec_->open();
	}

	// --- memory -----------------------------------------------------------------------------

	static std::uint64_t doc_bytes(const document& d) { return sizeof(document) + 96 + d.part_bytes(); }
	std::uint64_t map_bytes() const { return key_slots_.bytes() + buffer_docids_.bytes(); }

	// Brings the permit to what the buffer and version map hold now.
	// The buffer's memory: the active writer's allocations, or the documents' estimate.
	std::uint64_t buffered_bytes() const noexcept { return active_ ? active_->held.held() : buffer_bytes_; }
	void charge() {
		if (!permit_) { return; }
		const std::uint64_t want = buffered_bytes() + map_bytes();
		if (want > permit_.bytes()) {
			permit_.grow(want - permit_.bytes());
		} else {
			permit_.shrink(permit_.bytes() - want);
		}
	}
	bool needs_flush() const {
		if (buffer_size() == 0) { return false; }
		if (buffered_bytes() >= opts_.buffer_share || map_bytes() >= opts_.buffer_share / 4) { return true; }
		return opts_.budget && opts_.budget->pressure();
	}

	// --- in-memory state ------------------------------------------------------------------

	struct open_segment {
		files::segment_ref ref;
		std::shared_ptr<const kronuz::journal::Mapping> mapping;  // the file, mapped (ARCHITECTURE, I/O)
		std::string owned;                                         // or its bytes, when the backend cannot map
		segment seg;
		std::vector<std::uint64_t> deleted;  // bit per ordinal
		std::uint64_t dead = 0;              // bits set in deleted
		bool deletes_dirty = false;          // deletes since its live-docs generation
		bool damaged = false;                // a background merge found it corrupt; heal() clears it
		std::unique_ptr<kronuz::journal::File> unsynced;  // flushed, awaiting the commit's sync
		open_segment(files::segment_ref r, std::string b) : ref(r), owned(std::move(b)), seg(owned), deleted((seg.core().doc_count() + 63) / 64, 0) {}
		open_segment(files::segment_ref r, std::shared_ptr<const kronuz::journal::Mapping> m)
			: ref(r), mapping(std::move(m)), seg(mapping->bytes()), deleted((seg.core().doc_count() + 63) / 64, 0) {}
		std::string_view bytes() const noexcept { return mapping ? mapping->bytes() : std::string_view(owned); }
	};
	// A background merge. The writer sets everything above `plan` before handing it to the
	// executor; the merge grows `storage` and `reserved`, sets the rest, then `done`, and the
	// writer touches none of it again before `done`.
	struct merge_job {
		std::uint64_t id = 0;  // the output segment's
		std::array<std::uint8_t, 16> header{};
		std::vector<std::shared_ptr<open_segment>> sources;
		std::vector<std::vector<std::uint64_t>> deleted;  // the sources' deletes the merge reads
		bool reorder = false;                             // a reordering merge (D4)
		std::uint64_t reserved = 0;                       // on `storage`: the output's estimate, then what it wrote
		byte_budget::permit storage;
		flush_charge memory;  // the merge's working memory, then its output's, until published
		merge_plan plan;
		std::unique_ptr<open_segment> output;  // none when no document was live
		std::exception_ptr error;
		std::atomic<bool> done{false};
	};
	struct buffered {
		std::uint64_t docid, version;
		document doc;  // its key is doc.key
		bool dead = false;
	};
	// Documents inverted on arrival, and what the flush needs to write them in docid order.
	struct active_buffer {
		flush_charge held;  // what the buffer allocated: charge() draws it on permit_ (D17)
		detail::tuple_buffer buf;
		std::vector<std::uint64_t> dead;  // bit per ordinal: replaced or deleted while buffered
		std::uint64_t dead_count = 0, prev_docid = 0;
		bool ordered = true;  // docids increase with ordinals
		active_buffer() {
			buf.memory = &held;
			held.grow(sizeof(active_buffer));  // the object too (C11)
		}
		bool is_dead(ordinal_t o) const { return o >> 6 < dead.size() && (dead[o >> 6] >> (o & 63) & 1); }
		void set_dead(ordinal_t o, bool on) {
			if (o >> 6 >= dead.size()) { dead.resize((o >> 6) + 1); }
			const std::uint64_t bit = std::uint64_t(1) << (o & 63);
			if (bool(dead[o >> 6] & bit) == on) { return; }
			dead[o >> 6] ^= bit;
			on ? ++dead_count : --dead_count;
		}
	};
	struct pending_doc {
		std::uint64_t docid, version;
		std::string_view key;  // in the transaction
		const document* doc;
		bool dead = false;
	};
	// Where a docid's live copy is.
	struct location {
		bool found = false;
		std::size_t segment = 0;  // index into segs_, or buffer_ when in_buffer
		ordinal_t ord = 0;
		bool in_buffer = false;
	};
	struct key_entry {
		std::uint64_t docid = 0, version = 0;
	};

	void live() const {
		if (fenced_) { throw index_fenced("strata: the writer is fenced; reopen the index"); }
	}

	// Space errors before a sync stay recoverable (write_unsynced throws space_error);
	// every other I/O error fences.
	struct space_error : std::system_error {
		using std::system_error::system_error;
	};
	template <class F>
	void guarded(F&& f) {
		try {
			f();
		} catch (const space_error& e) {
			throw std::system_error(e.code(), e.what());
		} catch (const std::system_error& e) {
			fenced_ = true;
			throw index_fenced(std::string("strata: durability failure: ") + e.what());
		}
	}

	// A segment just written to f: mapped when the backend maps and the mapping holds what
	// was written (size and footer), so its bytes leave memory; else kept.
	static std::unique_ptr<open_segment> written_segment(kronuz::journal::File& f, const files::segment_ref& r, std::string bytes) {
		if (auto m = f.map(); m && m->bytes().size() == r.size && (format::read_version(m->bytes()), format::decode_footer(m->bytes()).toc_crc == r.footer_crc)) {
			return std::make_unique<open_segment>(r, std::move(m));
		}
		return std::make_unique<open_segment>(r, std::move(bytes));
	}

	// Creates a file and writes it whole. ENOSPC removes the partial file and throws
	// space_error; the caller has changed nothing yet.
	std::unique_ptr<kronuz::journal::File> write_unsynced(const std::string& name, std::string_view bytes) {
		auto f = io_->create_exclusive(name);
		try {
			index_detail::write_all(*f, bytes);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
			f.reset();
			io_->remove(name);
			sync_dir();  // the partial file's space returns once its removal is durable
			throw space_error(e.code(), e.what());
		}
		track(name, bytes.size());
		return f;
	}

	// --- background merges (D11, D18) ------------------------------------------------------

	bool merging(const open_segment& s) const {
		for (const auto& job : pending_) {
			for (const auto& src : job->sources) {
				if (src.get() == &s) { return true; }
			}
		}
		return false;
	}

	// Hands the merge of `ids` to the executor. It shares the sources and reads a copy of
	// their deletes so far; storage admission reserves the output's estimate as merge()
	// does, on a permit of the merge's own.
	// `forced`: a merge force_merge() asked for.
	void schedule(const std::vector<std::uint64_t>& ids, bool forced = false) {
		auto job = std::make_shared<merge_job>();
		job->reserved = segment_overhead;
		std::uint64_t live_docs = 0;
		for (const auto& s : segs_) {
			if (std::find(ids.begin(), ids.end(), s->ref.id) == ids.end() || merging(*s) || s->damaged) { continue; }
			const std::uint64_t docs = s->seg.core().doc_count();
			job->sources.push_back(s);
			job->deleted.push_back(s->deleted);
			job->reserved += s->ref.size + 18 * docs + 16 * (docs / format::block + 1);
			live_docs += docs - s->dead;
		}
		using mode = index_options::reorder_mode;
		job->reorder = (opts_.reordering == mode::force_merge && forced) || (opts_.reordering == mode::merges && (forced || live_docs >= opts_.reorder_min_docs));
		if (job->sources.empty()) { return; }
		if (storage_permit_) {
			job->storage = opts_.storage->open();
			if (!job->storage.try_grow(job->reserved) && !(drain() && job->storage.try_grow(job->reserved))) {
				throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for a merge");
			}
		}
		job->id = m_.next_segment++;
		job->header = new_identity();
		pending_.reserve(pending_.size() + 1);
		const merge_env ctx{io_, exec_, opts_.rate, opts_.budget, opts_.merge_memory_wait, m_.key_prefix, opts_.reorder_settings, configured_structures()};
		group_->post([job, ctx](const task::cancel_token& stop) { run_merge(*job, ctx, stop); });
		pending_.push_back(std::move(job));
	}

	// What a merge uses besides its job; the index's, never the index.
	struct merge_env {
		kronuz::journal::IO* io;
		executor* exec;
		rate_limiter* rate;
		byte_budget* budget;
		std::uint64_t memory_wait;
		std::string key_prefix;
		strata::reorder::settings reorder;
		format::structure_bits structures;  // the index's configured bits (F4)
	};

	// A merge's writes to one file, at increasing offsets: before each chunk an executor
	// checkpoint and the rate limiter's admission, then a draw on the job's storage
	// reservation past what it holds.
	class merge_file {
	public:
		merge_file(kronuz::journal::File& f, const merge_env& ctx, merge_job& job, const task::cancel_token& stop) : f_(f), ctx_(ctx), job_(job), stop_(stop) {}
		void write(std::string_view bytes, std::uint64_t& used) {
			const std::size_t chunk = ctx_.rate ? ctx_.rate->chunk() : std::size_t(1) << 20;
			while (!bytes.empty()) {
				const std::size_t n = std::min(bytes.size(), chunk);
				if (!ctx_.exec->checkpoint(stop_) || (ctx_.rate && !ctx_.rate->admit(*ctx_.exec, n, stop_))) { throw task::cancelled{}; }
				used += n;
				if (job_.storage && used > job_.reserved) {
					if (!job_.storage.try_grow(used - job_.reserved)) {
						throw space_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for a merge's output");
					}
					job_.reserved = used;
				}
				for (std::size_t done = 0; done < n;) {
					const std::size_t w = f_.write_at(at_ + done, bytes.substr(done, n - done));
					if (w == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
					done += w;
				}
				at_ += n;
				bytes.remove_prefix(n);
			}
		}
		std::uint64_t size() const noexcept { return at_; }

	private:
		kronuz::journal::File& f_;
		const merge_env& ctx_;
		merge_job& job_;
		const task::cancel_token& stop_;
		std::uint64_t at_ = 0;
	};

	// The output segment, streamed to its file a chunk at a time.
	class merge_output final : public output {
	public:
		merge_output(merge_file& f, std::uint64_t& used, std::size_t chunk) : f_(f), used_(used), chunk_(chunk) {}
		using output::write;
		void write(const char* data, std::size_t size) override {
			buf_.append(data, size);
			if (buf_.size() >= chunk_) { finish(); }
		}
		void finish() {
			f_.write(buf_, used_);
			buf_.clear();
		}

	private:
		merge_file& f_;
		std::uint64_t& used_;
		std::size_t chunk_;
		std::string buf_;
	};

	// The positions section, spilled to `seg-<id>.spill` until the postings end, then
	// copied into the output and removed. A spill left by a crash is an orphan open removes.
	class merge_spill final : public spill {
	public:
		merge_spill(const merge_env& ctx, merge_job& job, std::uint64_t& used, const task::cancel_token& stop, std::size_t chunk)
			: ctx_(ctx), job_(job), used_(used), stop_(stop), chunk_(chunk), name_(index_detail::spill_name(job.id)) {}
		merge_spill(const merge_spill&) = delete;
		merge_spill& operator=(const merge_spill&) = delete;
		~merge_spill() override {
			if (!f_) { return; }
			f_.reset();
			try {
				ctx_.io->remove(name_);
			} catch (const std::system_error&) {
			}
		}
		using output::write;
		void write(const char* data, std::size_t size) override {
			buf_.append(data, size);
			if (buf_.size() >= chunk_) { drain(); }
		}
		void copy_to(output& out) override {
			drain();
			if (!f_) { return; }
			const auto in = ctx_.io->open_existing(name_);
			std::string block;
			for (std::uint64_t at = 0; at < file_->size();) {
				if (stop_.requested()) { throw task::cancelled{}; }
				block.resize(std::min<std::uint64_t>(std::uint64_t(1) << 20, file_->size() - at));
				const std::size_t n = in->read_at(at, std::span<char>(block.data(), block.size()));
				if (n == 0) { throw std::system_error(EIO, std::generic_category(), "strata: short read of a merge's spill"); }
				out.write(block.data(), n);
				at += n;
			}
			const std::uint64_t spilled = file_->size();
			file_.reset();
			f_.reset();
			ctx_.io->remove(name_);
			used_ -= std::min(used_, spilled);
		}

	private:
		void drain() {
			if (buf_.empty()) { return; }
			if (!f_) {
				f_ = ctx_.io->create_exclusive(name_);
				file_ = std::make_unique<merge_file>(*f_, ctx_, job_, stop_);
			}
			file_->write(buf_, used_);
			buf_.clear();
		}
		const merge_env& ctx_;
		merge_job& job_;
		std::uint64_t& used_;
		const task::cancel_token& stop_;
		std::size_t chunk_;
		std::string name_, buf_;
		std::unique_ptr<kronuz::journal::File> f_;
		std::unique_ptr<merge_file> file_;
	};

	// A reordering merge's temporary file: its forward index (`seg-<id>.fwd`), mapped back
	// for bisection, or its stored records (`seg-<id>.rec`), mapped back as the output is
	// written. Written through the merge's file writes (checkpoints, rate limit, storage)
	// and removed after use; one left by a crash is an orphan open removes.
	class merge_scratch final : public strata::scratch {
	public:
		merge_scratch(const merge_env& ctx, merge_job& job, std::uint64_t& used, const task::cancel_token& stop, std::size_t chunk, std::string name)
			: ctx_(ctx), used_(used), name_(std::move(name)), f_(ctx.io->create_exclusive(name_)), file_(*f_, ctx, job, stop), out_(file_, used, chunk) {}
		merge_scratch(const merge_scratch&) = delete;
		merge_scratch& operator=(const merge_scratch&) = delete;
		~merge_scratch() override {
			mapped_.reset();
			f_.reset();
			used_ -= std::min(used_, file_.size());
			try {
				ctx_.io->remove(name_);
			} catch (const std::system_error&) {
			}
		}
		output& out() override { return out_; }
		std::string_view bytes() override {
			out_.finish();
			if ((mapped_ = f_->map())) { return mapped_->bytes(); }
			owned_ = index_detail::read_all(*ctx_.io->open_existing(name_));
			return owned_;
		}

	private:
		const merge_env& ctx_;
		std::uint64_t& used_;
		std::string name_;
		std::unique_ptr<kronuz::journal::File> f_;
		merge_file file_;
		merge_output out_;
		std::shared_ptr<const kronuz::journal::Mapping> mapped_;
		std::string owned_;
	};

	// On the executor: reads the sources' segments, which never change, and the job alone.
	// It waits for room on the indexing budget, charges its working memory there, streams
	// the output to its file unsynced (as a flush leaves it) under the rate limit with
	// positions spilled to a file, then reads it back as the segment. A failed or stopped
	// build removes its files.
	static void run_merge(merge_job& job, const merge_env& ctx, const task::cancel_token& stop) {
		try {
			if (stop.requested()) { throw task::cancelled{}; }
			std::uint64_t live = 0, estimate = 0;
			for (std::size_t k = 0; k < job.sources.size(); ++k) {
				std::uint64_t dead = 0;
				for (auto w : job.deleted[k]) { dead += std::uint64_t(std::popcount(w)); }
				live += job.sources[k]->seg.core().doc_count() - dead;
				estimate += job.sources[k]->ref.size;
			}
			if (live == 0) {  // nothing to write: publication drops the sources
				job.done.store(true, std::memory_order_release);
				return;
			}
			if (ctx.budget) {
				const std::uint64_t until = ctx.exec->now() + ctx.memory_wait;
				for (std::uint64_t now = ctx.exec->now(); ctx.budget->pressure(estimate) && now < until; now = ctx.exec->now()) {
					if (!ctx.exec->sleep_until(std::min(until, now + 10'000'000), stop)) { throw task::cancelled{}; }
				}
			}
			job.memory = flush_charge(ctx.budget);
			if (!ctx.exec->checkpoint(stop)) { throw task::cancelled{}; }
			std::vector<basic_merge_source<Layout>> sources;
			for (std::size_t k = 0; k < job.sources.size(); ++k) { sources.push_back({&job.sources[k]->seg, &job.deleted[k]}); }
			const std::string name = index_detail::segment_name(job.id);
			auto file = ctx.io->create_exclusive(name);
			std::string bytes;
			std::shared_ptr<const kronuz::journal::Mapping> mapped;  // the output, mapped; else read into bytes
			try {
				const std::size_t chunk = ctx.rate ? ctx.rate->chunk() : std::size_t(1) << 20;
				std::uint64_t used = 0;
				merge_file out(*file, ctx, job, stop);
				merge_output sink(out, used, chunk);
				std::vector<merge_ref> order;
				if constexpr (order_source<Layout>::exists) {
					if (job.reorder) {
						merge_scratch forward(ctx, job, used, stop, chunk, index_detail::forward_name(job.id));
						order = order_source<Layout>::order(sources, forward, ctx.reorder, &stop, &job.memory);
					}
				}
				{
					std::optional<merge_scratch> records;
					if (!order.empty() || (sources.size() == 1 && !sources[0].seg->core().docid_ordered())) {
						records.emplace(ctx, job, used, stop, chunk, index_detail::records_name(job.id));
					}
					merge_spill positions(ctx, job, used, stop, chunk);
					job.plan = detail::merge_layout(std::span<const basic_merge_source<Layout>>(sources), job.header, sink, positions,
						{.cancel = &stop, .memory = &job.memory, .order = order, .records = records ? &*records : nullptr, .key_prefix = ctx.key_prefix, .structures = ctx.structures});
				}
				sink.finish();
				job.memory.shrink(job.memory.held());  // the build's buffers are gone
				if (!(mapped = file->map())) {
					bytes = index_detail::read_all(*ctx.io->open_existing(name));
					job.memory.grow(bytes.capacity());
				}
				if ((mapped ? mapped->bytes().size() : bytes.size()) != out.size()) {
					throw std::system_error(EIO, std::generic_category(), "strata: a merge's output reads back short");
				}
			} catch (...) {
				file.reset();
				ctx.io->remove(name);
				ctx.io->sync_directory();  // the partial file's space returns once its removal is durable
				throw;
			}
			const std::string_view view = mapped ? mapped->bytes() : std::string_view(bytes);
			files::segment_ref ref;
			ref.id = job.id;
			ref.identity = job.header;
			ref.size = view.size();
			ref.footer_crc = format::decode_footer(view).toc_crc;
			ref.doc_count = job.plan.doc_count;
			job.output = mapped ? std::make_unique<open_segment>(ref, std::move(mapped)) : std::make_unique<open_segment>(ref, std::move(bytes));
			job.output->unsynced = std::move(file);
		} catch (const std::system_error& e) {
			job.error = e.code().value() == ENOSPC ? std::make_exception_ptr(space_error(e.code(), e.what())) : std::current_exception();
		} catch (...) {
			job.error = std::current_exception();
		}
		job.done.store(true, std::memory_order_release);
	}

	// Publishes the background merges that finished, after waiting for all of them with
	// `wait`. One that ran out of space, or that the executor stopped, is dropped, its
	// sources left for a later merge (and with `space_throws`, ENOSPC is thrown once the
	// rest are published). A corrupt source is marked for heal() and skipped until then.
	// Any other failure fences: an I/O error, or a broken invariant.
	void publish_merges(bool wait, bool space_throws) {
		if (!group_ || pending_.empty()) { return; }
		if (wait) { group_->wait(); }
		std::optional<std::string> space;  // the first refusal's own message: admission's or the disk's
		for (std::size_t i = 0; i < pending_.size();) {
			if (!pending_[i]->done.load(std::memory_order_acquire)) {
				++i;
				continue;
			}
			const auto job = std::move(pending_[i]);
			pending_.erase(pending_.begin() + std::ptrdiff_t(i));
			if (job->error) {
				job->storage.settle();
				job->memory.settle();
				try {
					std::rethrow_exception(job->error);
				} catch (const space_error& e) {
					if (!space) { space = e.what(); }
					continue;
				} catch (const task::cancelled&) {
					continue;
				} catch (const corrupt_segment& e) {
					if (mark_damaged(*job)) { continue; }
					fence(std::string("strata: a merge's output is corrupt: ") + e.what());
				} catch (const std::system_error& e) {
					fence(std::string("strata: durability failure: ") + e.what());
				} catch (const std::exception& e) {
					fence(std::string("strata: merge failure: ") + e.what());
				}
			}
			install_merge(*job);
		}
		settle_storage();
		if (space && space_throws) { throw space_error(std::make_error_code(std::errc::no_space_on_device), *space); }
	}

	[[noreturn]] void fence(const std::string& why) {
		fenced_ = true;
		throw index_fenced(why);
	}

	// Marks the merge's sources that fail verification; false when none does.
	bool mark_damaged(const merge_job& job) {
		bool any = false;
		for (const auto& src : job.sources) {
			if (std::find(segs_.begin(), segs_.end(), src) == segs_.end()) { continue; }
			try {
				src->seg.verify_all();
			} catch (const corrupt_segment&) {
				src->damaged = true;
				any = true;
			}
		}
		return any;
	}

	// Publishes a finished merge, on the writer's thread. When its sources are all still
	// here, the output replaces them, and each delete made since the merge read them carries
	// over through its ordinal map, once (D11); otherwise the output's file is reclaimed. A
	// delete that maps to no output document breaks the invariant and fences, changing
	// nothing.
	void install_merge(merge_job& job) {
		std::vector<std::size_t> at;
		for (const auto& src : job.sources) {
			const auto it = std::find(segs_.begin(), segs_.end(), src);
			if (it == segs_.end()) { break; }
			at.push_back(std::size_t(it - segs_.begin()));
		}
		const std::string name = index_detail::segment_name(job.id);
		auto reclaim_output = [&] {
			if (!job.output) { return; }
			track(name, job.output->ref.size);
			job.output.reset();
			reclaim_.push_back(name);
			queued_.insert(name);
		};
		std::vector<ordinal_t> carried;
		if (at.size() == job.sources.size() && job.output) {
			for (std::size_t k = 0; k < at.size(); ++k) {
				const auto& now = segs_[at[k]]->deleted;
				const auto& then = job.deleted[k];
				const auto& to = job.plan.ordinals[k];
				for (std::size_t w = 0; w < now.size(); ++w) {
					for (std::uint64_t fresh = now[w] & ~then[w]; fresh; fresh &= fresh - 1) {
						const ordinal_t o = to[w * 64 + std::size_t(std::countr_zero(fresh))];
						if (o == no_more_docs) {
							reclaim_output();
							settle_storage();
							job.storage.settle();
							job.memory.settle();
							fence("strata: merge publication: a delete names a document the merge dropped");
						}
						carried.push_back(o);
					}
				}
			}
		}
		if (at.size() != job.sources.size()) {
			reclaim_output();
		} else {
			if (job.output) {
				auto& out = *job.output;
				for (ordinal_t o : carried) { out.deleted[o >> 6] |= std::uint64_t(1) << (o & 63); }
				out.dead = carried.size();
				out.deletes_dirty = !carried.empty();
			}
			std::sort(at.begin(), at.end());
			for (std::size_t k = at.size(); k-- > 0;) { segs_.erase(segs_.begin() + std::ptrdiff_t(at[k])); }
			if (job.output) {
				// Its id was taken at scheduling, below any flushed since: it goes in id order,
				// which the manifest requires.
				track(name, job.output->ref.size);
				const auto at_id = std::lower_bound(segs_.begin(), segs_.end(), job.id, [](const auto& s, std::uint64_t id) { return s->ref.id < id; });
				segs_.emplace(at_id, std::move(job.output));
			}
		}
		settle_storage();
		job.storage.settle();
		job.memory.settle();  // published: the segment's bytes are the index's now (D17)
	}

	// Ends every background merge unpublished: queued ones are dropped, running ones
	// stopped, and finished outputs' files reclaimed.
	void abandon_merges() {
		if (!group_) { return; }
		group_->cancel();
		for (const auto& job : pending_) {
			if (job->done.load(std::memory_order_acquire) && job->output) {
				const std::string name = index_detail::segment_name(job->id);
				track(name, job->output->ref.size);
				job->output.reset();
				reclaim_.push_back(name);
				queued_.insert(name);
			}
			job->storage.settle();
			job->memory.settle();
		}
		pending_.clear();
		settle_storage();
	}

	// --- storage accounting (T5) -------------------------------------------------------------

	// Bytes a merge or install reserved for what it writes: settle_storage() counts them, so
	// a drain while it runs keeps them; they go when it ends.
	struct hold {
		basic_index* ix;
		std::uint64_t n = 0;
		void add(std::uint64_t bytes) {
			n += bytes;
			ix->held_ += bytes;
		}
		~hold() {
			ix->held_ -= n;
			ix->settle_storage();
		}
	};

	void track(const std::string& name, std::uint64_t size) {
		auto& at = files_[name];
		on_disk_ += size - at;
		at = size;
	}
	void remove_file(const std::string& name) {
		io_->remove(name);
		forget_file(name);
	}
	// A removed file's bytes stay counted until a directory sync makes its removal durable:
	// filesystems release the blocks then, not at the unlink.
	void forget_file(const std::string& name) {
		if (auto it = files_.find(name); it != files_.end()) {
			on_disk_ -= it->second;
			unlinked_ += it->second;
			files_.erase(it);
		}
	}
	void sync_dir() {
		io_->sync_directory();
		unlinked_ = 0;
	}
	// A bound on a segment's fixed bytes, apart from its documents and terms: the header,
	// footer, and table of contents, each section's alignment and region checksums, and the
	// term index's and key filter's least sizes. (A one-document segment takes 1,088.)
	static constexpr std::uint64_t segment_overhead = 4096;

	// A bound on the files the next commit writes besides segments: a manifest with one
	// more segment entry, a live-docs file for each segment with deletes since its last
	// one, and a metadata file when the metadata changed. Each index holds it standing, so
	// a commit that frees space can always be written.
	std::uint64_t commit_allowance() const {
		std::uint64_t n = manifest_bytes_ + 256;
		for (const auto& s : segs_) {
			if (s->deletes_dirty) { n += 96 + (std::uint64_t(s->seg.core().doc_count()) + 7) / 8; }
		}
		if (meta_dirty_ || !m_.meta.generation) {
			n += 96;
			for (const auto& [k, v] : meta_) { n += k.size() + v.size() + 20; }
		}
		return n;
	}

	// Reclaims every queued file for admission that is short of space; false if none was.
	bool drain() {
		if (reclaim_.empty() && !unlinked_) { return false; }
		reclaim();
		if (unlinked_) {
			try {
				sync_dir();
			} catch (const std::system_error&) {
				return false;  // the removals stay counted; a later sync frees them
			}
			settle_storage();
		}
		return true;
	}

	// Brings the storage permit to the files' bytes, what admission reserved, and the
	// commit allowance.
	void settle_storage() {
		if (!storage_permit_) { return; }
		manifest_bytes_ = files::encode(m_).size() + 160 * segs_.size();
		allowance_ = commit_allowance();
		const std::uint64_t want = on_disk_ + unlinked_ + truncated_ + reserved_ + allowance_ + held_;
		if (want > storage_permit_.bytes()) {
			storage_permit_.grow(want - storage_permit_.bytes());
		} else {
			storage_permit_.shrink(storage_permit_.bytes() - want);
		}
	}
	// A transaction's bytes on disk, overestimated: stored records, each term with its
	// posting and positions, a per-document overhead, and deletes' live-docs entries.
	static std::uint64_t estimate_bytes(const transaction& t) {
		std::uint64_t n = 64;
		for (const auto& o : t.ops) {
			n += 16 + o.key.size() + o.meta_key.size() + o.meta_value.size();
			n += 64 + o.doc.disk_estimate();  // a per-document overhead, and the parts
		}
		return n;
	}

	// --- lookups ----------------------------------------------------------------------------

	location locate_docid(std::uint64_t docid) const {
		if (auto v = buffer_docids_.find(docid, any_value); v) { return {true, *v, 0, true}; }
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			if (auto ord = s.seg.core().ordinal_of(docid); ord && !((s.deleted[*ord >> 6] >> (*ord & 63)) & 1)) { return {true, i, *ord, false}; }
		}
		return {};
	}

	// The live copy of a key: the version map, then segments newest first.
	std::optional<key_entry> lookup_key(std::string_view key) const {
		if (auto v = key_slot(key)) { return key_entry{buffered_docid(*v), buffered_version(*v)}; }
		const std::uint64_t hash = xxh64::hash(key);  // one hash, one filter probe per segment (CR7)
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			const auto& keys = s.seg.template get<keys_plugin>();
			if (const auto ord = keys.find(key, live_bits{s.deleted.data()}, hash)) {
				return key_entry{s.seg.core().docid(*ord), keys.version(*ord)};
			}
		}
		return std::nullopt;
	}

	// --- transactions -----------------------------------------------------------------------

	// Each mutation records how to undo it; a rejection unwinds them in reverse.
	struct undo_step {
		enum kind { buffer_push, buffer_dead, segment_dead, version_map, last_docid, meta } type;
		std::size_t a = 0;
		ordinal_t ord = 0;
		std::string key;
		std::optional<std::uint32_t> prev_index;  // version_map: the key's live copy before
		std::uint64_t prev_value = 0;
		std::optional<std::string> prev_meta;
		bool prev_dirty = false;
	};

	void rollback() {
		for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
			switch (it->type) {
			case undo_step::buffer_push:
				if (inverting()) {
					buffer_docids_.erase(pending_docs_.back().docid, any_value);
					pending_docs_.pop_back();
				} else {
					buffer_docids_.erase(buffer_.back().docid, any_value);
					buffer_bytes_ -= doc_bytes(buffer_.back().doc);
					buffer_.pop_back();
				}
				break;
			case undo_step::buffer_dead:
				set_buffered_dead(it->a, false);
				set_buffered_docid(buffered_docid(it->a), std::uint32_t(it->a));
				break;
			case undo_step::segment_dead:
				segs_[it->a]->deleted[it->ord >> 6] &= ~(std::uint64_t(1) << (it->ord & 63));
				--segs_[it->a]->dead;
				segs_[it->a]->deletes_dirty = it->prev_dirty;
				break;
			case undo_step::version_map: set_key_slot(it->key, it->prev_index); break;
			case undo_step::last_docid: m_.last_docid = it->prev_value; break;
			case undo_step::meta:
				if (it->prev_meta) {
					meta_[it->key] = *it->prev_meta;
				} else {
					meta_.erase(it->key);
				}
				meta_dirty_ = it->prev_dirty;
				break;
			}
		}
	}

	// Removes a live copy; a version-map entry naming its docid goes with it.
	void remove_copy(const location& at) {
		if (at.in_buffer) {
			const std::uint64_t docid = buffered_docid(at.segment);
			if (const std::string_view key = buffered_key(at.segment); !key.empty()) {
				if (const auto v = key_slot(key); v && *v == at.segment) { set_key(key, std::nullopt); }
			}
			set_buffered_dead(at.segment, true);
			buffer_docids_.erase(docid, any_value);
			undo_.push_back({undo_step::buffer_dead, at.segment});
		} else {
			auto& s = *segs_[at.segment];
			undo_.push_back({undo_step::segment_dead, at.segment, at.ord, {}, {}, 0, {}, s.deletes_dirty});
			s.deleted[at.ord >> 6] |= std::uint64_t(1) << (at.ord & 63);
			++s.dead;
			s.deletes_dirty = true;
		}
	}
	// The key's live copy (a buffer index) set or removed, undoably.
	void set_key(std::string_view key, std::optional<std::uint32_t> index) {
		undo_step u{undo_step::version_map};
		u.key.assign(key);
		if (const auto v = key_slot(key)) { u.prev_index = *v; }
		undo_.push_back(std::move(u));
		set_key_slot(key, index);
	}
	void set_key_slot(std::string_view key, std::optional<std::uint32_t> index) {
		const std::uint64_t h = ::wordwise::hash(key.data(), key.size());
		auto same = [&](std::uint32_t v) { return v < buffer_size() && buffered_key(v) == key; };
		std::uint32_t* v = key_slots_.find(h, same);
		if (index) {
			if (v) {
				*v = *index;
			} else {
				key_slots_.insert(h, *index);
			}
		} else if (v) {
			key_slots_.erase(h, same);
		}
	}
	// The buffer index of a key's live copy in the buffer, if any.
	std::optional<std::uint32_t> key_slot(std::string_view key) const {
		const std::uint32_t* v = key_slots_.find(::wordwise::hash(key.data(), key.size()), [&](std::uint32_t i) { return i < buffer_size() && buffered_key(i) == key; });
		return v ? std::optional<std::uint32_t>(*v) : std::nullopt;
	}
	void set_buffered_docid(std::uint64_t docid, std::uint32_t index) {
		if (std::uint32_t* v = buffer_docids_.find(docid, any_value)) {
			*v = index;
		} else {
			buffer_docids_.insert(docid, index);
		}
	}
	static constexpr auto any_value = [](std::uint32_t) { return true; };  // docids are their own hashes
	void bump_last_docid(std::uint64_t docid) {
		if (docid <= m_.last_docid) { return; }
		undo_.push_back({undo_step::last_docid, 0, 0, {}, {}, m_.last_docid});
		m_.last_docid = docid;
	}

	// The core's stage-1 context: the index's key prefix (F8).
	validate_context context() const { return {m_.key_prefix}; }
	std::optional<std::string_view> key_of(const document& d, bool& valid) const { return keys_plugin::key_of(context(), d, valid); }

	// Validation that can reject runs here, before anything changes (design D10).
	bool valid(const document& d) const { return d.valid(context()); }

	verdict evaluate(const transaction& t) {
		verdict v;
		if (t.expected_schema && *t.expected_schema != m_.schema_version) {
			v.result = verdict::schema_conflict;
			v.current_version = m_.schema_version;
			return v;
		}
		for (std::size_t i = 0; i < t.ops.size(); ++i) {
			const operation& o = t.ops[i];
			v.op = i;
			auto reject = [&](verdict::outcome why, std::uint64_t current = 0) {
				v.result = why;
				v.current_version = current;
				return v;
			};
			switch (o.type) {
			case operation::set_meta:
			case operation::delete_meta: {
				if (o.meta_key.starts_with(files::reserved_meta_prefix)) { return reject(verdict::invalid); }  // the engine's (F20)
				undo_step u{undo_step::meta};
				u.key = o.meta_key;
				u.prev_dirty = meta_dirty_;
				if (auto it = meta_.find(o.meta_key); it != meta_.end()) { u.prev_meta = it->second; }
				undo_.push_back(std::move(u));
				if (o.type == operation::set_meta) {
					meta_[o.meta_key] = o.meta_value;
				} else {
					meta_.erase(o.meta_key);
				}
				meta_dirty_ = true;
				continue;
			}
			default:
				break;
			}
			const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
			bool key_valid = true;
			std::optional<std::string_view> doc_key;  // into the operation, which outlives stage 2
			if (writes) {
				if (!valid(o.doc)) { return reject(verdict::invalid); }
				doc_key = key_of(o.doc, key_valid);
				if (!key_valid) { return reject(verdict::invalid); }
			}
			// The key the operation addresses and its live copy.
			std::optional<std::string_view> key;
			if (o.type == operation::replace_key || o.type == operation::delete_key) {
				if (m_.key_prefix.empty() || !o.key.starts_with(m_.key_prefix)) { return reject(verdict::invalid); }
				key = o.key;
				if (writes && doc_key != key) { return reject(verdict::invalid); }
			} else if (writes) {
				key = doc_key;
			}
			std::optional<key_entry> by_key;
			if (key) { by_key = lookup_key(*key); }
			location by_docid;
			if (o.type == operation::replace_docid || o.type == operation::delete_docid) {
				if (o.docid == 0) { return reject(verdict::invalid); }
				by_docid = locate_docid(o.docid);
				// A key and a docid naming different live documents reject.
				if (by_key && by_docid.found && by_key->docid != o.docid) { return reject(verdict::invalid); }
				if (by_key && !by_docid.found) { return reject(verdict::invalid); }
			}
			// The current version of the addressed document.
			std::uint64_t current = 0;
			if (by_key) {
				current = by_key->version;
			} else if (by_docid.found) {
				current = version_at(by_docid);
			}
			const bool exists = by_key.has_value() || by_docid.found;
			if (o.type == operation::add && exists) { return reject(verdict::version_conflict, current); }
			if (o.expected_version) {
				// A version belongs to a key: a conditional operation without one is invalid.
				if (!key) { return reject(verdict::invalid); }
				if (*o.expected_version == 0 ? exists : (!exists || *o.expected_version != current)) { return reject(verdict::version_conflict, current); }
			}
			if (o.type == operation::delete_key || o.type == operation::delete_docid) {
				if (exists) {
					remove_copy(by_key ? locate_docid(by_key->docid) : by_docid);
					if (key) { set_key(*key, std::nullopt); }
					v.docids.push_back(by_key ? by_key->docid : o.docid);
				} else {
					v.docids.push_back(0);
				}
				v.versions.push_back(0);
				continue;
			}
			// add or replace: the docid kept, given, or assigned.
			std::uint64_t docid;
			if (o.type == operation::replace_docid) {
				docid = o.docid;
			} else if (by_key) {
				docid = by_key->docid;
			} else {
				docid = m_.last_docid + 1;
			}
			if (exists) { remove_copy(by_key ? locate_docid(by_key->docid) : by_docid); }
			// A version belongs to a key: a document without one has version 0 (F9).
			const std::uint64_t version = !key ? 0 : exists ? current + 1 : 1;
			bump_last_docid(docid);
			set_buffered_docid(docid, std::uint32_t(buffer_size()));
			if (inverting()) {
				pending_docs_.push_back({docid, version, key ? std::string_view(o.doc.key) : std::string_view(), &o.doc});
			} else {
				buffer_.push_back({docid, version, o.doc});
				if (document& d = buffer_.back().doc; d.split_text) { d.length = std::uint32_t(text_detail::count_words(d.text).words); }  // split at flush
				buffer_bytes_ += doc_bytes(buffer_.back().doc);
			}
			undo_.push_back({undo_step::buffer_push});
			if (key) { set_key(*key, std::uint32_t(buffer_size() - 1)); }
			v.docids.push_back(docid);
			v.versions.push_back(version);
		}
		v.op = 0;
		return v;
	}

	std::uint64_t version_at(const location& at) const {
		if (at.in_buffer) { return buffered_version(at.segment); }
		return segs_[at.segment]->seg.template get<keys_plugin>().version(at.ord);
	}

	void set_schema(std::uint64_t version) { m_.schema_version = version; }

	// --- segment identity ----------------------------------------------------------------

	// A new segment's physical identity (F6, F13-R41): the producing term, then a base drawn
	// at open plus a count, through splitmix64's bijective finalizer, so ids never repeat
	// within an open and collide across producers only by chance (Lucene's randomId: a random
	// base, incremented per id).
	std::array<std::uint8_t, 16> new_identity() {
		std::uint64_t z = identity_base_ + identities_++;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		z ^= z >> 31;
		std::array<std::uint8_t, 16> id{};
		std::memcpy(id.data(), &producer_term_, 8);
		std::memcpy(id.data() + 8, &z, 8);
		return id;
	}
	void seed_identities() {
		if (opts_.identity_seed) {
			identity_base_ = *opts_.identity_seed;
		} else {
			std::random_device rd;
			identity_base_ = std::uint64_t(rd()) << 32 ^ rd();
		}
		identities_ = 0;
	}

	// --- the buffer -------------------------------------------------------------------------

	bool inverting() const noexcept { return opts_.flush_threads <= 1; }
	std::size_t active_count() const noexcept { return active_ ? active_->buf.doc_count() : 0; }
	// Buffered documents, dead ones included; a document's buffer index is its ordinal in active_.
	std::size_t buffer_size() const noexcept { return inverting() ? active_count() + pending_docs_.size() : buffer_.size(); }
	std::uint64_t buffered_docid(std::size_t i) const {
		if (!inverting()) { return buffer_[i].docid; }
		return i < active_count() ? active_->buf.docid(ordinal_t(i)) : pending_docs_[i - active_count()].docid;
	}
	std::string_view buffered_key(std::size_t i) const {
		if (!inverting()) { return buffer_[i].doc.key; }
		return i < active_count() ? active_->buf.key(ordinal_t(i)) : pending_docs_[i - active_count()].key;
	}
	std::uint64_t buffered_version(std::size_t i) const {
		if (!inverting()) { return buffer_[i].version; }
		return i < active_count() ? active_->buf.version(ordinal_t(i)) : pending_docs_[i - active_count()].version;
	}
	void set_buffered_dead(std::size_t i, bool dead) {
		if (!inverting()) {
			buffer_[i].dead = dead;
		} else if (i < active_count()) {
			active_->set_dead(ordinal_t(i), dead);
		} else {
			pending_docs_[i - active_count()].dead = dead;
		}
	}

	// Stage 2 for an accepted transaction: its documents into the active writer. Validation
	// ran in stage 1 (D10), so only memory can fail here, and that fences.
	void invert_pending() {
		if (pending_docs_.empty()) { return; }
		try {
			if (!active_) {
				active_ = std::make_unique<active_buffer>();
				active_->buf.max_run_bytes = max_run_bytes_;
			}
			auto& a = *active_;
			for (const pending_doc& p : pending_docs_) {
				terms_in_.clear();
				for (const auto& t : p.doc->terms) { terms_in_.push_back({t.term, t.wdf, t.positions}); }
				const document& d = *p.doc;
				const ordinal_t ord = d.split_text ? a.buf.add_split(p.docid, terms_in_, d.text, d.stored, p.version, p.key)
												   : a.buf.add(p.docid, terms_in_, d.text, d.tokens, d.length, d.stored, p.version, p.key);
				if (ord && p.docid <= a.prev_docid) { a.ordered = false; }
				a.prev_docid = p.docid;
				if (p.dead) { a.set_dead(ord, true); }
			}
		} catch (...) {
			pending_docs_.clear();
			fenced_ = true;
			throw;
		}
		pending_docs_.clear();
	}

	// Whether the transaction's documents fit the active buffer's 32-bit structures.
	bool fits(const transaction& t) const noexcept {
		if (!active_) { return true; }
		std::size_t entries = 0;
		for (const auto& o : t.ops) { entries += o.doc.terms.size() + (o.doc.split_text ? o.doc.text.size() / 2 + 1 : o.doc.tokens.size()); }
		return active_->buf.fits(t.ops.size(), entries);
	}

	void clear_buffer() {
		buffer_.clear();
		active_.reset();
		pending_docs_.clear();
		buffer_docids_.clear();
		buffer_bytes_ = 0;
	}

	// --- flush ------------------------------------------------------------------------------

	// Writes the buffer's live documents, sorted by docid, as a new segment. Each run's
	// memory is charged to the budget on its own permit, settled when its segment is
	// published (D17); the sort's on another, settled when the flush ends.
	void flush() {
		if (inverting()) {
			flush_active();
		} else {
			flush_documents();
		}
		clear_buffer();
		key_slots_.clear();  // a cache of writer state, rebuilt from segments (I5)
		if (permit_) { permit_.settle(); }
		reserved_ = 0;  // the buffer's bytes are on disk now
		settle_storage();
	}

	// One segment from the active writer: its postings as they are when its docids increase
	// and none died, else rebuilt in docid order without the dead. A failure keeps the buffer.
	void flush_active() {
		if (!active_count()) { return; }
		auto& a = *active_;
		flush_charge run(opts_.budget);  // the flush's sort, scratch, and output, until published
		std::vector<ordinal_t> order;
		const bool as_is = a.dead_count == 0 && a.ordered;
		if (!as_is) {
			for (ordinal_t o = 0; o < a.buf.doc_count(); ++o) {
				if (!a.is_dead(o)) { order.push_back(o); }
			}
			std::sort(order.begin(), order.end(), [&](ordinal_t x, ordinal_t y) { return a.buf.docid(x) < a.buf.docid(y); });
			run.grow(order.capacity() * sizeof(ordinal_t));
			if (order.empty()) { return; }
		}
		const std::uint64_t id = m_.next_segment++;  // never reused, even after a failure
		const std::array<std::uint8_t, 16> header = new_identity();
		segment_writer encoder;
		encoder.memory = &run;
		encoder.key_prefix = m_.key_prefix;
		encoder.structures = configured_structures();
		// The segment goes to its file as it is written. ENOSPC removes the partial file and
		// keeps the buffer, as write_unsynced does.
		const std::string name = index_detail::segment_name(id);
		auto file = io_->create_exclusive(name);
		index_detail::file_sink sink(*file);
		try {
			detail::chunked_spill positions(&run);
			a.buf.flush(header, order, encoder, sink, positions);
			sink.finish();
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
			file.reset();
			io_->remove(name);
			sync_dir();
			throw space_error(e.code(), e.what());
		}
		track(name, sink.size());
		files::segment_ref ref;
		ref.id = id;
		ref.identity = header;
		ref.size = sink.size();
		std::unique_ptr<open_segment> seg;
		if (auto m = file->map(); m && m->bytes().size() == ref.size) {
			ref.footer_crc = format::decode_footer(m->bytes()).toc_crc;
			ref.doc_count = as_is ? a.buf.doc_count() : order.size();
			seg = std::make_unique<open_segment>(ref, std::move(m));
		} else {
			std::string bytes = index_detail::read_all(*file);  // a backend that cannot map
			ref.footer_crc = format::decode_footer(bytes).toc_crc;
			ref.doc_count = as_is ? a.buf.doc_count() : order.size();
			seg = std::make_unique<open_segment>(ref, std::move(bytes));
		}
		seg->unsynced = std::move(file);
		segs_.emplace_back(std::move(seg));
	}

	// More than one flush thread: the buffered documents in contiguous docid runs, one
	// segment each, inverted and encoded in parallel (D18).
	void flush_documents() {
		flush_charge sorting(opts_.budget);
		std::vector<const buffered*> docs;
		for (const auto& b : buffer_) {
			if (!b.dead) { docs.push_back(&b); }
		}
		sorting.grow(docs.capacity() * sizeof(docs[0]));
		if (!docs.empty()) {
			std::sort(docs.begin(), docs.end(), [](const buffered* a, const buffered* b) { return a->docid < b->docid; });
			// Contiguous docid runs, one segment each, inverted and encoded in parallel (D18).
			const std::size_t runs = std::max<std::size_t>(1, std::min(opts_.flush_threads, docs.size() / std::max<std::size_t>(1, opts_.flush_min_docs)));
			std::vector<std::uint64_t> ids(runs);
			std::vector<std::array<std::uint8_t, 16>> identities(runs);
			for (std::size_t k = 0; k < runs; ++k) {
				ids[k] = m_.next_segment++;  // never reused, even after a failure
				identities[k] = new_identity();
			}
			std::vector<std::string> outputs(runs);
			std::vector<std::exception_ptr> errors(runs);
			std::vector<flush_charge> charges(runs);  // each run's, held until its segment is published
			auto invert = [&](std::size_t k) {
				try {
					const std::size_t begin = docs.size() * k / runs, end = docs.size() * (k + 1) / runs;
					flush_charge charge(opts_.budget);  // on the run's own thread
					{
						segment_writer w;
						if (opts_.budget) { w.memory = &charge; }
						w.key_prefix = m_.key_prefix;
						w.structures = configured_structures();
						std::vector<posting_input> in;
						for (std::size_t i = begin; i < end; ++i) {
							const buffered* b = docs[i];
							in.clear();
							for (const auto& t : b->doc.terms) { in.push_back({t.term, t.wdf, t.positions}); }
							if (b->doc.split_text) {
								detail::writer_access::add_split(w, b->docid, in, b->doc.text, b->doc.length, b->doc.stored, b->version, b->doc.key);
							} else {
								detail::writer_access::add_text(w, b->docid, in, b->doc.text, b->doc.tokens, b->doc.length, b->doc.stored, b->version, b->doc.key);
							}
						}
						outputs[k] = w.flush(identities[k]);
					}
					// The writer's buffers are gone; the segment's bytes stay charged until published.
					charge.shrink(charge.held() - std::min<std::uint64_t>(charge.held(), outputs[k].capacity()));
					charges[k] = std::move(charge);
				} catch (...) { errors[k] = std::current_exception(); }
			};
			std::vector<std::thread> threads;
			for (std::size_t k = 1; k < runs; ++k) { threads.emplace_back(invert, k); }
			invert(0);
			for (auto& t : threads) { t.join(); }
			for (auto& e : errors) { if (e) { std::rethrow_exception(e); } }
			// Every run's file is written before any segment is published: a write that fails
			// (ENOSPC) publishes none and removes the runs already written, so the retry has
			// their space and the buffer it keeps holds no published document.
			std::vector<std::unique_ptr<kronuz::journal::File>> files(runs);
			for (std::size_t k = 0; k < runs; ++k) {
				try {
					files[k] = write_unsynced(index_detail::segment_name(ids[k]), outputs[k]);
				} catch (const space_error&) {
					for (std::size_t w = 0; w < k; ++w) { files[w].reset(); remove_file(index_detail::segment_name(ids[w])); }
					if (k) { sync_dir(); }
					throw;
				}
			}
			for (std::size_t k = 0; k < runs; ++k) {
				auto& bytes = outputs[k];
				auto file = std::move(files[k]);
				files::segment_ref ref;
				ref.id = ids[k];
				ref.identity = identities[k];
				ref.size = bytes.size();
				ref.footer_crc = format::decode_footer(bytes).toc_crc;
				ref.doc_count = docs.size() * (k + 1) / runs - docs.size() * k / runs;
				auto seg = written_segment(*file, ref, std::move(bytes));
				seg->unsynced = std::move(file);
				segs_.emplace_back(std::move(seg));
				charges[k].settle();
			}
		}
	}

	// --- commit -----------------------------------------------------------------------------

	void commit_files() {
		// A segment whose documents are all deleted leaves without a merge; retention frees
		// its files.
		std::erase_if(segs_, [](const auto& s) { return s->dead == s->seg.core().doc_count(); });
		// The manifest lists segments by increasing id, and open() refuses any other order:
		// a regression fences before anything is written, whichever caller commits.
		for (std::size_t i = 1; i < segs_.size(); ++i) {
			if (segs_[i]->ref.id <= segs_[i - 1]->ref.id) { fence("strata: commit: segments out of id order"); }
		}
		// 1. New data files, synced together: flushed segments, live-docs generations,
		// metadata. Their generations come from the counters directly, so a failed commit
		// never reuses a name.
		// Nothing changes until every file is durable, so a failed write or sync leaves the
		// next commit to write them again under new names.
		std::vector<kronuz::journal::File*> files;
		std::vector<std::unique_ptr<kronuz::journal::File>> written;
		std::vector<std::pair<open_segment*, files::live_docs>> lives;
		std::optional<files::metadata> md;
		for (auto& s : segs_) {
			if (s->unsynced) { files.push_back(s->unsynced.get()); }
			if (s->deletes_dirty) {
				files::live_docs l;
				l.segment = s->ref.id;
				l.generation = m_.next_live++;
				l.doc_count = s->seg.core().doc_count();
				for (ordinal_t o = 0; o < s->seg.core().doc_count(); ++o) {
					if ((s->deleted[o >> 6] >> (o & 63)) & 1) { l.deleted.push_back(o); }
				}
				l.live_count = l.doc_count - l.deleted.size();
				l.live_length = live_length(*s);
				written.push_back(write_unsynced(index_detail::live_name(l.segment, l.generation), files::encode(l)));
				lives.emplace_back(s.get(), std::move(l));
			}
		}
		if (meta_dirty_ || !m_.meta.generation) {
			md.emplace();
			md->generation = m_.next_meta++;
			md->entries = meta_;
			written.push_back(write_unsynced(index_detail::meta_name(md->generation), files::encode(*md)));
		}
		for (const auto& f : written) { files.push_back(f.get()); }
		io_->sync_files(files);
		for (auto& s : segs_) { s->unsynced.reset(); }
		for (auto& [s, l] : lives) {
			const std::string bytes = files::encode(l);
			s->ref.live = {l.generation, bytes.size(), files::trailing_crc(bytes)};
			s->ref.live_count = l.live_count;
			s->ref.live_length = l.live_length;
			s->deletes_dirty = false;
		}
		for (auto& s : segs_) {
			if (!s->ref.live.generation) {
				s->ref.live_count = s->seg.core().doc_count();
				s->ref.live_length = s->seg.template get<inverted_plugin>().total_length();
			}
		}
		if (md) {
			const std::string bytes = files::encode(*md);
			m_.meta = {md->generation, bytes.size(), files::trailing_crc(bytes)};
			meta_dirty_ = false;
		}
		files::manifest next = m_;
		next.generation = next_manifest_;
		if (pending_pin_) { next.pinned.push_back(*pending_pin_); }
		if (pending_unpin_) { std::erase(next.pinned, *pending_unpin_); }
		if (applied_since_commit_) { ++next.revision; }
		next.segments.clear();
		next.doc_count = next.total_length = next.live_count = next.live_length = 0;
		next.mask = next.configured;
		next.min_version = next.max_version = 0;
		for (const auto& s : segs_) {
			next.mask |= s->seg.core().structures();
			const std::uint32_t v = s->seg.core().version();
			next.min_version = next.min_version ? std::min(next.min_version, v) : v;
			next.max_version = std::max(next.max_version, v);
			next.segments.push_back(s->ref);
			next.doc_count += s->ref.doc_count;
			next.total_length += s->seg.template get<inverted_plugin>().total_length();
			next.live_count += s->ref.live_count;
			next.live_length += s->ref.live_length;
		}
		// 2-5. Directory, the manifest under a temporary name, the rename, the directory.
		sync_dir();
		const std::string tmp = index_detail::manifest_name(next.generation) + ".tmp";
		const std::string bytes = files::encode(next);
		++next_manifest_;  // a failed attempt's name is never reused
		write_new(tmp, bytes);
		io_->replace(tmp, index_detail::manifest_name(next.generation));
		files_.erase(tmp);
		files_[index_detail::manifest_name(next.generation)] = bytes.size();
		sync_dir();
		m_ = next;
		applied_since_commit_ = 0;
		pending_pin_.reset();
		pending_unpin_.reset();
	}

	void write_new(const std::string& name, std::string_view bytes) { write_unsynced(name, bytes)->sync(); }

	std::uint64_t live_length(const open_segment& s) const {
		std::uint64_t n = 0;
		for (ordinal_t o = 0; o < s.seg.core().doc_count(); ++o) {
			if (!((s.deleted[o >> 6] >> (o & 63)) & 1)) { n += s.seg.template get<inverted_plugin>().length(o); }
		}
		return n;
	}

	// --- retention --------------------------------------------------------------------------

	// Keeps the current manifest, the pinned ones, and the retained predecessors. Older
	// manifests are removed first, durably; then every file of strata's that no kept
	// manifest names is queued for reclaim(). Works from the files on record, with no
	// directory scan.
	void retain() {
		retention_blocked_.reset();
		std::set<std::uint64_t> keep(m_.pinned.begin(), m_.pinned.end());
		keep.insert(m_.generation);
		std::vector<std::uint64_t> manifests;
		for (const auto& [n, size] : files_) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { manifests.push_back(p->a); }
		}
		std::sort(manifests.rbegin(), manifests.rend());
		std::size_t predecessors = 0;
		for (std::uint64_t g : manifests) {
			if (g < m_.generation && predecessors < opts_.retained_predecessors) {
				keep.insert(g);
				++predecessors;
			}
		}
		std::erase_if(manifest_files_, [&](const auto& e) { return !keep.count(e.first); });
		std::set<std::string> referenced;
		for (std::uint64_t g : keep) {
			if (g != m_.generation && !files_.count(index_detail::manifest_name(g))) { continue; }
			const auto* names = manifest_files(g);
			if (!names) {  // a kept manifest it cannot read: remove nothing this round
				retention_blocked_ = g;
				return;
			}
			referenced.insert(names->begin(), names->end());
		}
		// Flushed segments awaiting a commit are on disk but not yet named by a manifest.
		for (const auto& s : segs_) { referenced.insert(index_detail::segment_name(s->ref.id)); }
		// Manifests leave first, durably, then the data files only they referenced: a crash
		// between the two never leaves a manifest naming a removed file.
		std::vector<std::string> obsolete;
		for (const auto& [n, size] : files_) {
			if (!referenced.count(n) && !queued_.count(n)) { obsolete.push_back(n); }
		}
		// A failure here leaves the commits standing: nothing is queued until a later round
		// removes the manifests and syncs the directory.
		try {
			for (const auto& n : obsolete) {
				if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) {
					sync_owed_ = true;
					remove_file(n);  // IO::remove: a missing file is a success
				}
			}
			if (sync_owed_) {
				sync_dir();
				sync_owed_ = false;
			}
		} catch (const std::system_error&) {
			return;
		}
		for (const auto& n : obsolete) {
			if (auto p = index_detail::parse_name(n); p && p->type != index_detail::parsed_name::manifest) {
				reclaim_.push_back(n);
				queued_.insert(n);
			}
		}
	}
	// The files manifest `g` names (itself included), read once from disk for an older one;
	// null when it cannot be read, and only a successful read is cached.
	const std::vector<std::string>* manifest_files(std::uint64_t g) {
		if (g == m_.generation) {
			auto& names = manifest_files_[g];
			names = names_of(m_);
			return &names;
		}
		if (auto it = manifest_files_.find(g); it != manifest_files_.end()) { return &it->second; }
		try {
			auto f = io_->open_existing(index_detail::manifest_name(g));
			return &(manifest_files_[g] = names_of(files::decode_manifest(index_detail::read_all(*f))));
		} catch (const std::exception&) {
			return nullptr;
		}
	}
	static std::vector<std::string> names_of(const files::manifest& mf) {
		std::vector<std::string> names{index_detail::manifest_name(mf.generation)};
		if (mf.meta.generation) { names.push_back(index_detail::meta_name(mf.meta.generation)); }
		for (const auto& s : mf.segments) {
			names.push_back(index_detail::segment_name(s.id));
			if (s.live.generation) { names.push_back(index_detail::live_name(s.id, s.live.generation)); }
		}
		return names;
	}

	// --- recovery ---------------------------------------------------------------------------

	// A create that writes nothing until its first commit (D3-C): the directory lock, an
	// interrupted create's residue cleared, the state of an empty index in memory.
	static basic_index create_deferred(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) {
		basic_index ix(io);
		ix.lock_directory(opts);
		ix.clear_uncreated();
		ix.m_.uuid = uuid;
		ix.m_.key_prefix = opts.key_prefix;
		// Every structure the index's layout reads, keys only with a key prefix (F4, F20).
		ix.m_.configured = segment::readable & ~(opts.key_prefix.empty() ? format::bit::keys : 0);
		ix.opts_ = opts;
		ix.seed_identities();
		if (opts.budget) { ix.permit_ = opts.budget->open(); }
		if (opts.storage) { ix.storage_permit_ = opts.storage->open(); }
		ix.start_merges();
		return ix;
	}

	// Whether names are only what an interrupted create leaves: the first metadata file and
	// the first manifest's temporary.
	static bool uncreated(const std::vector<std::string>& names) {
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (p && !(p->a == 1 && (p->type == index_detail::parsed_name::meta || p->type == index_detail::parsed_name::manifest_tmp))) { return false; }
		}
		return true;
	}
	// The owner lock is the directory's own (D2), writing nothing. Busy (EWOULDBLOCK) and
	// other lock errors refuse as they are. A filesystem not known local (an IO that does not
	// classify is not), or a lock it does not support, warns or refuses as options::nonlocal
	// says. A failed classification warns as an unknown filesystem, or refuses under refuse.
	void lock_directory(const options& opts) {
		const bool refuse = opts.nonlocal == options::nonlocal_policy::refuse;
		kronuz::journal::FilesystemClass fs;
		try {
			fs = io_->directory_filesystem();
		} catch (const std::system_error& e) {
			if (refuse) { throw std::system_error(e.code(), std::string("strata: the directory's filesystem is unknown: ") + e.what()); }
			fs = {false, std::string("unknown (") + e.what() + ")"};
		}
		const bool fuse = fs.name.starts_with("fuse") || fs.name.starts_with("macfuse") || fs.name.starts_with("osxfuse");
		if (!fs.local && refuse && !fuse) {
			throw std::system_error(std::make_error_code(std::errc::not_supported), "strata: " + fs.name + " is not a known-local filesystem");
		}
		try {
			lock_ = io_->acquire_directory_owner();
		} catch (const std::system_error& e) {
			if (e.code() != std::errc::not_supported || refuse) { throw; }  // unlocked: refuse's to stop
			fs_warning_ = strata::filesystem_warning{fs.name, fs.local, false};
			return;
		}
		if (!fs.local) { fs_warning_ = strata::filesystem_warning{fs.name, false, true}; }
	}

	// Removes an interrupted create's files, durably, under the owner lock; throws when the
	// directory holds a committed index or a segment. Syncs only when it removed something.
	void clear_uncreated() {
		std::vector<std::string> names;
		{
			auto cursor = io_->scan_directory();
			while (auto n = cursor->next()) { names.push_back(*n); }
		}
		if (!uncreated(names)) { throw std::system_error(std::make_error_code(std::errc::file_exists), "strata: the directory holds an index"); }
		bool removed = false;
		for (const auto& n : names) {
			if (index_detail::parse_name(n)) {
				io_->remove(n);
				removed = true;
			}
		}
		if (removed) { sync_dir(); }
	}

	void recover(std::optional<std::uint64_t> to) {
		std::vector<std::string> names;
		{
			auto cursor = io_->scan_directory();
			while (auto n = cursor->next()) { names.push_back(*n); }
		}
		std::uint64_t top = 0, max_segment = 0, max_live = 0, max_meta = 0;
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (!p) { continue; }
			track(n, read_checked([&] { return io_->open_existing(n)->size(); }, &media_retries_));  // an EIO twice is damage
			switch (p->type) {
			case index_detail::parsed_name::manifest: top = std::max(top, p->a); break;
			case index_detail::parsed_name::manifest_tmp: top = std::max(top, p->a); break;
			case index_detail::parsed_name::segment:
			case index_detail::parsed_name::spill:
			case index_detail::parsed_name::scratch: max_segment = std::max(max_segment, p->a); break;
			case index_detail::parsed_name::live:
				max_segment = std::max(max_segment, p->a);
				max_live = std::max(max_live, p->b);
				break;
			case index_detail::parsed_name::meta: max_meta = std::max(max_meta, p->a); break;
			}
		}
		std::vector<std::uint64_t> manifests;
		for (const auto& n : names) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { manifests.push_back(p->a); }
		}
		if (manifests.empty()) {
			if (uncreated(names)) { throw index_not_created("strata: no index was committed here"); }
			throw corrupt_index("no manifest");
		}
		std::sort(manifests.begin(), manifests.end());
		std::uint64_t chosen = manifests.back();
		if (to) {
			if (!std::binary_search(manifests.begin(), manifests.end(), *to)) { throw corrupt_index("no such manifest"); }
			chosen = *to;
		}
		m_ = read_checked([&] { return files::decode_manifest_for(index_detail::read_all(*io_->open_existing(index_detail::manifest_name(chosen))), readable()); }, &media_retries_);
		if (m_.generation != chosen) { throw corrupt_index("manifest generation differs from its name"); }
		// Every file the manifest names must exist and agree with its entry.
		check_readable(m_);
		auto files_of = load_files(*io_, m_, false, &media_retries_);
		segs_ = std::move(files_of.segments);
		meta_ = std::move(files_of.meta);
		if (!opts_.key_prefix.empty() && opts_.key_prefix != m_.key_prefix) { throw std::invalid_argument("strata: the index's key prefix differs"); }
		opts_.key_prefix = m_.key_prefix;
		// Counters start above every name on disk, so no name is ever reused (D3).
		m_.next_segment = std::max(m_.next_segment, max_segment + 1);
		m_.next_live = std::max(m_.next_live, max_live + 1);
		m_.next_meta = std::max(m_.next_meta, max_meta + 1);
		next_manifest_ = std::max(m_.generation, top) + 1;
		// An older manifest becomes the head by committing its content as a new generation,
		// so power loss leaves the old head or the chosen one, never a manifest between (D3).
		if (chosen != manifests.back()) { commit_files(); }
		// Orphans leave only after a successful open, under the owner lock.
		for (const auto& n : names) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest_tmp) { remove_file(n); }
		}
		retain();
		reclaim(opts_.reclaim_slice);
	}

	static std::string read_named(kronuz::journal::IO& io, const std::string& name, std::uint64_t size) {
		std::unique_ptr<kronuz::journal::File> f;
		try {
			f = io.open_existing(name);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOENT) { throw; }  // read_checked tells the medium's errors from the host's
			throw corrupt_index("missing file: " + name);
		}
		std::string bytes = index_detail::read_all(*f);
		if (bytes.size() != size) { throw corrupt_index("file size differs from the manifest: " + name); }
		return bytes;
	}

	// Every file a manifest names, read from io and checked against its entry: each
	// segment's size, footer checksum, and document count (and every byte, when `whole`);
	// each live-docs file's checksum, segment, generation, document count, live count, and
	// live length; the metadata file's checksum and generation. recover and install share
	// it, so a checkpoint is held to what an open is. Throws corrupt_index (or
	// corrupt_segment) at the first file that disagrees; changes nothing.
	struct loaded_files {
		std::vector<std::shared_ptr<open_segment>> segments;
		std::map<std::string, std::string> meta;
	};
	// Runs a read and its checks, and once more if either fails: an unreliable read (a
	// transient media fault) passes the second time; damage on the medium fails both, and
	// an EIO then reports as corrupt_index, while other errors stay what they are.
	template <class F>
	static auto read_checked(F&& f, std::uint64_t* retries) -> decltype(f()) {
		try {
			return f();
		} catch (const corrupt_index&) {
		} catch (const corrupt_segment&) {
		} catch (const std::system_error&) {
		}
		if (retries) { ++*retries; }
		try {
			return f();
		} catch (const std::system_error& e) {
			// Only a read error is the medium's; a lack of descriptors, memory, or permission is
			// the host's, and a healthy shard must not read as damaged.
			if (e.code().value() != EIO) { throw; }
			throw corrupt_index(std::string("unreadable: ") + e.what());
		}
	}
	// What this binary reads, against which decode_manifest_for classifies a manifest before any
	// segment opens (F20, F13-R42).
	static files::readable_by readable() { return {segment::readable, format::min_read_version, format::version}; }
	// Refuses configured bits no flush could write, as damage (CR5): each bit's owner set
	// (F4), and keys set exactly when the index has a key prefix (F8).
	static void check_readable(const files::manifest& m) {
		for (format::structure_bits b = m.configured; b; b &= b - 1) {
			const format::structure_bits owner = format::requires_of(b & -b);
			if (owner && !(m.configured & owner)) { throw corrupt_index("configured structure bit " + std::to_string(std::countr_zero(b)) + " without its owner"); }
		}
		if (m.key_prefix.empty() == bool(m.configured & format::bit::keys)) { throw corrupt_index("the key prefix and the configured keys bit disagree"); }
	}
	static loaded_files load_files(kronuz::journal::IO& io, const files::manifest& m, bool whole, std::uint64_t* retries = nullptr) {
		loaded_files out;
		for (const auto& r : m.segments) {
			out.segments.emplace_back(read_checked([&] { return load_segment(io, m, r, whole); }, retries));
		}
		if (m.meta.generation) {
			out.meta = read_checked([&] {
				const std::string mb = read_named(io, index_detail::meta_name(m.meta.generation), m.meta.size);
				if (files::trailing_crc(mb) != m.meta.crc) { throw corrupt_index("metadata differs from the manifest"); }
				const files::metadata md = files::decode_metadata(mb);
				if (md.generation != m.meta.generation) { throw corrupt_index("metadata generation"); }
				return md.entries;
			}, retries);
		}
		return out;
	}
	// The manifest's mask and version range passed check_readable, so a segment it names that
	// this binary cannot read disagrees with its manifest: damage (F2, F20).
	static std::unique_ptr<open_segment> load_segment(kronuz::journal::IO& io, const files::manifest& m, const files::segment_ref& r, bool whole) {
		try {
			return load_segment_of(io, m, r, whole);
		} catch (const unsupported_segment& e) {
			throw corrupt_segment(e.what());
		}
	}
	static std::unique_ptr<open_segment> load_segment_of(kronuz::journal::IO& io, const files::manifest& m, const files::segment_ref& r, bool whole) {
		const std::string name = index_detail::segment_name(r.id);
		std::unique_ptr<kronuz::journal::File> f;
		try {
			f = io.open_existing(name);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOENT) { throw; }
			throw corrupt_index("missing file: " + name);
		}
		auto mapped = f->map();
		std::string bytes;
		if (!mapped) { bytes = index_detail::read_all(*f); }
		const std::string_view view = mapped ? mapped->bytes() : std::string_view(bytes);
		if (view.size() != r.size) { throw corrupt_index("file size differs from the manifest: " + name); }
		(void)format::read_version(view);
		if (format::decode_footer(view).toc_crc != r.footer_crc) { throw corrupt_index("segment differs from its manifest entry"); }
		// The header's identity is the entry's (I13, F13-R41): a file of another producer under
		// this name is never taken for the one the manifest names.
		if (std::memcmp(view.data() + format::identity_offset, r.identity.data(), 16) != 0) { throw corrupt_index("segment identity differs from its manifest entry"); }
		auto s = mapped ? std::make_unique<open_segment>(r, std::move(mapped)) : std::make_unique<open_segment>(r, std::move(bytes));
		if (s->seg.core().doc_count() != r.doc_count) { throw corrupt_index("segment document count"); }
		// The manifest vouches for its segments: one outside its mask or version range is damage.
		const std::uint32_t v = s->seg.core().version();
		if ((s->seg.core().structures() & ~m.mask) || v < m.min_version || v > m.max_version) {
			throw corrupt_segment("segment outside its manifest's structures or versions");
		}
		if (whole) { s->seg.verify_all(); }
		if (r.live.generation) {
			const std::string lb = read_named(io, index_detail::live_name(r.id, r.live.generation), r.live.size);
			if (files::trailing_crc(lb) != r.live.crc) { throw corrupt_index("live docs differ from the manifest"); }
			const files::live_docs l = files::decode_live_docs(lb);
			if (l.segment != r.id || l.generation != r.live.generation || l.doc_count != r.doc_count || l.live_count != r.live_count ||
				l.live_length != r.live_length) {
				throw corrupt_index("live docs disagree with the manifest");
			}
			for (std::uint64_t o : l.deleted) { s->deleted[o >> 6] |= std::uint64_t(1) << (o & 63); }
			s->dead = 0;
			for (std::uint64_t w : s->deleted) { s->dead += std::uint64_t(std::popcount(w)); }
		}
		return s;
	}

	std::unique_ptr<executor::group> group_;  // first: assigned first and ended first, in ~basic_index()
	std::unique_ptr<inline_executor> own_exec_;  // merges' executor when the host passes none
	executor* exec_ = nullptr;
	kronuz::journal::IO* io_;
	std::unique_ptr<kronuz::journal::OwnerLock> lock_;
	options opts_;
	files::manifest m_;
	std::vector<std::shared_ptr<open_segment>> segs_;  // by id, as the manifest lists them; a background merge shares its sources
	std::vector<std::shared_ptr<merge_job>> pending_;  // handed to the executor, oldest first
	// The buffer (D18's stage 2). At one flush thread each accepted document is inverted into
	// active_ as apply returns; with more, buffer_ keeps documents whole for the flush's runs.
	std::vector<buffered> buffer_;
	std::unique_ptr<active_buffer> active_;
	std::vector<pending_doc> pending_docs_;  // the transaction's documents, inverted once it is accepted
	std::vector<posting_input> terms_in_;  // inversion's scratch
	std::uint64_t max_run_bytes_ = detail::tuple_buffer::run_cap;  // a run's bytes at most; tests lower it (index_test_access)
	index_detail::hash_slots buffer_docids_;  // docid -> buffer index (ordinal in active_), the docid as its hash
	index_detail::hash_slots key_slots_;      // the version map: a key -> its live copy's buffer index, its bytes read from the buffer
	std::map<std::string, std::string> meta_;
	std::vector<undo_step> undo_;
	std::uint64_t buffer_bytes_ = 0;
	byte_budget::permit permit_;
	byte_budget::permit storage_permit_;
	std::map<std::string, std::uint64_t> files_;  // the index's files and their sizes
	std::map<std::uint64_t, std::vector<std::string>> manifest_files_;  // a retained manifest's files
	std::deque<std::string> reclaim_;  // obsolete data files, oldest first
	std::set<std::string> queued_;
	bool sync_owed_ = false;  // manifests removed since the last directory sync
	std::uint64_t media_retries_ = 0;
	std::optional<std::uint64_t> retention_blocked_;
	std::optional<strata::filesystem_warning> fs_warning_;
	std::uint64_t on_disk_ = 0, reserved_ = 0;
	std::uint64_t unlinked_ = 0;  // removed, awaiting a directory sync to be freed
	std::uint64_t truncated_ = 0;  // truncated in this reclaim step, awaiting the file's sync
	std::uint64_t held_ = 0;      // reserved by a merge or install while it writes
	std::uint64_t allowance_ = 0, manifest_bytes_ = 0;  // the commit allowance, and its manifest's part
	std::uint64_t applied_since_commit_ = 0;
	std::uint64_t next_manifest_ = 1;
	std::optional<std::uint64_t> pending_pin_, pending_unpin_;
	bool meta_dirty_ = false;
	bool fenced_ = false;
	std::uint64_t producer_term_ = 0, identity_base_ = 0, identities_ = 0;
};
using index = basic_index<search_layout>;

}  // namespace strata
