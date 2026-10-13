// An index: one shard's segments, live docs, metadata, and manifests in one directory,
// with one writer (Phase 3 design). Transactions apply atomically on the owner core;
// buffered documents flush to segments sorted by docid; commit() makes everything
// applied so far durable through the commit protocol; open() recovers the last durable
// manifest and fails closed when a file it names is missing or differs.
#pragma once

#include "strata/files.hh"
#include "strata/reader.hh"
#include "strata/writer.hh"

#include "completion/io.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace strata {

// One document as the host builds it; terms are unique within it.
struct term_entry {
	std::string term;
	std::uint32_t wdf = 1;
	std::vector<std::uint32_t> positions;
};
struct document {
	std::vector<term_entry> terms;
	std::uint32_t length = 0;
	std::string stored;
};

struct operation {
	enum kind : std::uint8_t { add, replace_key, delete_key, replace_docid, delete_docid, set_meta, delete_meta };
	kind type = add;
	std::uint64_t docid = 0;                       // replace_docid, delete_docid
	std::string key;                               // replace_key, delete_key: the key term
	std::optional<std::uint64_t> expected_version;  // 0: must be absent
	document doc;                                  // add, replace_*
	std::string meta_key, meta_value;              // set_meta, delete_meta
};

struct transaction {
	std::vector<operation> ops;
	std::optional<std::uint64_t> expected_schema;  // the schema version it was built against
	std::optional<std::uint64_t> set_schema;       // the schema version it sets
};

// Apply's answer. A rejected transaction left no trace.
struct verdict {
	enum outcome : std::uint8_t { accepted, version_conflict, schema_conflict, invalid, skipped };
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

namespace index_detail {

inline std::string segment_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".sx"; }
inline std::string live_name(std::uint64_t id, std::uint64_t gen) { return "seg-" + std::to_string(id) + "." + std::to_string(gen) + ".live"; }
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
	enum kind { segment, live, meta, manifest, manifest_tmp } type;
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

inline void write_all(kronuz::journal::File& f, std::string_view bytes) {
	for (std::uint64_t at = 0; at < bytes.size();) {
		const std::size_t n = f.write_at(at, bytes.substr(at));
		if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
		at += n;
	}
}

}  // namespace index_detail

struct index_options {
	std::string key_prefix;                 // the host's key-term prefix (Xapiand's "Q"); empty: no keys
	std::size_t retained_predecessors = 2;  // manifests kept behind the current one (I14)
};

class index {
public:
	using options = index_options;

	// Creates an empty index in an empty directory and commits its first manifest.
	static index create(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) {
		index ix(io);
		ix.lock_ = io.acquire_owner(true);
		ix.m_.uuid = uuid;
		ix.m_.key_prefix = opts.key_prefix;
		ix.opts_ = opts;
		ix.commit();
		return ix;
	}

	// Opens the highest durable manifest (or `to`, deleting every higher one first), fails
	// closed when a file it names is missing or differs, then deletes orphans.
	static index open(kronuz::journal::IO& io, const options& opts = {}, std::optional<std::uint64_t> to = std::nullopt) {
		index ix(io);
		ix.lock_ = io.acquire_owner(false);
		ix.opts_ = opts;
		ix.recover(to);
		return ix;
	}

	index(index&&) noexcept = default;
	index& operator=(index&&) noexcept = default;

	// Releases the owner lock and drops all state; uncommitted work is lost. The index
	// is fenced afterwards.
	void close() {
		segs_.clear();
		buffer_.clear();
		buffer_docids_.clear();
		versions_.clear();
		lock_.reset();
		fenced_ = true;
	}

	// --- writer ---------------------------------------------------------------------------

	// Applies a transaction at a log position. Positions at or below the applied one are
	// skipped (replay); a rejected transaction changes nothing.
	verdict apply(const transaction& t, files::position at) {
		live();
		verdict v;
		if (at != files::position{} && at <= m_.applied) {
			v.result = verdict::skipped;
			return v;
		}
		undo_.clear();
		v = evaluate(t);
		if (v.result != verdict::accepted) {
			rollback();
		} else {
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
	}

	// Makes everything applied durable (design D1), then reclaims what retention allows.
	// ENOSPC from a write before that file's sync leaves the index as it was (the partial
	// file removed); any other failure, and any failed sync, rename, or directory sync,
	// fences the writer.
	void commit() {
		live();
		guarded([&] {
			flush();
			commit_files();
		});
		retain();
	}

	// --- reading --------------------------------------------------------------------------

	struct segment_view {
		std::uint64_t id;
		const segment* seg;
		const std::vector<std::uint64_t>* deleted;  // bit per ordinal
		bool live(ordinal_t ord) const { return !(((*deleted)[ord >> 6] >> (ord & 63)) & 1); }
	};
	std::vector<segment_view> segments() const {
		std::vector<segment_view> out;
		for (const auto& s : segs_) { out.push_back({s->ref.id, &s->seg, &s->deleted}); }
		return out;
	}
	const files::manifest& manifest() const noexcept { return m_; }
	std::uint64_t last_docid() const noexcept { return m_.last_docid; }
	std::uint64_t schema_version() const noexcept { return m_.schema_version; }
	const std::map<std::string, std::string>& metadata() const noexcept { return meta_; }
	std::size_t buffered_count() const noexcept { return buffer_.size(); }

	// The live document's (docid, version) for a key, if any.
	std::optional<std::pair<std::uint64_t, std::uint64_t>> find_key(std::string_view key) const {
		if (auto e = lookup_key(key)) { return std::pair{e->docid, e->version}; }
		return std::nullopt;
	}
	bool has_docid(std::uint64_t docid) const { return locate_docid(docid).found; }

private:
	explicit index(kronuz::journal::IO& io) : io_(&io) {}

	// --- in-memory state ------------------------------------------------------------------

	struct open_segment {
		files::segment_ref ref;
		std::string bytes;
		segment seg;
		std::vector<std::uint64_t> deleted;  // bit per ordinal
		bool deletes_dirty = false;          // deletes since its live-docs generation
		std::unique_ptr<kronuz::journal::File> unsynced;  // flushed, awaiting the commit's sync
		open_segment(files::segment_ref r, std::string b) : ref(r), bytes(std::move(b)), seg(bytes), deleted((seg.doc_count() + 63) / 64, 0) {}
	};
	struct buffered {
		std::uint64_t docid, version;
		std::string key;
		document doc;
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
			throw space_error(e.code(), e.what());
		}
		return f;
	}

	// --- lookups ----------------------------------------------------------------------------

	location locate_docid(std::uint64_t docid) const {
		if (auto it = buffer_docids_.find(docid); it != buffer_docids_.end()) { return {true, it->second, 0, true}; }
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			if (auto ord = s.seg.ordinal_of(docid); ord && !((s.deleted[*ord >> 6] >> (*ord & 63)) & 1)) { return {true, i, *ord, false}; }
		}
		return {};
	}

	// The live copy of a key: the version map, then segments newest first.
	std::optional<key_entry> lookup_key(std::string_view key) const {
		if (auto it = versions_.find(std::string(key)); it != versions_.end()) { return it->second; }
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			if (!s.seg.may_hold_key(key)) { continue; }
			auto info = s.seg.term(key);
			if (!info) { continue; }
			postings_iterator it(s.seg, *info);
			for (ordinal_t ord = it.next(); ord != no_more_docs; ord = it.next()) {
				if (!((s.deleted[ord >> 6] >> (ord & 63)) & 1)) { return key_entry{s.seg.docid(ord), s.seg.version(ord)}; }
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
		std::optional<key_entry> prev_key;
		std::uint64_t prev_value = 0;
		std::optional<std::string> prev_meta;
		bool prev_dirty = false;
	};

	void rollback() {
		for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
			switch (it->type) {
			case undo_step::buffer_push:
				buffer_docids_.erase(buffer_.back().docid);
				buffer_.pop_back();
				break;
			case undo_step::buffer_dead:
				buffer_[it->a].dead = false;
				buffer_docids_[buffer_[it->a].docid] = it->a;
				break;
			case undo_step::segment_dead:
				segs_[it->a]->deleted[it->ord >> 6] &= ~(std::uint64_t(1) << (it->ord & 63));
				segs_[it->a]->deletes_dirty = it->prev_dirty;
				break;
			case undo_step::version_map:
				if (it->prev_key) {
					versions_[it->key] = *it->prev_key;
				} else {
					versions_.erase(it->key);
				}
				break;
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

	void remove_copy(const location& at) {
		if (at.in_buffer) {
			buffer_[at.segment].dead = true;
			buffer_docids_.erase(buffer_[at.segment].docid);
			undo_.push_back({undo_step::buffer_dead, at.segment});
		} else {
			auto& s = *segs_[at.segment];
			undo_.push_back({undo_step::segment_dead, at.segment, at.ord, {}, {}, 0, {}, s.deletes_dirty});
			s.deleted[at.ord >> 6] |= std::uint64_t(1) << (at.ord & 63);
			s.deletes_dirty = true;
		}
	}
	void set_key(const std::string& key, std::optional<key_entry> value) {
		undo_step u{undo_step::version_map};
		u.key = key;
		if (auto it = versions_.find(key); it != versions_.end()) { u.prev_key = it->second; }
		undo_.push_back(std::move(u));
		if (value) {
			versions_[key] = *value;
		} else {
			versions_.erase(key);
		}
	}
	void bump_last_docid(std::uint64_t docid) {
		if (docid <= m_.last_docid) { return; }
		undo_.push_back({undo_step::last_docid, 0, 0, {}, {}, m_.last_docid});
		m_.last_docid = docid;
	}

	// The key term of a document: its one term with the index's key prefix.
	std::optional<std::string> key_of(const document& d, bool& valid) const {
		valid = true;
		if (m_.key_prefix.empty()) { return std::nullopt; }
		std::optional<std::string> key;
		for (const auto& t : d.terms) {
			if (t.term.starts_with(m_.key_prefix)) {
				if (key) { valid = false; }
				key = t.term;
			}
		}
		return key;
	}

	// Validation that can reject runs here, before anything changes (design D10).
	static bool valid(const document& d) {
		std::set<std::string_view> seen;
		for (const auto& t : d.terms) {
			if (t.term.empty() || t.term.size() > 245 || !seen.insert(t.term).second) { return false; }
			for (std::size_t i = 1; i < t.positions.size(); ++i) {
				if (t.positions[i] <= t.positions[i - 1]) { return false; }
			}
		}
		return true;
	}

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
			std::optional<std::string> doc_key;
			if (writes) {
				if (!valid(o.doc)) { return reject(verdict::invalid); }
				doc_key = key_of(o.doc, key_valid);
				if (!key_valid) { return reject(verdict::invalid); }
			}
			// The key the operation addresses and its live copy.
			std::optional<std::string> key;
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
			const std::uint64_t version = exists ? current + 1 : 1;
			bump_last_docid(docid);
			buffer_docids_[docid] = buffer_.size();
			buffer_.push_back({docid, version, key.value_or(std::string()), o.doc});
			undo_.push_back({undo_step::buffer_push});
			if (key) { set_key(*key, key_entry{docid, version}); }
			v.docids.push_back(docid);
			v.versions.push_back(version);
		}
		v.op = 0;
		return v;
	}

	std::uint64_t version_at(const location& at) const {
		if (at.in_buffer) { return buffer_[at.segment].version; }
		return segs_[at.segment]->seg.version(at.ord);
	}

	void set_schema(std::uint64_t version) { m_.schema_version = version; }

	// --- flush ------------------------------------------------------------------------------

	// Writes the buffer's live documents, sorted by docid, as a new segment.
	void flush() {
		std::vector<const buffered*> docs;
		for (const auto& b : buffer_) {
			if (!b.dead) { docs.push_back(&b); }
		}
		if (!docs.empty()) {
			std::sort(docs.begin(), docs.end(), [](const buffered* a, const buffered* b) { return a->docid < b->docid; });
			segment_writer w;
			std::vector<posting_input> in;
			for (const buffered* b : docs) {
				in.clear();
				for (const auto& t : b->doc.terms) { in.push_back({t.term, t.wdf, t.positions}); }
				w.add(b->docid, in, b->doc.length, b->doc.stored, b->version, b->key);
			}
			const std::uint64_t id = m_.next_segment++;  // never reused, even after a failure
			std::array<std::uint8_t, 16> header{};
			std::memcpy(header.data(), &id, 8);
			std::memcpy(header.data() + 8, m_.uuid.data(), 8);
			std::string bytes = w.flush(header);
			auto file = write_unsynced(index_detail::segment_name(id), bytes);
			files::segment_ref ref;
			ref.id = id;
			ref.size = bytes.size();
			ref.footer_crc = format::decode_footer(bytes).toc_crc;
			ref.doc_count = docs.size();
			auto seg = std::make_unique<open_segment>(ref, std::move(bytes));
			seg->unsynced = std::move(file);
			segs_.push_back(std::move(seg));
		}
		buffer_.clear();
		buffer_docids_.clear();
		versions_.clear();  // a cache of writer state, rebuilt from segments (I5)
	}

	// --- commit -----------------------------------------------------------------------------

	void commit_files() {
		// 1. New data files, synced: flushed segments, live-docs generations, metadata.
		// Their generations come from the counters directly, so a failed commit never
		// reuses a name.
		for (auto& s : segs_) {
			if (s->unsynced) {
				s->unsynced->sync();
				s->unsynced.reset();
			}
			if (s->deletes_dirty) {
				files::live_docs l;
				l.segment = s->ref.id;
				l.generation = m_.next_live++;
				l.doc_count = s->seg.doc_count();
				for (ordinal_t o = 0; o < s->seg.doc_count(); ++o) {
					if ((s->deleted[o >> 6] >> (o & 63)) & 1) { l.deleted.push_back(o); }
				}
				l.live_count = l.doc_count - l.deleted.size();
				l.live_length = live_length(*s);
				const std::string bytes = files::encode(l);
				write_new(index_detail::live_name(l.segment, l.generation), bytes);
				s->ref.live = {l.generation, bytes.size(), files::trailing_crc(bytes)};
				s->ref.live_count = l.live_count;
				s->ref.live_length = l.live_length;
				s->deletes_dirty = false;
			} else if (!s->ref.live.generation) {
				s->ref.live_count = s->seg.doc_count();
				s->ref.live_length = s->seg.total_length();
			}
		}
		if (meta_dirty_ || !m_.meta.generation) {
			files::metadata md;
			md.generation = m_.next_meta++;
			md.entries = meta_;
			const std::string bytes = files::encode(md);
			write_new(index_detail::meta_name(md.generation), bytes);
			m_.meta = {md.generation, bytes.size(), files::trailing_crc(bytes)};
			meta_dirty_ = false;
		}
		files::manifest next = m_;
		next.generation = next_manifest_;
		if (applied_since_commit_) { ++next.revision; }
		next.segments.clear();
		next.doc_count = next.total_length = next.live_count = next.live_length = 0;
		for (const auto& s : segs_) {
			next.segments.push_back(s->ref);
			next.doc_count += s->ref.doc_count;
			next.total_length += s->seg.total_length();
			next.live_count += s->ref.live_count;
			next.live_length += s->ref.live_length;
		}
		// 2-5. Directory, the manifest under a temporary name, the rename, the directory.
		io_->sync_directory();
		const std::string tmp = index_detail::manifest_name(next.generation) + ".tmp";
		const std::string bytes = files::encode(next);
		++next_manifest_;  // a failed attempt's name is never reused
		write_new(tmp, bytes);
		io_->replace(tmp, index_detail::manifest_name(next.generation));
		io_->sync_directory();
		m_ = next;
		applied_since_commit_ = 0;
	}

	void write_new(const std::string& name, std::string_view bytes) { write_unsynced(name, bytes)->sync(); }

	std::uint64_t live_length(const open_segment& s) const {
		std::uint64_t n = 0;
		for (ordinal_t o = 0; o < s.seg.doc_count(); ++o) {
			if (!((s.deleted[o >> 6] >> (o & 63)) & 1)) { n += s.seg.length(o); }
		}
		return n;
	}

	// --- retention --------------------------------------------------------------------------

	// Keeps the current manifest, the pinned ones, and the retained predecessors; deletes
	// older manifests first, then every file of strata's that no kept manifest names.
	void retain() {
		std::set<std::uint64_t> keep(m_.pinned.begin(), m_.pinned.end());
		keep.insert(m_.generation);
		std::vector<std::uint64_t> manifests;
		std::vector<std::string> names;
		auto cursor = io_->scan_directory();
		while (auto n = cursor->next()) { names.push_back(*n); }
		for (const auto& n : names) {
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
		std::set<std::string> referenced;
		for (std::uint64_t g : keep) {
			std::optional<files::manifest> mf;
			if (g == m_.generation) {
				mf = m_;
			} else {
				try {
					auto f = io_->open_existing(index_detail::manifest_name(g));
					mf = files::decode_manifest(index_detail::read_all(*f));
				} catch (const std::exception&) {
					continue;  // an unreadable retained manifest keeps nothing alive
				}
			}
			referenced.insert(index_detail::manifest_name(g));
			if (mf->meta.generation) { referenced.insert(index_detail::meta_name(mf->meta.generation)); }
			for (const auto& s : mf->segments) {
				referenced.insert(index_detail::segment_name(s.id));
				if (s.live.generation) { referenced.insert(index_detail::live_name(s.id, s.live.generation)); }
			}
		}
		// Unflushed state never lives on disk unnamed, but flushed segments awaiting a commit
		// do; they stay.
		for (const auto& s : segs_) { referenced.insert(index_detail::segment_name(s->ref.id)); }
		// Manifests leave first, then the data files only they referenced.
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (p && p->type == index_detail::parsed_name::manifest && !referenced.count(n)) { io_->remove(n); }
		}
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (p && p->type != index_detail::parsed_name::manifest && !referenced.count(n)) { io_->remove(n); }
		}
	}

	// --- recovery ---------------------------------------------------------------------------

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
			switch (p->type) {
			case index_detail::parsed_name::manifest: top = std::max(top, p->a); break;
			case index_detail::parsed_name::manifest_tmp: top = std::max(top, p->a); break;
			case index_detail::parsed_name::segment: max_segment = std::max(max_segment, p->a); break;
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
		if (manifests.empty()) { throw corrupt_index("no manifest"); }
		std::sort(manifests.begin(), manifests.end());
		std::uint64_t chosen = manifests.back();
		if (to) {
			if (!std::binary_search(manifests.begin(), manifests.end(), *to)) { throw corrupt_index("no such manifest"); }
			chosen = *to;
			// Every higher manifest leaves, durably, before anything else changes (D3).
			for (std::uint64_t g : manifests) {
				if (g > chosen) { io_->remove(index_detail::manifest_name(g)); }
			}
			io_->sync_directory();
		}
		auto mf = io_->open_existing(index_detail::manifest_name(chosen));
		m_ = files::decode_manifest(index_detail::read_all(*mf));
		if (m_.generation != chosen) { throw corrupt_index("manifest generation differs from its name"); }
		// Every file the manifest names must exist with its size and checksum.
		for (const auto& r : m_.segments) {
			std::string bytes = read_named(index_detail::segment_name(r.id), r.size);
			if (format::decode_footer(bytes).toc_crc != r.footer_crc) { throw corrupt_index("segment differs from its manifest entry"); }
			auto s = std::make_unique<open_segment>(r, std::move(bytes));
			if (s->seg.doc_count() != r.doc_count) { throw corrupt_index("segment document count"); }
			if (r.live.generation) {
				const std::string lb = read_named(index_detail::live_name(r.id, r.live.generation), r.live.size);
				if (files::trailing_crc(lb) != r.live.crc) { throw corrupt_index("live docs differ from the manifest"); }
				const files::live_docs l = files::decode_live_docs(lb);
				if (l.segment != r.id || l.generation != r.live.generation || l.doc_count != r.doc_count || l.live_count != r.live_count) {
					throw corrupt_index("live docs disagree with the manifest");
				}
				for (std::uint64_t o : l.deleted) { s->deleted[o >> 6] |= std::uint64_t(1) << (o & 63); }
			}
			segs_.push_back(std::move(s));
		}
		if (m_.meta.generation) {
			const std::string mb = read_named(index_detail::meta_name(m_.meta.generation), m_.meta.size);
			if (files::trailing_crc(mb) != m_.meta.crc) { throw corrupt_index("metadata differs from the manifest"); }
			const files::metadata md = files::decode_metadata(mb);
			if (md.generation != m_.meta.generation) { throw corrupt_index("metadata generation"); }
			meta_ = md.entries;
		}
		if (!opts_.key_prefix.empty() && opts_.key_prefix != m_.key_prefix) { throw std::invalid_argument("strata: the index's key prefix differs"); }
		opts_.key_prefix = m_.key_prefix;
		// Counters start above every name on disk, so no name is ever reused (D3).
		m_.next_segment = std::max(m_.next_segment, max_segment + 1);
		m_.next_live = std::max(m_.next_live, max_live + 1);
		m_.next_meta = std::max(m_.next_meta, max_meta + 1);
		next_manifest_ = std::max(m_.generation, top) + 1;
		// Orphans leave only after a successful open, under the owner lock.
		for (const auto& n : names) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest_tmp) { io_->remove(n); }
		}
		retain();
	}

	std::string read_named(const std::string& name, std::uint64_t size) {
		std::unique_ptr<kronuz::journal::File> f;
		try {
			f = io_->open_existing(name);
		} catch (const std::system_error&) {
			throw corrupt_index("missing file: " + name);
		}
		std::string bytes = index_detail::read_all(*f);
		if (bytes.size() != size) { throw corrupt_index("file size differs from the manifest: " + name); }
		return bytes;
	}

	kronuz::journal::IO* io_;
	std::unique_ptr<kronuz::journal::OwnerLock> lock_;
	options opts_;
	files::manifest m_;
	std::vector<std::unique_ptr<open_segment>> segs_;  // oldest first
	std::vector<buffered> buffer_;
	std::unordered_map<std::uint64_t, std::size_t> buffer_docids_;
	std::unordered_map<std::string, key_entry> versions_;  // the version map
	std::map<std::string, std::string> meta_;
	std::vector<undo_step> undo_;
	std::uint64_t applied_since_commit_ = 0;
	std::uint64_t next_manifest_ = 1;
	bool meta_dirty_ = false;
	bool fenced_ = false;
};

}  // namespace strata
