// The index on faultfs: keys and versions, replace and delete, atomic transactions,
// explicit docids, replayed positions, commits surviving crashes, fencing, ENOSPC
// recovery, retention, and fail-closed opens.
#include "prospect/index.hh"
#include "strata/core/faultfs.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <set>

namespace strata {
// Test hooks no public option offers: a lower run cap (CR12, R28).
struct index_test_access {
	static void max_run_bytes(prospect::index& ix, std::uint64_t n) { ix.max_run_bytes_ = n; }
};
}  // namespace strata

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace prospect;
using kv = std::pair<std::uint64_t, std::uint64_t>;

static const std::array<char, 16> uuid = {'s', 't', 'r', 'a', 't', 'a', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i', 'd'};

static document doc(const std::string& key, const std::string& body, std::uint32_t pos = 1) {
	document d;
	d.key = key;
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
static std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> contents(const prospect::index& ix) {
	std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> out;
	for (const auto& s : ix.segments()) {
		for (ordinal_t o = 0; o < s.seg()->core().doc_count(); ++o) {
			if (!s.live(o)) { continue; }
			const auto docid = s.seg()->core().docid(o);
			if (out.count(docid)) { std::fprintf(stderr, "two live copies of docid %llu\n", (unsigned long long)docid); }
			out[docid] = {s.seg()->get<strata::keys_plugin>().version(o), s.seg()->get<strata::stored_plugin>().stored(o)};
		}
	}
	return out;
}

// A live-docs file in the delta encoding whatever its size, deltas wrapping as unsigned.
static std::string live_deltas(const files::live_docs& l) {
	using namespace files::detail;
	std::string out(files::live_magic, 8);
	put(out, files::version);
	for (std::uint64_t v : {l.segment, l.generation, l.doc_count, l.live_count, l.live_length}) { put(out, v); }
	std::string deltas;
	for (std::size_t i = 0; i < l.deleted.size(); ++i) { put(deltas, i ? l.deleted[i] - l.deleted[i - 1] : l.deleted[i]); }
	put(out, 1);
	put_bytes(out, deltas);
	seal(out);
	return out;
}

// faultfs::io, but the n-th later open_existing of one name fails once with `error`.
class flaky_io final : public kronuz::journal::IO {
public:
	explicit flaky_io(faultfs::io& inner) : inner_(&inner) {}
	void arm(std::string name, int nth, int error, int times = 1) {
		name_ = std::move(name);
		left_ = nth;
		error_ = error;
		times_ = times;
	}
	bool fired() const { return left_ == 0; }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return inner_->acquire_owner(create); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_->acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return inner_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return inner_->directory_trust(); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override {
		if (left_ && name == name_ && --left_ == 0) {
			if (--times_ > 0) { left_ = 1; }  // the next open of it fails too
			throw std::system_error(error_, std::generic_category(), "flaky open");
		}
		return inner_->open_existing(name);
	}
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override { return inner_->create_exclusive(name); }
	void replace(std::string_view a, std::string_view b) override { inner_->replace(a, b); }
	void remove(std::string_view name) override { inner_->remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_->scan_directory(); }
	void sync_directory() override { inner_->sync_directory(); }

private:
	faultfs::io* inner_;
	std::string name_;
	int left_ = 0, error_ = 0, times_ = 1;
};

// A live-docs file in the bitset encoding for a document count near 2^64, with an empty
// body: a size computed as (count + 7) / 8 wraps to 0 and would accept it.
static std::string live_wrapping_bitset(std::uint64_t segment, std::uint64_t generation) {
	using namespace files::detail;
	std::string out(files::live_magic, 8);
	put(out, files::version);
	const std::uint64_t docs = ~std::uint64_t(0);
	for (std::uint64_t v : {segment, generation, docs, docs, std::uint64_t(0)}) { put(out, v); }
	put(out, 0);
	put_bytes(out, "");
	seal(out);
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
	prospect::index::options keyed;
	keyed.key_prefix = "Q";
	keyed.retained_predecessors = 2;  // the suite checks recover --to and pinned manifests
	// The version map's table against std::map: inserts, finds, and removals (which shift
	// entries back) with hashes narrowed so many values share one.
	for (std::uint64_t narrow : {~std::uint64_t(0), std::uint64_t(7), std::uint64_t(0)}) {
		std::mt19937_64 r(narrow);
		strata::index_detail::hash_slots t;
		std::map<std::uint32_t, std::uint64_t> model;  // value -> its hash
		for (int step = 0; step < 20000; ++step) {
			const std::uint32_t v = std::uint32_t(r() % 3000);
			const std::uint64_t h = (std::uint64_t(v) * 0x9E3779B97F4A7C15ull) & narrow;
			auto same = [&](std::uint32_t x) { return x == v; };
			const bool there = model.count(v) != 0;
			CHECK((t.find(h, same) != nullptr) == there);
			if (r() % 3 == 0) {
				CHECK(t.erase(h, same) == there);
				model.erase(v);
			} else if (!there) {
				t.insert(h, v);
				model[v] = h;
			}
			CHECK(t.size() == model.size());
		}
		for (const auto& [v, h] : model) { CHECK(t.find(h, [&](std::uint32_t x) { return x == v; }) != nullptr); }
	}
	// Keys, versions, replace, delete, and reopening.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		auto v = ix.apply(tx({add("Qa", "alpha"), add("Qb", "beta"), add("", "anonymous")}), {});
		// A document without a key has version 0 (F9).
		CHECK(v.result == verdict::accepted && v.docids == std::vector<std::uint64_t>({1, 2, 3}) && v.versions == std::vector<std::uint64_t>({1, 1, 0}));
		CHECK(ix.find_key("Qa") == kv(1, 1));  // from the version map
		CHECK(ix.get("Qa") == "alpha" && !ix.get("Qz"));  // the buffered record
		ix.refresh();
		CHECK(ix.find_key("Qa") == kv(1, 1));  // from the segment
		CHECK(ix.get("Qa") == "alpha" && ix.get("Qb") == "beta");
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
		ix = prospect::index::open(io, keyed);
		CHECK(contents(ix) == before);
		CHECK(ix.last_docid() == 4 && ix.find_key("Qa") == kv(1, 2));
	}
	// Version checks and atomicity: a rejected transaction leaves no trace.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
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
		two_keys.terms.push_back({"Qz", 0, {}});  // a term with the key prefix: invalid (F8)
		o.doc = two_keys;
		CHECK(ix.apply(tx({o}), {}).result == verdict::invalid);
		CHECK(ix.apply(tx({add("Qa", "again")}), {}).result == verdict::version_conflict);
		// The reserved metadata prefix is the engine's (F20, F13-R49): a host's set_meta or
		// delete_meta under it is invalid and undoes the transaction's earlier steps.
		auto meta = [](operation::kind k, const char* key) {
			operation m;
			m.type = k;
			m.meta_key = key;
			m.meta_value = "host";
			return m;
		};
		v = ix.apply(tx({meta(operation::set_meta, "m"), meta(operation::set_meta, "strata.order_preset")}), {});
		CHECK(v.result == verdict::invalid && v.op == 1);
		CHECK(ix.apply(tx({meta(operation::delete_meta, "strata.order_preset")}), {}).result == verdict::invalid);
		CHECK(ix.apply(tx({meta(operation::set_meta, "strata"), meta(operation::set_meta, "stratum.x")}), {}).result == verdict::accepted);
		ix.commit();
		CHECK(!ix.metadata().count("m") && !ix.metadata().count("strata.order_preset") && ix.metadata().count("strata") == 1);
	}
	// Token documents hold the same content as their terms grouped (equal digests, buffered
	// and flushed); empty, oversized, out-of-bounds, out-of-order, and keyed tokens reject, as
	// does a token repeating a terms entry.
	{
		auto tokens = [](const std::string& key, const std::string& text) {
			document d;
			d.key = key;
			d.text = text;
			std::uint32_t pos = 0;
			for (std::size_t i = 0; i < text.size();) {
				std::size_t j = text.find(' ', i);
				if (j == std::string::npos) { j = text.size(); }
				if (j > i) { d.tokens.push_back({std::uint32_t(i), std::uint32_t(j - i), ++pos}); }
				i = j + 1;
			}
			d.length = pos;
			d.stored = text;
			return d;
		};
		auto grouped = [&](const std::string& key, const std::string& text) {
			const document t = tokens(key, text);
			document d;
			d.key = t.key;
			d.terms = t.terms;
			std::map<std::string, std::vector<std::uint32_t>> by_term;
			for (const auto& tk : t.tokens) { by_term[text.substr(tk.offset, tk.size)].push_back(tk.position); }
			for (auto& [term, positions] : by_term) { d.terms.push_back({term, std::uint32_t(positions.size()), positions}); }
			d.length = t.length;
			d.stored = t.stored;
			return d;
		};
		faultfs::filesystem fa, fb;
		faultfs::io ia(fa), ib(fb);
		auto a = prospect::index::create(ia, keyed, uuid), b = prospect::index::create(ib, keyed, uuid);
		std::mt19937_64 r(5);
		const char* words[] = {"a", "b", "c", "rare", "the", "of"};
		for (int round = 0; round < 3; ++round) {
			for (int i = 0; i < 200; ++i) {
				std::string text;
				for (int w = int(r() % 30); w >= 0; --w) { text += std::string(r() % 3 ? " " : "  ") + words[r() % std::size(words)]; }
				const std::string key = "Q" + std::to_string(r() % 150);
				operation oa, ob;
				oa.type = ob.type = operation::replace_key;
				oa.key = ob.key = key;
				oa.doc = tokens(key, text);
				ob.doc = grouped(key, text);
				CHECK(a.apply(tx({oa}), {}).result == verdict::accepted && b.apply(tx({ob}), {}).result == verdict::accepted);
			}
			CHECK(a.digest() == b.digest());
			a.refresh(), b.refresh();
			CHECK(a.digest() == b.digest());
		}
		a.commit(), b.commit();
		CHECK(contents(a) == contents(b));
		auto rejects = [&](document d) {
			operation o;
			o.doc = std::move(d);
			return a.apply(tx({o}), {}).result == verdict::invalid;
		};
		document d = tokens("Qk", "one two");
		d.tokens[1].size = 0;
		CHECK(rejects(d));
		d = tokens("Qk", "one two");
		d.tokens[1].offset = std::uint32_t(d.text.size());
		CHECK(rejects(d));
		d = tokens("Qk", "one two");
		d.tokens[1].size = 4;  // past the text's end
		CHECK(rejects(d));
		d = tokens("Qk", "one two");
		d.tokens[1].position = d.tokens[0].position;
		CHECK(rejects(d));
		CHECK(rejects(tokens("Qk", "one Qtwo")));  // a token with the key prefix
		d = tokens("Qk", "one two");
		d.terms.push_back({"two", 1, {9}});
		CHECK(rejects(d));
		d = tokens("Qk", "one two");
		d.terms.push_back({"three", 1, {9}});
		operation o;
		o.doc = d;
		CHECK(a.apply(tx({o}), {}).result == verdict::accepted);
	}
	// Inversion on arrival (one flush thread) writes the bytes of documents buffered whole
	// (two threads, one run), with replaces and deletes inside the buffer, explicit docids out
	// of order, rejected transactions, and term, token, and split documents mixed.
	{
		auto seg_files = [](const faultfs::filesystem& fs) {
			std::map<std::string, std::string> out;
			for (const auto& n : fs.list()) {
				if (n.starts_with("seg-")) { out[n] = std::string(fs.view(n)); }
			}
			return out;
		};
		auto token_doc = [](const std::string& key, const std::string& text) {
			document d;
			d.key = key;
			d.text = text;
			std::uint32_t pos = 0;
			for (std::size_t i = 0; i < text.size();) {
				std::size_t j = text.find(' ', i);
				if (j == std::string::npos) { j = text.size(); }
				if (j > i) { d.tokens.push_back({std::uint32_t(i), std::uint32_t(j - i), ++pos}); }
				i = j + 1;
			}
			d.length = pos;
			d.stored = key + text;
			return d;
		};
		// The same words split by the index: buffered whole at two threads, so the digest
		// splits them (inverted_plugin::records, FM3) and the flush calls add_split.
		auto split_doc = [](const std::string& key, const std::string& text) {
			document d;
			d.key = key;
			d.text = text;
			d.split_text = true;
			d.stored = key + text;
			return d;
		};
		index::options same = keyed, whole = keyed;  // one identity seed: the files compare byte for byte (F13-R41)
		same.identity_seed = whole.identity_seed = 13;
		whole.flush_threads = 2;
		whole.flush_min_docs = std::size_t(1) << 30;
		const char* words[] = {"a", "b", "c", "rare", "the", "of"};
		for (std::uint64_t seed = 0; seed < 12; ++seed) {
			faultfs::filesystem fa, fb;
			faultfs::io ia(fa), ib(fb);
			auto a = prospect::index::create(ia, same, uuid), b = prospect::index::create(ib, whole, uuid);
			std::mt19937_64 r(seed);
			for (int round = 0; round < 3; ++round) {
				for (int i = 0; i < 300; ++i) {
					std::vector<operation> ops;
					for (int n = 1 + int(r() % 3); n > 0; --n) {
						const std::string key = "Q" + std::to_string(r() % 60);
						std::string text;
						for (int w = int(r() % 8); w >= 0; --w) { text += std::string(" ") + words[r() % std::size(words)]; }
						operation o;
						switch (r() % 8) {
						case 0: o = erase(key); break;
						case 1: o.type = operation::delete_docid, o.docid = 1 + r() % 400; break;
						case 2: o.type = operation::replace_docid, o.docid = 1 + r() % 400, o.doc = token_doc("", text); break;
						case 3: o = add("", text); break;
						case 4: o = replace(key, text, r() % 4 ? std::nullopt : std::optional<std::uint64_t>(r() % 3)); break;
						default:
							o.type = operation::replace_key, o.key = key, o.doc = r() % 2 ? token_doc(key, text) : split_doc(key, text);
							break;
						}
						ops.push_back(std::move(o));
					}
					const auto t = tx(std::move(ops));
					const auto va = a.apply(t, {}), vb = b.apply(t, {});
					CHECK(va.result == vb.result && va.docids == vb.docids && va.versions == vb.versions);
				}
				CHECK(a.buffered_count() == b.buffered_count());
				CHECK(a.digest() == b.digest());
				a.refresh(), b.refresh();
				CHECK(a.digest() == b.digest());
			}
			a.commit(), b.commit();
			CHECK(seg_files(fa) == seg_files(fb) && contents(a) == contents(b));
		}
	}
	// Documents split by the index (split_text) write the bytes of the same documents with
	// their words as explicit tokens, inverted on arrival and buffered whole; a word with the
	// key prefix, a word repeating a terms entry, or tokens or a length beside split_text reject.
	{
		auto seg_files = [](const faultfs::filesystem& fs) {
			std::map<std::string, std::string> out;
			for (const auto& n : fs.list()) {
				if (n.starts_with("seg-")) { out[n] = std::string(fs.view(n)); }
			}
			return out;
		};
		auto doc_of = [](const std::string& key, const std::string& text, bool split) {
			document d;
			d.key = key;
			d.text = text;
			d.stored = key + text;
			if (split) {
				d.split_text = true;
				return d;
			}
			std::uint32_t pos = 0;
			for (std::size_t i = 0; i < text.size();) {
				std::size_t j = text.find(' ', i);
				if (j == std::string::npos) { j = text.size(); }
				if (j > i) { d.tokens.push_back({std::uint32_t(i), std::uint32_t(j - i), ++pos}); }
				i = j + 1;
			}
			d.length = pos;
			return d;
		};
		index::options same = keyed, whole = keyed;  // one identity seed: the files compare byte for byte (F13-R41)
		same.identity_seed = whole.identity_seed = 13;
		whole.flush_threads = 2;
		whole.flush_min_docs = std::size_t(1) << 30;
		const std::string words[] = {"a", "b", "rare", "eightbyt", "ninebytes", "oneQinside", std::string(254, 'm'), std::string(300, 'l')};
		for (std::uint64_t seed = 0; seed < 4; ++seed) {
			faultfs::filesystem fa, fb, fc;
			faultfs::io ia(fa), ib(fb), ic(fc);
			auto a = prospect::index::create(ia, same, uuid), b = prospect::index::create(ib, same, uuid), c = prospect::index::create(ic, whole, uuid);
			std::mt19937_64 r(seed);
			for (int round = 0; round < 2; ++round) {
				for (int i = 0; i < 300; ++i) {
					std::string text = r() % 4 ? "" : " ";
					for (int w = int(r() % 40); w >= 0; --w) { text += words[r() % std::size(words)] + std::string(1 + r() % 3, ' '); }
					if (r() % 2) { text.pop_back(); }
					const std::string key = "Q" + std::to_string(r() % 100);
					operation split, given;
					split.type = given.type = operation::replace_key;
					split.key = given.key = key;
					split.doc = doc_of(key, text, true);
					given.doc = doc_of(key, text, false);
					CHECK(a.apply(tx({split}), {}).result == verdict::accepted);
					CHECK(b.apply(tx({given}), {}).result == verdict::accepted);
					CHECK(c.apply(tx({split}), {}).result == verdict::accepted);
				}
				CHECK(a.digest() == b.digest() && c.digest() == b.digest());
				a.refresh(), b.refresh(), c.refresh();
			}
			a.commit(), b.commit(), c.commit();
			CHECK(seg_files(fa) == seg_files(fb) && seg_files(fc) == seg_files(fb));
			auto rejects = [&](document d) {
				operation o;
				o.doc = std::move(d);
				return a.apply(tx({o}), {}).result == verdict::invalid;
			};
			CHECK(rejects(doc_of("Qk", "one Qtwo", true)));
			CHECK(rejects(doc_of("Qk", "Qone two", true)));
			CHECK(rejects(doc_of("Qk", "one  Q", true)));
			document d = doc_of("Qk", "one two", true);
			d.terms.push_back({"two", 1, {9}});
			CHECK(rejects(d));
			d = doc_of("Qk", "one two", true);
			d.tokens.push_back({0, 3, 1});
			CHECK(rejects(d));
			d = doc_of("Qk", "one two", true);
			d.length = 2;  // the index counts a split text's length
			CHECK(rejects(d));
			operation o;
			o.doc = doc_of("Qk", "oneQtwo three", true);
			CHECK(a.apply(tx({o}), {}).result == verdict::accepted);
		}
	}
	// Terms past Lucene's and Tantivy's limits (64 KiB, 1 MiB), as keys and as body terms,
	// are found buffered, flushed, reopened, and merged.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		const std::string key = "Q" + std::string(std::size_t(1) << 20, 'k'), body = std::string(65531, 'b');
		auto ix = prospect::index::create(io, keyed, uuid);
		ix.apply(tx({add(key, body), add("Qs", body + "x")}), {});
		CHECK(ix.find_key(key) == kv(1, 1) && ix.find_key("Qs") == kv(2, 1));
		ix.commit();
		ix.apply(tx({add("Qt", body)}), {});
		ix.commit();
		ix.close();
		ix = prospect::index::open(io, keyed);
		auto df = [&] {
			std::uint64_t n = 0;
			for (const auto& seg : ix.segments()) { if (auto t = seg.seg()->get<prospect::inverted_plugin>().term(body)) { n += t->df; } }
			return n;
		};
		CHECK(ix.find_key(key) == kv(1, 1) && ix.segments().size() == 2 && df() == 2);
		ix.force_merge(1);
		CHECK(ix.segments().size() == 1 && ix.find_key(key) == kv(1, 1) && df() == 2);
		CHECK(contents(ix).at(1).second == body);
	}
	// A conditional operation without a key is invalid, whatever the expected version: a
	// keyless document has no version to compare (Phase 4 review, M3).
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		auto v = ix.apply(tx({add("", "keyless")}), {});
		const auto docid = v.docids[0];
		for (std::uint64_t expected : {0u, 1u, 5u}) {
			operation o; o.type = operation::delete_docid; o.docid = docid; o.expected_version = expected;
			CHECK(ix.apply(tx({o}), {}).result == verdict::invalid);
			o.type = operation::replace_docid; o.doc = doc("", "again");
			CHECK(ix.apply(tx({o}), {}).result == verdict::invalid);
		}
		operation keyed_replace; keyed_replace.type = operation::replace_docid; keyed_replace.docid = 999; keyed_replace.doc = doc("Qn", "named");
		keyed_replace.expected_version = 0;
		CHECK(ix.apply(tx({keyed_replace}), {}).result == verdict::accepted);  // names its key: stays conditional
	}
	// Explicit docids, the key/docid agreement, and replayed positions.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
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
		ix = prospect::index::open(io, keyed);
		CHECK(ix.manifest().applied == files::position({3, 2}) && ix.has_docid(100) && ix.has_docid(101));
	}
	// Commits survive crashes; uncommitted work does not, and nothing half-applied appears.
	for (std::uint64_t seed = 0; seed < 200; ++seed) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> committed;
		{
			auto ix = prospect::index::create(io, keyed, uuid);
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
		auto ix = prospect::index::open(io, keyed);
		CHECK(contents(ix) == committed);
	}
	// A crash during commit opens either the previous commit or the new one.
	for (std::uint64_t seed = 0; seed < 300; ++seed) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> first, second;
		{
			auto ix = prospect::index::create(io, keyed, uuid);
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
		auto ix = prospect::index::open(io, keyed);
		const auto now = contents(ix);
		CHECK(now == first || now == second);
	}
	// A failed sync fences; reopening recovers the last commit.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		ix.apply(tx({add("Qa", "a")}), {});
		ix.commit();
		const auto good = contents(ix);
		ix.apply(tx({add("Qb", "b")}), {});
		fs.add_fault({faultfs::op::sync, 1, EIO, false});
		CHECK(throws([&] { ix.commit(); }));
		CHECK(throws([&] { ix.apply(tx({add("Qc", "c")}), {}); }));  // fenced
		fs.clear_faults();
		ix.close();
		ix = prospect::index(prospect::index::open(io, keyed));
		CHECK(contents(ix) == good);
	}
	// ENOSPC while flushing keeps the buffer; the same commit succeeds once space returns.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
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
	// ENOSPC on a later run of a parallel flush publishes no run, so the retry once space
	// returns leaves one live copy of each document.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto parallel = keyed; parallel.flush_threads = 2; parallel.flush_min_docs = 1;
		auto ix = prospect::index::create(io, parallel, uuid);
		std::mt19937_64 r(10);
		for (const char* key : {"Qa", "Qb"}) {
			operation big = add(key, key + 1);  // the body term without the key prefix
			big.doc.stored.resize(20000);
			for (auto& c : big.doc.stored) { c = char(r()); }  // incompressible
			CHECK(ix.apply(tx({big}), {}).result == verdict::accepted);
		}
		fs.set_capacity(fs.used() + 30000);  // room for one run's segment, not two
		bool space = false;
		try {
			ix.commit();
		} catch (const std::system_error& e) {
			space = e.code().value() == ENOSPC;
		}
		CHECK(space);
		fs.set_capacity(~std::uint64_t(0));
		ix.commit();
		std::size_t live = 0;
		for (const auto& s : ix.segments()) { for (ordinal_t o = 0; o < s.seg()->core().doc_count(); ++o) { live += s.live(o); } }
		CHECK(live == 2 && contents(ix).size() == 2);
	}
	// Merges under faults: ENOSPC leaves the index as it was and the merge succeeds once
	// space returns; any other write failure fences, and reopening recovers the last commit.
	for (int fault = 0; fault < 2; ++fault) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		std::mt19937_64 r(10);
		for (int i = 0; i < 4; ++i) {
			operation o = add("Q" + std::to_string(i), "body");
			o.doc.stored.resize(5000);
			for (auto& c : o.doc.stored) { c = char(r()); }
			CHECK(ix.apply(tx({o}), {}).result == verdict::accepted);
			if (i != 0) { ix.refresh(); }  // Q0 and Q1 share a segment, so the delete leaves it
		}
		ix.apply(tx({erase("Q1")}), {});
		ix.commit();
		const auto good = contents(ix);
		const auto digest = ix.digest();
		std::vector<std::uint64_t> ids;
		for (const auto& v : ix.segments()) { ids.push_back(v.id); }
		CHECK(ids.size() == 3);
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
			ix = prospect::index(prospect::index::open(io, keyed));
			CHECK(contents(ix) == good && ix.segments().size() == 1);
		} else {
			fs.add_fault({faultfs::op::create, 1, EIO, false});
			CHECK(throws([&] { ix.merge(ids); }));
			CHECK(throws([&] { ix.commit(); }));  // fenced
			fs.clear_faults();
			ix.close();
			ix = prospect::index(prospect::index::open(io, keyed));
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
		prospect::index::options o = keyed;
		o.storage = &storage;
		o.auto_merge = true;
		{
			auto ix = prospect::index::create(io, o, uuid);
			for (int i = 0; i < 300; ++i) {
				ix.apply(tx({add("Q" + std::to_string(i % 120), "body " + std::to_string(i))}), {});
				if (i % 40 == 39) { ix.commit(); }
			}
			ix.commit();
			CHECK(storage.used() > on_disk(fs) && storage.used() - on_disk(fs) < 4096);
		}
		CHECK(storage.used() == 0);
		auto ix = prospect::index::open(io, o);
		CHECK(storage.used() > on_disk(fs) && storage.used() - on_disk(fs) < 4096);
	}
	// A failed truncation round (condition 18): a truncate or the file's sync fails mid-round,
	// and once reclamation finishes the permit holds what a fault-free run holds.
	{
		auto beyond = [&](faultfs::op kind, std::uint64_t nth, bool fault) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			byte_budget storage(1ull << 40);
			prospect::index::options o = keyed;
			o.storage = &storage;
			o.reclaim_step = 4096;
			o.reclaim_slice = 0;
			o.retained_predecessors = 0;
			o.identity_seed = 1;  // runs compare sizes: the manifest holds each footer CRC as LEB128 (F13-R41)
			auto ix = prospect::index::create(io, o, uuid);
			std::mt19937_64 r(9);
			std::string body(40000, '\0');
			for (auto& c : body) { c = char(r()); }
			operation old;
			old.type = operation::add;
			old.doc.key = "Qold";
			old.doc.stored = body;
			ix.apply(tx({old}), {});
			ix.commit();
			ix.apply(tx({erase("Qold")}), {});
			ix.commit();
			if (fault) { fs.add_fault({kind, nth, EIO, false}); }
			try {
				ix.reclaim(8192);
			} catch (const std::exception&) {
			}
			fs.clear_faults();
			while (ix.reclaim_pending()) { ix.reclaim(8192); }
			ix.apply(tx({add("Qa", "a")}), {});
			ix.commit();
			ix.commit();
			std::uint64_t bytes = 0;
			for (const auto& n : fs.list()) {
				if (strata::index_detail::parse_name(n)) { bytes += fs.view(n).size(); }
			}
			return (long long)storage.used() - (long long)bytes;
		};
		const auto control = beyond(faultfs::op::truncate, 1, false);
		CHECK(beyond(faultfs::op::truncate, 1, true) == control);
		CHECK(beyond(faultfs::op::truncate, 2, true) == control);
		CHECK(beyond(faultfs::op::sync, 1, true) == control);
	}
	// Admission rejects with no_space and changes nothing; what it admitted commits without
	// ENOSPC on a disk of the budget's size; merges wait for room; once another index on
	// the same storage is dropped, work continues and the content holds.
	{
		faultfs::filesystem fs, other_fs;
		faultfs::io io(fs), other_io(other_fs);
		const std::uint64_t capacity = 256 * 1024;
		byte_budget storage(capacity);
		prospect::index::options o = keyed;
		o.storage = &storage;
		std::mt19937_64 r(21);
		auto doc_of = [&](const std::string& key) {
			operation op = add(key, "body");
			op.doc.stored.resize(3000);
			for (auto& c : op.doc.stored) { c = char(r()); }
			return op;
		};
		auto other = prospect::index::create(other_io, o, uuid);
		for (int i = 0; i < 30; ++i) { CHECK(other.apply(tx({doc_of("Qo" + std::to_string(i))}), {}).result == verdict::accepted); }
		other.commit();
		fs.set_capacity(capacity - storage.used());  // the disk this index has left
		auto ix = prospect::index::create(io, o, uuid);
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
		ix = prospect::index::open(io, o);
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
		prospect::index::options o = keyed;
		o.storage = &storage;
		auto ix = prospect::index::create(io, o, uuid);
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
	// A merge on a budget with `room` to spare, the disk holding what the budget holds:
	// it completes, or admission refuses it; the disk never does.
	// With `queued` bytes, an obsolete segment of one incompressible record of that size
	// waits in the reclaim queue (reclaim_slice 0 keeps it there across the reopen), so
	// admission drains it during the merge.
	auto merge_rooms = [&](const prospect::index::options& base, const std::function<void(prospect::index&)>& build, std::uint64_t first, std::uint64_t last, std::uint64_t step,
						   std::uint64_t queued = 0) {
		std::size_t merged = 0, refused = 0;
		for (std::uint64_t room = first; room <= last; room += step) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			byte_budget storage(1ull << 40);
			prospect::index::options o = base;
			o.storage = &storage;
			if (queued) {
				o.reclaim_slice = 0;
				o.retained_predecessors = 0;
			}
			auto ix = prospect::index::create(io, o, uuid);
			if (queued) {
				std::mt19937_64 r(queued);
				operation big;
				big.type = operation::replace_docid;
				big.docid = 1ull << 50;
				big.doc.stored.resize(queued);
				for (auto& c : big.doc.stored) { c = char(r()); }
				ix.apply(tx({big}), {});
				ix.commit();
				operation gone;
				gone.type = operation::delete_docid;
				gone.docid = 1ull << 50;
				ix.apply(tx({gone}), {});
				ix.commit();
				CHECK(ix.reclaim_pending() == 1);
			}
			build(ix);
			ix.commit();
			std::vector<std::uint64_t> ids;
			for (const auto& v : ix.segments()) { ids.push_back(v.id); }
			byte_budget tight(storage.used() + room);  // the same index, reopened on a budget this tight
			ix.close();
			o.storage = &tight;
			// A merge streams its output, drawing on the budget before each chunk (review L5,
			// as background merges do): with the disk holding just what the budget has, a write
			// past the reservation would meet the disk's ENOSPC, not admission's refusal.
			ix = prospect::index::open(io, o);
			fs.set_capacity(fs.used() + (tight.capacity() - tight.used()));  // the disk has what the budget has
			try {
				ix.merge(ids);
				CHECK(ix.segments().size() == 1);
				++merged;
			} catch (const std::system_error& e) {
				CHECK(std::string(e.what()).find("no storage reserved") != std::string::npos);  // admission, not the disk
				++refused;
			}
			ix.close();  // before `tight`, which its permit draws on, leaves scope
		}
		CHECK(merged > 0 && (refused > 0 || queued));
		if (!merged || (!refused && !queued)) { std::fprintf(stderr, "  merge rooms: %zu merged, %zu refused\n", merged, refused); }
	};
	merge_rooms(keyed, [&](prospect::index& ix) {
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
	}, 80000, 92000, 200);
	// Postings can grow when merged: a term on A's first document and B's 127 is a tail of
	// small deltas in B and one block in the merge, spanning the jump across A.
	merge_rooms({}, [&](prospect::index& ix) {
		auto shared = [] {
			document d;
			for (int t = 0; t < 1000; ++t) { d.terms.push_back({"t" + std::to_string(t), 0, {}}); }
			return d;
		};
		auto one = [&](document d) {
			operation op;
			op.type = operation::add;
			op.doc = std::move(d);
			CHECK(ix.apply(tx({op}), {}).result == verdict::accepted);
		};
		one(shared());
		for (int i = 0; i < 1023; ++i) {
			document d;
			d.terms.push_back({"f", 0, {}});
			one(std::move(d));
		}
		ix.refresh();
		for (int i = 0; i < 127; ++i) { one(shared()); }
	}, 150000, 200000, 1000);
	// A transaction admitted by draining a queued obsolete file commits at once on a disk of
	// the budget's size: the drain frees the file's bytes durably before they count (L8).
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		byte_budget storage(1ull << 40);
		prospect::index::options o = keyed;
		o.storage = &storage;
		o.reclaim_slice = 0;
		o.retained_predecessors = 0;
		std::mt19937_64 r(35);
		auto record = [&](const std::string& key, std::size_t bytes) {
			operation op = add(key, "x");
			op.doc.stored.resize(bytes);
			for (auto& c : op.doc.stored) { c = char(r()); }
			return op;
		};
		auto ix = prospect::index::create(io, o, uuid);
		ix.apply(tx({record("Qold", 31000)}), {});
		ix.commit();
		ix.apply(tx({erase("Qold")}), {});
		ix.commit();
		CHECK(ix.reclaim_pending() == 1);
		byte_budget tight(storage.used() + 2000);
		ix.close();
		o.storage = &tight;
		ix = prospect::index::open(io, o);
		fs.set_capacity(fs.used() + (tight.capacity() - tight.used()));
		CHECK(ix.apply(tx({record("Qnew", 10000)}), {}).result == verdict::accepted);
		CHECK(!throws([&] { ix.commit(); }));
		ix.close();
	}
	// L8, truncation: reclaim's truncation steps count as free once the truncated file is
	// synced. A budget and a disk of 80,000 bytes, an obsolete 40,000-byte segment truncated
	// in 4,096-byte steps 8,192 bytes per commit, then one record of each size: every
	// admitted commit succeeds.
	{
		std::size_t admitted = 0;
		for (std::size_t n = 500; n <= 40000; n += 500) {
			faultfs::filesystem fs(80000);
			faultfs::io io(fs);
			byte_budget storage(80000);
			prospect::index::options o = keyed;
			o.storage = &storage;
			o.reclaim_step = 4096;
			o.reclaim_slice = 8192;
			o.retained_predecessors = 0;
			std::mt19937_64 r(43);
			auto big = [&](const std::string& key, std::size_t bytes) {
				operation op = add(key, "x");
				op.doc.stored.resize(bytes);
				for (auto& c : op.doc.stored) { c = char(r()); }
				return op;
			};
			auto ix = prospect::index::create(io, o, uuid);
			ix.apply(tx({big("Qold", 40000)}), {});
			ix.commit();
			ix.apply(tx({erase("Qold")}), {});
			ix.commit();
			if (ix.apply(tx({big("Qnew", n)}), {}).result != verdict::accepted) { continue; }
			++admitted;
			CHECK(!throws([&] { ix.commit(); }));
			ix.close();
		}
		CHECK(admitted > 40);
	}
	// L8, partial files: a file removed after ENOSPC mid-write frees its bytes once its
	// removal is durable, so the commit succeeds as space returns.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		byte_budget storage(1ull << 40);
		prospect::index::options o = keyed;
		o.storage = &storage;
		auto ix = prospect::index::create(io, o, uuid);
		fs.set_capacity(fs.used() + 3000);
		fs.short_writes(97);
		std::mt19937_64 r(44);
		operation op = add("Qa", "x");
		op.doc.stored.resize(10000);
		for (auto& c : op.doc.stored) { c = char(r()); }
		CHECK(ix.apply(tx({op}), {}).result == verdict::accepted);
		std::uint64_t capacity = fs.used() + 3000;
		int attempts = 0;
		bool committed = false;
		for (; attempts <= 30 && !committed; ++attempts) {
			try {
				ix.commit();
				committed = true;
			} catch (const std::system_error&) {
				fs.set_capacity(capacity += 1000);
			}
		}
		CHECK(committed && attempts <= 12);
		ix.close();
	}
	// The same merges with an obsolete segment queued: the drain inside admission keeps the
	// merge's reservation, and counts the freed bytes once their removal is durable
	// (review L5, L8), so the disk never refuses.
	const std::function<void(prospect::index&)> growing = [&](prospect::index& ix) {
		auto shared = [] {
			document d;
			for (int t = 0; t < 1000; ++t) { d.terms.push_back({"t" + std::to_string(t), 0, {}}); }
			return d;
		};
		auto one = [&](document d) {
			operation op;
			op.type = operation::add;
			op.doc = std::move(d);
			CHECK(ix.apply(tx({op}), {}).result == verdict::accepted);
		};
		one(shared());
		for (int i = 0; i < 1023; ++i) {
			document d;
			d.terms.push_back({"f", 0, {}});
			one(std::move(d));
		}
		ix.refresh();
		for (int i = 0; i < 127; ++i) { one(shared()); }
	};
	for (std::uint64_t queued : {20000u, 50000u, 300000u}) { merge_rooms({}, growing, 158000, 228000, 2000, queued); }
	// ENOSPC strikes mid-file under short writes: the partial file is removed, and the same
	// commit succeeds once space returns.
	std::size_t mid_file = 0;
	for (std::uint64_t room = 0; room < 3000; room += 211) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		fs.short_writes(97);
		auto ix = prospect::index::create(io, keyed, uuid);
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
		ix = prospect::index::open(io, keyed);
		CHECK(contents(ix).size() == 2);
	}
	CHECK(mid_file > 3);
	// ENOSPC at every point of a commit's files (segment, live docs, metadata, manifest):
	// the commit fails without fencing, and succeeds as a reference commit does once space
	// returns.
	{
		auto run = [&](faultfs::filesystem& fs, faultfs::io& io, std::optional<std::uint64_t> room) {
			auto ix = prospect::index::create(io, keyed, uuid);
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
			ix = prospect::index::open(io, keyed);
			CHECK(contents(ix) == want && ix.digest() == digest && ix.metadata().at("m").size() == 1500);
		}
		CHECK(failed > 20);
	}
	// install holds a checkpoint to what an open holds its head to: a checkpoint whose
	// files disagree with their entries, or that cannot be read, is refused before the
	// target changes, and the target stays unfenced.
	{
		auto build_source = [&](faultfs::filesystem& fs, faultfs::io& io) {
			auto ix = prospect::index::create(io, keyed, uuid);
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
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // a live count unlike its entry, totals in step
				auto m = manifest_of(fs, g);
				for (auto& r : m.segments) {
					if (r.live.generation) {
						--r.live_count;
						--m.live_count;
					}
				}
				rewrite(io, strata::index_detail::manifest_name(g), strata::files::encode(m));
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // live docs whose delta wraps back onto an ordinal
				auto m = manifest_of(fs, g);
				for (auto& r : m.segments) {
					if (!r.live.generation) { continue; }
					const auto name = strata::index_detail::live_name(r.id, r.live.generation);
					auto l = strata::files::decode_live_docs(fs.view(name));
					l.deleted = {0, 2, 0};
					m.live_count -= r.live_count;
					l.live_count = r.live_count = r.doc_count - 3;
					m.live_count += r.live_count;
					const std::string lb = live_deltas(l);
					rewrite(io, name, lb);
					r.live.size = lb.size();
					r.live.crc = strata::files::trailing_crc(lb);
				}
				rewrite(io, strata::index_detail::manifest_name(g), strata::files::encode(m));
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // live docs whose bitset size wraps near 2^64
				auto m = manifest_of(fs, g);
				for (auto& r : m.segments) {
					if (!r.live.generation) { continue; }
					const auto name = strata::index_detail::live_name(r.id, r.live.generation);
					const std::string lb = live_wrapping_bitset(r.id, r.live.generation);
					rewrite(io, name, lb);
					r.live.size = lb.size();
					r.live.crc = strata::files::trailing_crc(lb);
				}
				rewrite(io, strata::index_detail::manifest_name(g), strata::files::encode(m));
			},
			[&](faultfs::filesystem& fs, faultfs::io& io, std::uint64_t g) {  // a manifest under another generation's name
				auto m = manifest_of(fs, g);
				m.generation = g + 5;
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
			auto target = prospect::index::create(tio, keyed, uuid);
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
			target = prospect::index::open(tio, keyed);
			CHECK(contents(target).size() == 2);
		}
		// The same checkpoint, undamaged, installs and reopens.
		faultfs::filesystem sfs, tfs;
		faultfs::io sio(sfs), tio(tfs);
		auto [source, cp] = build_source(sfs, sio);
		auto target = prospect::index::create(tio, keyed, uuid);
		target.install(sio, cp);
		CHECK(target.digest() == source.digest() && target.metadata().at("m") == "v");
		target.close();
		target = prospect::index::open(tio, keyed);
		CHECK(target.digest() == source.digest());
	}
	// install's admission covers its commit's metadata file and manifest: on a disk of the
	// budget's size, an install completes or admission refuses it; the disk never does.
	{
		faultfs::filesystem sfs;
		faultfs::io sio(sfs);
		auto source = prospect::index::create(sio, keyed, uuid);
		for (int s = 0; s < 3; ++s) {
			std::vector<operation> ops;
			for (int i = 0; i < 20; ++i) { ops.push_back(add("Q" + std::to_string(s * 20 + i), "x")); }
			source.apply(tx(ops), {});
			source.refresh();
		}
		source.apply(tx({erase("Q5")}), {});
		for (int k = 0; k < 32; ++k) {
			operation m;
			m.type = operation::set_meta;
			m.meta_key = "schema" + std::to_string(k);
			m.meta_value = std::string(1024, char('a' + k % 26));
			source.apply(tx({m}), {});
		}
		const std::uint64_t cp = source.checkpoint();
		std::uint64_t copy = 0;
		for (const auto& r : source.manifest().segments) { copy += r.size + r.live.size; }
		std::size_t installed = 0, refused = 0;
		for (std::uint64_t slack = 0; slack <= 48000; slack += 1000) {
			std::uint64_t capacity;
			{
				faultfs::filesystem fs;
				faultfs::io io(fs);
				byte_budget big(1ull << 40);
				auto o = keyed;
				o.storage = &big;
				auto t = prospect::index::create(io, o, uuid);
				t.apply(tx({add("Qt", "t")}), {});
				t.commit();
				capacity = big.used() + copy + slack;
			}
			faultfs::filesystem tfs(capacity);
			faultfs::io tio(tfs);
			byte_budget storage(capacity);
			auto o = keyed;
			o.storage = &storage;
			auto target = prospect::index::create(tio, o, uuid);
			target.apply(tx({add("Qt", "t")}), {});
			target.commit();
			try {
				target.install(sio, cp);
				CHECK(target.digest() == source.digest());
				++installed;
			} catch (const std::system_error& e) {
				CHECK(std::string(e.what()).find("no storage reserved") != std::string::npos);  // admission, not the disk
				++refused;
			}
			target.close();
		}
		CHECK(installed > 0 && refused > 0);
	}
	// Retention keeps the current manifest and two predecessors; orphans leave at open.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
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
		ix = prospect::index(prospect::index::open(io, keyed));
		for (const auto& n : fs.list()) { CHECK(n != "seg-999.sx" && n != "manifest-1.tmp"); }
		CHECK(contents(ix).size() == 6);
		// recover --to an older retained manifest commits its content as a newer
		// generation, so the rollback holds when the index reopens.
		const std::uint64_t current = ix.manifest().generation;
		ix.close();
		ix = prospect::index(prospect::index::open(io, keyed, current - 2));
		CHECK(contents(ix).size() == 4 && ix.manifest().generation > current);
		ix.close();
		ix = prospect::index(prospect::index::open(io, keyed));
		CHECK(contents(ix).size() == 4);
	}
	// A missing or changed file fails the open and changes nothing.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		{
			auto ix = prospect::index::create(io, keyed, uuid);
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
		CHECK(throws([&] { prospect::index::open(io, keyed); }));
		CHECK(fs.list().size() == names.size() - 1);
		auto f = io.create_exclusive(seg);
		std::string changed = kept;
		changed[changed.size() - 30] ^= 1;  // in the footer the manifest's CRC covers
		f->write_at(0, changed);
		CHECK(throws([&] { prospect::index::open(io, keyed); }));
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
		auto a = prospect::index::create(ia, opts, uuid), b = prospect::index::create(ib, opts, uuid);
		auto ra = prospect::index::create(ic, keyed, uuid), rb = prospect::index::create(id, keyed, uuid);  // unbudgeted
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
	// The flush's memory (D17): a writer charges its buffers to one permit per run, past the
	// capacity if it must, and writes the same bytes as an uncharged one.
	{
		flush_charge none;
		none.grow(1 << 20);
		CHECK(!none && none.bytes() == 0);
		std::vector<std::string> words;
		for (int j = 0; j < 1000; ++j) { words.push_back("t" + std::to_string(j)); }
		std::vector<std::uint32_t> pos(20);
		for (std::uint32_t j = 0; j < 20; ++j) { pos[j] = j + 1; }
		byte_budget tiny(4096);
		flush_charge c(&tiny);
		segment_writer w, plain;
		w.memory = &c;
		std::vector<posting_input> in;
		for (std::uint32_t i = 0; i < 3000; ++i) {
			in.clear();
			for (std::uint32_t j = 0; j < 20; ++j) { in.push_back({words[(i + 37 * j) % 1000], 1, std::span<const std::uint32_t>(pos.data() + j, 1)}); }
			w.add(i + 1, in, 20, "stored");
			plain.add(i + 1, in, 20, "stored");
		}
		CHECK(tiny.used() == c.bytes() && c.bytes() >= c.held() && c.held() >= (2u << 20));  // the term table and an arena chunk
		const std::uint64_t buffers = c.held();
		const std::string bytes = w.flush({}), same = plain.flush({});
		CHECK(bytes == same);
		CHECK(c.held() >= buffers + bytes.size() && tiny.used() == c.bytes() && tiny.used() > tiny.capacity() && tiny.pressure());
		c.settle();
		CHECK(tiny.used() == 0 && !tiny.pressure() && tiny.peak() >= buffers + bytes.size());
	}
	// In an index, each run's charge draws above the buffer's own bytes during the flush and
	// settles when its segment is published, on one thread or four; segments are the bytes
	// an unbudgeted index writes for the same input.
	{
		auto rich = [](int i) {
			operation o = replace("Q" + std::to_string(i), "body");
			for (std::uint32_t j = 0; j < 20; ++j) { o.doc.terms.push_back({"t" + std::to_string((i + 37 * j) % 1000), 1, {j + 2}}); }
			o.doc.length += 20;
			return o;
		};
		auto seg_files = [](const faultfs::filesystem& fs) {
			std::map<std::string, std::string> out;
			for (const auto& n : fs.list()) {
				if (n.starts_with("seg-")) { out[n] = std::string(fs.view(n)); }
			}
			return out;
		};
		for (std::size_t threads : {std::size_t(1), std::size_t(4)}) {
			byte_budget budget(1ull << 40);
			index::options opts = keyed, plain = keyed;
			opts.identity_seed = plain.identity_seed = 13;  // the files compare byte for byte (F13-R41)
			opts.budget = &budget;
			opts.flush_threads = plain.flush_threads = threads;
			opts.flush_min_docs = plain.flush_min_docs = 500;
			faultfs::filesystem fa, fb;
			faultfs::io ia(fa), ib(fb);
			auto a = prospect::index::create(ia, opts, uuid), ref = prospect::index::create(ib, plain, uuid);
			for (int i = 0; i < 6000; ++i) {
				CHECK(a.apply(tx({rich(i)}), {}).result == verdict::accepted);
				ref.apply(tx({rich(i)}), {});
			}
			const std::uint64_t buffered = budget.used(), before = budget.peak();
			CHECK(buffered > 0 && a.segments().empty());
			a.refresh();
			ref.refresh();
			CHECK(a.segments().size() == threads);
			if (threads == 1) {
				// The buffer is the writer: its segment's bytes drew on top of it.
				std::uint64_t bytes = 0;
				for (const auto& [_, f] : seg_files(fa)) { bytes += f.size(); }
				CHECK(budget.peak() > before && budget.peak() >= buffered + bytes);
			} else {
				// At least one run's term table and arena chunk drew on top of the buffer.
				CHECK(budget.peak() > before && budget.peak() >= buffered + 24 * 1024 + 4096);
			}
			CHECK(budget.used() == 0);  // every run's permit and the buffer's settled
			CHECK(a.digest() == ref.digest() && contents(a) == contents(ref));
			const auto files = seg_files(fa);
			CHECK(files.size() == threads && files == seg_files(fb));
		}
		// A budget smaller than any flush: every transaction flushes, each flush draws past
		// the capacity and completes, and the content is what an unbudgeted index holds.
		byte_budget tiny(1024);
		index::options opts = keyed;
		opts.budget = &tiny;
		faultfs::filesystem fa, fb;
		faultfs::io ia(fa), ib(fb);
		auto a = prospect::index::create(ia, opts, uuid), ref = prospect::index::create(ib, keyed, uuid);
		for (int i = 0; i < 200; ++i) {
			CHECK(a.apply(tx({rich(i)}), {}).result == verdict::accepted);
			ref.apply(tx({rich(i)}), {});
		}
		a.refresh();
		ref.refresh();
		CHECK(a.segments().size() > 100 && tiny.peak() > tiny.capacity());
		CHECK(tiny.used() == 0 && !tiny.pressure());
		CHECK(a.digest() == ref.digest() && contents(a) == contents(ref));
	}
	// The offline check: a healthy index passes and reports what it read; a byte changed
	// inside a section (which opening the index does not read) fails it.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::string seg_name;
		{
			auto ix = prospect::index::create(io, keyed, uuid);
			for (int i = 0; i < 400; ++i) { ix.apply(tx({replace("Q" + std::to_string(i % 150), "w" + std::to_string(i % 17))}), {}); }
			ix.commit();
			const auto r = ix.check();
			CHECK(r.segments == 1 && r.documents == 150 && r.terms == 17 && r.keys == 150 && r.postings == 150 && r.stored_bytes > 0 && r.deleted == 0);
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
		auto ix = prospect::index::open(io, keyed);
		CHECK(throws([&] { ix.check(); }));
	}
	// With auto_merge, a commit merges what its flush adds, so an index driven only by
	// commits stays within the policy's tiers.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto o = keyed;
		o.auto_merge = true;
		auto ix = prospect::index::create(io, o, uuid);
		for (int i = 0; i < 40; ++i) {
			CHECK(ix.apply(tx({add("Q" + std::to_string(i), "body")}), {}).result == verdict::accepted);
			ix.commit();
		}
		CHECK(ix.segments().size() < 20 && contents(ix).size() == 40);
	}
	// Reclamation: a segment whose documents are all deleted leaves at the commit without a
	// merge, and obsolete files leave reclaim_step bytes at a time, about reclaim_slice per
	// commit; power lost at any point of it leaves the commit's content and no file that
	// the next open does not reclaim.
	{
		auto o = keyed;
		o.retained_predecessors = 0;
		o.reclaim_step = 4096;
		o.reclaim_slice = 8192;
		std::mt19937_64 r(11);
		auto big = [&](const std::string& key) {
			operation op = add(key, "body");
			op.doc.stored.resize(40000);
			for (auto& c : op.doc.stored) { c = char(r()); }
			return op;
		};
		auto ids = [](const prospect::index& ix) {
			std::set<std::uint64_t> out;
			for (const auto& v : ix.segments()) { out.insert(v.id); }
			return out;
		};
		// Builds a committed index with a big segment (Q0, Q1) and a small one (Q2), then
		// deletes Q0 and Q1; returns the big segment's file name and size.
		auto setup = [&](faultfs::io& io, prospect::index& ix) {
			CHECK(ix.apply(tx({big("Q0"), big("Q1")}), {}).result == verdict::accepted);
			ix.refresh();
			const auto first = ids(ix);
			CHECK(ix.apply(tx({add("Q2", "kept")}), {}).result == verdict::accepted);
			ix.commit();
			const std::string dead = "seg-" + std::to_string(*first.begin()) + ".sx";
			const std::uint64_t size = io.open_existing(dead)->size();
			CHECK(ix.apply(tx({erase("Q0"), erase("Q1")}), {}).result == verdict::accepted);
			return std::pair{dead, size};
		};
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = prospect::index::create(io, o, uuid);
			const auto [dead, size] = setup(io, ix);
			CHECK(size > 80000);
			const auto kept = ids(ix);
			ix.commit();
			std::set<std::uint64_t> left = ids(ix);
			CHECK(left.size() == 1 && kept.count(*left.begin()));  // dropped, nothing merged
			CHECK(contents(ix).size() == 1);
			CHECK(ix.reclaim_pending() == 1 && io.open_existing(dead)->size() == size - 8192);
			CHECK(ix.reclaim(4096) == 4096 && io.open_existing(dead)->size() == size - 12288);
			ix.reclaim();
			CHECK(ix.reclaim_pending() == 0 && throws([&] { io.open_existing(dead); }));
		}
		// Admission drains queued files when space is short.
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			strata::byte_budget storage(1 << 30);
			auto so = o;
			so.storage = &storage;
			so.reclaim_slice = 0;
			auto ix = prospect::index::create(io, so, uuid);
			const auto [dead, size] = setup(io, ix);
			ix.commit();
			CHECK(ix.reclaim_pending() == 1);
			strata::byte_budget tight(storage.used() - size / 2);  // holds what is live, not the queued file
			ix.close();
			so.storage = &tight;
			ix = prospect::index::open(io, so);
			CHECK(ix.reclaim_pending() == 1);
			CHECK(ix.apply(tx({add("Q5", "new")}), {}).result == verdict::accepted);
			CHECK(ix.reclaim_pending() == 0 && throws([&] { io.open_existing(dead); }));
			ix.close();
		}
		// Power lost after every mutation of the commit and its reclamation.
		std::uint64_t points = 0;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = prospect::index::create(io, o, uuid);
			setup(io, ix);
			const auto from = fs.mutations();
			ix.commit();
			ix.reclaim();
			points = fs.mutations() - from;
		}
		CHECK(points > 20);
		for (std::uint64_t n = 0; n < points; ++n) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> before, after;
			{
				auto ix = prospect::index::create(io, o, uuid);
				setup(io, ix);
				before = contents(ix);  // the deletes are applied, not committed
				fs.power_off_after(n);
				try {
					ix.commit();
					ix.reclaim();
				} catch (const std::exception&) {
				}
			}
			fs.crash(n);
			auto ix = prospect::index::open(io, o);
			const auto now = contents(ix);
			CHECK(now.size() == 3 || now == before);
			ix.reclaim();
			ix.commit();
			std::set<std::string> named;
			for (const auto& f : fs.list()) {
				if (f.starts_with("manifest-")) { named.insert(f); }
			}
			std::size_t data = 0;
			for (const auto& f : fs.list()) { data += !named.count(f); }
			CHECK(data == ix.segments().size() + 1);  // each segment and the metadata file
			if (failures) {
				std::fprintf(stderr, "  power lost after mutation %llu of %llu\n", (unsigned long long)n, (unsigned long long)points);
				break;
			}
		}
		std::printf("  reclamation: %llu crash points\n", (unsigned long long)points);
	}
	// Retention removes nothing in a round where a kept manifest cannot be read: a transient
	// EIO or EMFILE on a pinned checkpoint's manifest, during an open or a commit, leaves
	// the checkpoint whole, and a later round reclaims what is obsolete.
	for (int error : {EIO, EMFILE}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::uint64_t pinned, digest;
		{
			auto ix = prospect::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a"), add("Qb", "b")}), {});
			pinned = ix.checkpoint();
			digest = ix.digest();
			ix.apply(tx({erase("Qa"), add("Qc", "c")}), {});
			ix.force_merge(1);
			for (int i = 0; i < 4; ++i) { ix.commit(); }
		}
		const std::string name = strata::index_detail::manifest_name(pinned);
		auto on_disk = [&] {
			const auto names = fs.list();
			return std::find(names.begin(), names.end(), name) != names.end();
		};
		flaky_io f(io);
		f.arm(name, 2, error);  // the open's first read tracks the file; the second is retention's
		auto ix = prospect::index::open(f, keyed);
		CHECK(f.fired() && on_disk() && ix.manifest().pinned == std::vector<std::uint64_t>{pinned});
		f.arm(name, 1, error);
		ix.apply(tx({add("Qd", "d")}), {});
		ix.commit();
		CHECK(f.fired() && on_disk());
		ix.apply(tx({add("Qe", "e")}), {});
		ix.commit();  // reads it, and reclaims what no kept manifest names
		ix.reclaim(UINT64_MAX);
		std::uint64_t manifests = 0;
		for (const auto& n : fs.list()) {
			if (auto p = strata::index_detail::parse_name(n); p && p->type == strata::index_detail::parsed_name::manifest) { ++manifests; }
		}
		CHECK(on_disk() && manifests == 1 + keyed.retained_predecessors + 1 && ix.reclaim_pending() == 0);
		faultfs::filesystem tfs;
		faultfs::io tio(tfs);
		auto target = prospect::index::create(tio, keyed, uuid);
		target.install(io, pinned);
		CHECK(target.digest() == digest);
	}
	// Condition 15: only an EIO that survives the retry reads as damage; a lack of file
	// descriptors or memory, or a refused permission, stays what it is.
	for (int error : {EMFILE, ENFILE, ENOMEM, EACCES, EIO}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::string segment;
		{
			auto ix = prospect::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a")}), {});
			ix.commit();
			segment = strata::index_detail::segment_name(ix.segments().at(0).id);
		}
		flaky_io f(io);
		f.arm(segment, 2, error, 2);  // the open's size check passes; both loading reads fail
		std::string first = "none";  // the open's size check itself fails twice (condition 15)
		{
			flaky_io g(io);
			g.arm(segment, 1, error, 2);
			try {
				prospect::index::open(g, keyed);
			} catch (const strata::corrupt_index&) {
				first = "corrupt";
			} catch (const std::system_error& e) {
				first = e.code().value() == error ? "itself" : "another error";
			}
		}
		CHECK(first == (error == EIO ? "corrupt" : "itself"));
		std::string what = "none";
		try {
			prospect::index::open(f, keyed);
		} catch (const strata::corrupt_index&) {
			what = "corrupt";
		} catch (const std::system_error& e) {
			what = e.code().value() == error ? "itself" : "another error";
		}
		CHECK(what == (error == EIO ? "corrupt" : "itself"));
	}
	// A kept manifest that never reads (a damaged pinned checkpoint) stops retention, which
	// the index reports; releasing the pin resumes it (condition 13).
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		ix.apply(tx({add("Qa", "a")}), {});
		const std::uint64_t pinned = ix.checkpoint();
		const auto name = strata::index_detail::manifest_name(pinned);
		std::string bytes(fs.view(name));
		bytes[bytes.size() / 2] ^= 1;
		io.remove(name);
		{
			auto f = io.create_exclusive(name);
			strata::index_detail::write_all(*f, bytes);
			f->sync();
		}
		io.sync_directory();
		ix.commit();
		ix.close();
		ix = prospect::index::open(io, keyed);  // a fresh index reads the damaged manifest
		auto strata_files = [&] {
			std::size_t n = 0;
			for (const auto& f : fs.list()) { n += strata::index_detail::parse_name(f).has_value(); }
			return n;
		};
		for (int i = 0; i < 30; ++i) {
			ix.apply(tx({replace("Qa", "a" + std::to_string(i))}), {});
			ix.commit();
		}
		CHECK(ix.retention_blocked() == pinned && strata_files() > 30);
		ix.release(pinned);
		ix.commit();
		ix.reclaim(UINT64_MAX);
		CHECK(!ix.retention_blocked() && strata_files() < 12);
	}
	// A head that pins a checkpoint whose manifest retention already removed (recover --to a
	// manifest from before the release): retention skips it and keeps reclaiming
	// (condition 14).
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::uint64_t listing;
		{
			auto ix = prospect::index::create(io, keyed, uuid);
			ix.apply(tx({add("Qa", "a")}), {});
			const std::uint64_t pinned = ix.checkpoint();
			for (int i = 0; i < 4; ++i) {
				ix.apply(tx({replace("Qa", "b" + std::to_string(i))}), {});
				ix.commit();
			}
			ix.release(pinned);
			ix.commit();
			listing = 0;  // a retained predecessor that still lists the pin
			for (const auto& n : fs.list()) {
				if (auto p = strata::index_detail::parse_name(n); p && p->type == strata::index_detail::parsed_name::manifest &&
					strata::files::decode_manifest(fs.view(n)).pinned == std::vector<std::uint64_t>{pinned}) {
					listing = p->a;
				}
			}
			CHECK(listing != 0);
			for (const auto& n : fs.list()) { CHECK(n != strata::index_detail::manifest_name(pinned)); }
		}
		auto ix = prospect::index::open(io, keyed, listing);
		for (int i = 0; i < 50; ++i) {
			ix.apply(tx({replace("Qa", "c" + std::to_string(i))}), {});
			ix.commit();
		}
		ix.reclaim(UINT64_MAX);
		std::size_t n = 0;
		for (const auto& f : fs.list()) { n += strata::index_detail::parse_name(f).has_value(); }
		CHECK(!ix.retention_blocked() && n < 20);
	}
	// A merge whose admission drains through a failing truncate or unlink is not fenced; the
	// file stays queued (condition 14, condition 10's merge half).
	for (faultfs::op kind : {faultfs::op::truncate, faultfs::op::remove}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		byte_budget storage(1ull << 40);
		prospect::index::options o = keyed;
		o.storage = &storage;
		o.reclaim_slice = 0;
		o.reclaim_step = 4096;
		o.retained_predecessors = 0;
		std::mt19937_64 r(14);
		auto ix = prospect::index::create(io, o, uuid);
		operation big = add("Qold", "x");
		big.doc.stored.resize(40000);
		for (auto& c : big.doc.stored) { c = char(r()); }
		ix.apply(tx({big}), {});
		ix.commit();
		ix.apply(tx({erase("Qold")}), {});
		ix.commit();
		for (int s = 0; s < 2; ++s) {
			std::vector<operation> ops;
			for (int i = 0; i < 20; ++i) { ops.push_back(add("Q" + std::to_string(s * 20 + i), "y")); }
			ix.apply(tx(ops), {});
			ix.refresh();
		}
		ix.commit();
		CHECK(ix.reclaim_pending() == 1);
		std::vector<std::uint64_t> ids;
		for (const auto& v : ix.segments()) { ids.push_back(v.id); }
		byte_budget tight(storage.used() + 1000);  // the merge needs the drain
		ix.close();
		o.storage = &tight;
		ix = prospect::index::open(io, o);
		fs.add_fault({kind, 1, EIO, false});
		try {
			ix.merge(ids);
		} catch (const std::system_error& e) {
			CHECK(std::string(e.what()).find("no storage reserved") != std::string::npos);
		}
		fs.clear_faults();
		CHECK(ix.reclaim_pending() == 1);  // the failed removal stays queued
		(void)ix.apply(tx({add("Qz", "z")}), {});  // accepted or no_space, never fenced
		CHECK(!throws([&] { ix.commit(); }));  // not fenced
		ix.close();
	}
	// A queued file that is gone before its truncation opens it is forgotten, not an error.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		prospect::index::options o = keyed;
		o.reclaim_slice = 0;
		o.reclaim_step = 4096;
		o.retained_predecessors = 0;
		std::mt19937_64 r(15);
		auto ix = prospect::index::create(io, o, uuid);
		operation big = add("Qold", "x");
		big.doc.stored.resize(40000);
		for (auto& c : big.doc.stored) { c = char(r()); }
		ix.apply(tx({big}), {});
		ix.commit();
		const auto gone = strata::index_detail::segment_name(ix.segments().at(0).id);
		ix.apply(tx({erase("Qold")}), {});
		ix.commit();
		CHECK(ix.reclaim_pending() == 1);
		io.remove(gone);
		CHECK(!throws([&] { ix.reclaim(UINT64_MAX); }) && ix.reclaim_pending() == 0);
	}
	// A failed truncate, unlink, or directory sync after the commit point neither fails
	// commit() nor loses the commit; the file stays queued and a later round reclaims it.
	for (faultfs::op kind : {faultfs::op::truncate, faultfs::op::remove, faultfs::op::sync_directory}) {
		std::size_t returned = 0;
		for (std::uint64_t nth = 1; nth <= 6; ++nth) {
			prospect::index::options o = keyed;
			o.retained_predecessors = 0;
			o.reclaim_step = 4096;
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = prospect::index::create(io, o, uuid);
			ix.apply(tx({add("Qa", std::string(40000, 'a'))}), {});
			ix.commit();
			ix.apply(tx({erase("Qa"), add("Qb", "b")}), {1, 1});
			fs.add_fault({kind, nth, EIO, false});
			try {
				ix.commit();
				++returned;
			} catch (const strata::index_fenced&) {
				fs.clear_faults();
				continue;  // a failure before the commit point
			}
			fs.clear_faults();
			ix.apply(tx({add("Qc", "c")}), {2, 1});
			ix.commit();
			ix.reclaim(UINT64_MAX);
			CHECK(ix.reclaim_pending() == 0);
			std::uint64_t bytes = 0;
			for (const auto& n : fs.list()) { bytes += fs.view(n).size(); }
			CHECK(bytes < 20000);  // the 40,000-byte segment is gone
			ix.close();
			fs.crash(nth);
			auto again = prospect::index::open(io, o);
			CHECK(again.find_key("Qb") && !again.find_key("Qa"));
		}
		CHECK(returned > 0);
	}
	// ENOSPC on a later file of a commit's batch, then a successful retry, then power loss:
	// the retry makes every file the manifest names durable.
	for (std::uint64_t seed = 0; seed < 20; ++seed) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		for (int s = 0; s < 3; ++s) {
			std::vector<operation> ops;
			for (int i = 0; i < 4; ++i) { ops.push_back(add("Q" + std::to_string(s * 4 + i), "x")); }
			ix.apply(tx(ops), {});
			ix.refresh();
		}
		ix.commit();
		ix.apply(tx({erase("Q0"), erase("Q4"), erase("Q8")}), {});  // a live-docs file per segment
		const auto want = contents(ix);
		fs.add_fault({faultfs::op::write, 2, ENOSPC, false});
		CHECK(throws([&] { ix.commit(); }));
		fs.clear_faults();
		ix.commit();
		ix.close();
		fs.crash(seed);
		CHECK(contents(prospect::index::open(io, keyed)) == want);
	}
	// Binding checks (review condition 4): an intact file from another generation or another
	// index under a name the manifest gives, or a manifest resealed with entries its files
	// do not match, fails the open with that check's error and changes nothing on disk.
	{
		// An index of keys Q0..Q9 with bodies of `letter`: two commits, so a segment carries
		// two live-docs generations and the metadata two, all retained.
		auto build = [&](faultfs::io& io, char letter, const std::array<char, 16>& id, int first_deleted) {
			auto ix = prospect::index::create(io, keyed, id);
			std::vector<operation> ops;
			for (int i = 0; i < 10; ++i) { ops.push_back(add("Q" + std::to_string(i), std::string(4, letter))); }
			CHECK(ix.apply(tx(ops), {}).result == verdict::accepted);
			operation m;
			m.type = operation::set_meta;
			m.meta_key = "m";
			m.meta_value = "v1";
			ix.apply(tx({m}), {});
			ix.commit();
			ix.apply(tx({erase("Q" + std::to_string(first_deleted))}), {});
			m.meta_value = "v2";
			ix.apply(tx({m}), {});
			ix.commit();
			ix.apply(tx({erase("Q" + std::to_string(first_deleted + 1))}), {});
			m.meta_value = "v3";
			ix.apply(tx({m}), {});
			ix.commit();
		};
		auto put = [](faultfs::io& io, const std::string& name, std::string_view bytes) {
			try {
				io.remove(name);
			} catch (const std::exception&) {
			}
			auto f = io.create_exclusive(name);
			strata::index_detail::write_all(*f, bytes);
			f->sync();
			io.sync_directory();
		};
		auto head = [](faultfs::filesystem& fs) {
			std::string best;
			std::uint64_t top = 0;
			for (const auto& n : fs.list()) {
				if (auto p = strata::index_detail::parse_name(n); p && p->type == strata::index_detail::parsed_name::manifest && p->a >= top) {
					top = p->a;
					best = n;
				}
			}
			return best;
		};
		auto snapshot_of = [](faultfs::filesystem& fs) {
			std::map<std::string, std::string> out;
			for (const auto& n : fs.list()) { out[n] = std::string(fs.view(n)); }
			return out;
		};
		const std::array<char, 16> other = {'o', 't', 'h', 'e', 'r'};
		// Each case: damage, then the open must fail with `want` and leave every file as it was.
		auto expect = [&](const char* what, const std::function<void(faultfs::filesystem&, faultfs::io&, faultfs::filesystem&)>& damage, const std::string& want) {
			faultfs::filesystem fs, ofs;
			faultfs::io io(fs), oio(ofs);
			build(io, 'a', uuid, 1);
			build(oio, 'b', other, 3);
			damage(fs, io, ofs);
			const auto before = snapshot_of(fs);
			std::string got;
			try {
				prospect::index::open(io, keyed);
			} catch (const std::exception& e) {
				got = e.what();
			}
			CHECK(got.find(want) != std::string::npos);
			CHECK(snapshot_of(fs) == before);
			if (got.find(want) == std::string::npos) { std::fprintf(stderr, "  %s: got \"%s\"\n", what, got.c_str()); }
		};
		auto manifest_of = [&](faultfs::filesystem& fs) { return strata::files::decode_manifest(fs.view(head(fs))); };
		auto reseal = [&](faultfs::filesystem& fs, faultfs::io& io, const strata::files::manifest& m) { put(io, head(fs), strata::files::encode(m)); };
		expect("segment of another size", [&](auto& fs, auto& io, auto&) {
			const auto mine = manifest_of(fs);
			const auto& r = mine.segments[0];
			put(io, strata::index_detail::segment_name(r.id), std::string(fs.view(strata::index_detail::segment_name(r.id))) + std::string(8, '\0'));
		}, "file size differs from the manifest");
		expect("another index's segment of the same size", [&](auto& fs, auto& io, auto& ofs) {
			const auto mine = manifest_of(fs);
			const auto& r = mine.segments[0];
			const auto theirs_m = manifest_of(ofs);
			const auto theirs = std::string(ofs.view(strata::index_detail::segment_name(theirs_m.segments[0].id)));
			CHECK(theirs.size() == r.size);
			put(io, strata::index_detail::segment_name(r.id), theirs);
		}, "segment differs from its manifest entry");
		expect("a resealed document count", [&](auto& fs, auto& io, auto&) {
			auto m = manifest_of(fs);
			m.segments[0].doc_count += 1;
			m.doc_count += 1;  // the manifest's own totals still agree
			reseal(fs, io, m);
		}, "segment document count");
		expect("live docs of another generation, resealed", [&](auto& fs, auto& io, auto&) {
			auto m = manifest_of(fs);
			auto& r = m.segments[0];
			const auto older = std::string(fs.view(strata::index_detail::live_name(r.id, r.live.generation - 1)));
			put(io, strata::index_detail::live_name(r.id, r.live.generation), older);
			r.live.size = older.size();
			r.live.crc = strata::files::trailing_crc(older);
			reseal(fs, io, m);
		}, "live docs disagree with the manifest");
		expect("another index's live docs of the same size", [&](auto& fs, auto& io, auto& ofs) {
			const auto mine = manifest_of(fs);
			const auto& r = mine.segments[0];
			const auto theirs_m = manifest_of(ofs);
			const auto& o = theirs_m.segments[0];
			const auto theirs = std::string(ofs.view(strata::index_detail::live_name(o.id, o.live.generation)));
			CHECK(theirs.size() == r.live.size);
			put(io, strata::index_detail::live_name(r.id, r.live.generation), theirs);
		}, "live docs differ from the manifest");
		auto relive = [&](faultfs::filesystem& fs, faultfs::io& io, const std::function<void(strata::files::live_docs&)>& change, bool deltas) {
			auto m = manifest_of(fs);
			auto& r = m.segments[0];
			const auto name = strata::index_detail::live_name(r.id, r.live.generation);
			auto l = strata::files::decode_live_docs(fs.view(name));
			change(l);
			const std::string lb = deltas ? live_deltas(l) : strata::files::encode(l);
			put(io, name, lb);
			r.live.size = lb.size();
			r.live.crc = strata::files::trailing_crc(lb);
			reseal(fs, io, m);
		};
		expect("live docs of another segment, resealed", [&](auto& fs, auto& io, auto&) {
			relive(fs, io, [](auto& l) { ++l.segment; }, false);
		}, "live docs disagree with the manifest");
		expect("live docs whose bitset size wraps near 2^64", [&](auto& fs, auto& io, auto&) {
			auto m = manifest_of(fs);
			auto& r = m.segments[0];
			const std::string lb = live_wrapping_bitset(r.id, r.live.generation);
			put(io, strata::index_detail::live_name(r.id, r.live.generation), lb);
			r.live.size = lb.size();
			r.live.crc = strata::files::trailing_crc(lb);
			reseal(fs, io, m);
		}, "bitset size");
		expect("a resealed live count, totals in step", [&](auto& fs, auto& io, auto&) {
			auto m = manifest_of(fs);
			--m.segments[0].live_count;
			--m.live_count;
			reseal(fs, io, m);
		}, "live docs disagree with the manifest");
		expect("a resealed live length, totals in step", [&](auto& fs, auto& io, auto&) {
			auto m = manifest_of(fs);
			--m.segments[0].live_length;
			--m.live_length;
			reseal(fs, io, m);
		}, "live docs disagree with the manifest");
		expect("live docs whose delta wraps back onto an ordinal", [&](auto& fs, auto& io, auto&) {
			relive(fs, io, [](auto& l) { l.deleted.push_back(l.deleted.front()); --l.live_count; }, true);
		}, "ordinal past the document count");
		expect("metadata of another generation", [&](auto& fs, auto& io, auto&) {
			const auto m = manifest_of(fs);
			const auto older = std::string(fs.view(strata::index_detail::meta_name(m.meta.generation - 1)));
			CHECK(older.size() == m.meta.size);
			put(io, strata::index_detail::meta_name(m.meta.generation), older);
		}, "metadata differs from the manifest");
		expect("metadata of another generation, resealed", [&](auto& fs, auto& io, auto&) {
			auto m = manifest_of(fs);
			const auto older = std::string(fs.view(strata::index_detail::meta_name(m.meta.generation - 1)));
			put(io, strata::index_detail::meta_name(m.meta.generation), older);
			m.meta.crc = strata::files::trailing_crc(older);
			reseal(fs, io, m);
		}, "metadata generation");
		expect("a manifest under a newer name", [&](auto& fs, auto& io, auto&) {
			const auto name = head(fs);
			const auto p = strata::index_detail::parse_name(name);
			put(io, strata::index_detail::manifest_name(p->a + 5), std::string(fs.view(name)));
		}, "manifest generation differs from its name");
	}
	// Documents whose entries pass a small run cap, split or tokenized, are taken: 70,000
	// occurrences of one word encode to about 70 KB, past the 64 KiB cap, so each such entry
	// takes a run alone (CR12, CR17); the writer then takes more and commits them all.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = prospect::index::create(io, keyed, uuid);
		strata::index_test_access::max_run_bytes(ix, 64 << 10);
		CHECK(ix.apply(tx({add("Qa", "a")}), {}).result == verdict::accepted);
		std::string text;
		for (int i = 0; i < 70000; ++i) { text += "w "; }
		operation big;
		big.doc.key = "Qbig";
		big.doc.text = text;
		big.doc.split_text = true;
		CHECK(ix.apply(tx({big}), {}).result == verdict::accepted);
		big.doc.key = "Qbig2";
		big.doc.split_text = false;
		for (std::uint32_t i = 0; i < 70000; ++i) { big.doc.tokens.push_back({2 * i, 1, i + 1}); }
		big.doc.length = 70000;
		CHECK(ix.apply(tx({big}), {}).result == verdict::accepted);
		big.doc.key = "Qbig3";
		big.doc.tokens.resize(1000);
		big.doc.length = 1000;
		CHECK(ix.apply(tx({big}), {}).result == verdict::accepted);
		for (int i = 0; i < 50; ++i) { CHECK(ix.apply(tx({add("Qd" + std::to_string(i), "d")}), {}).result == verdict::accepted); }
		ix.commit();
		CHECK(contents(ix).size() == 54 && ix.find_key("Qbig") && ix.find_key("Qbig2") && ix.find_key("Qd49"));
	}
	// get by key on the search layout, through the inverting buffer at one flush thread and the
	// whole-document buffer at two: buffered and flushed copies, replaces, deletes, merges, and
	// reopens, against a model.
	for (std::size_t flush_threads : {1, 2}) {
		for (std::uint64_t seed = 1; seed <= 3; ++seed) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			prospect::index::options opts;
			opts.key_prefix = "Q";
			opts.flush_threads = flush_threads;
			auto ix = prospect::index::create(io, opts, uuid);
			std::mt19937_64 rng(seed);
			std::map<std::string, std::string> model;
			auto key_of = [](std::uint64_t n) { return "Q" + std::to_string(1000 + n); };
			for (int step = 0; step < 300; ++step) {
				const std::string key = key_of(rng() % 60), value = "v" + std::to_string(step) + std::string(rng() % 40, 'x');
				switch (rng() % 8) {
					case 0: case 1: case 2: case 3:
						CHECK(ix.apply(tx({replace(key, value)}), {}).result == verdict::accepted);
						model[key] = value;
						break;
					case 4: case 5:
						CHECK(ix.apply(tx({erase(key)}), {}).result == verdict::accepted);
						model.erase(key);
						break;
					case 6: ix.refresh(); break;
					default:
						if (step % 4 == 0) { ix.force_merge(1 + rng() % 3); }
						ix.commit();
						if (step % 7 == 0) {
							ix.close();
							ix = prospect::index::open(io, opts);
						}
				}
				for (const std::string& k : {key, key_of(rng() % 60), key_of(9999)}) {
					const auto want = model.find(k);
					const auto have = ix.get(k);
					CHECK(have.has_value() == (want != model.end()));
					if (have && want != model.end()) { CHECK(*have == want->second); }
				}
			}
		}
	}
	// A search-layout index opened as the document layout fails closed: its manifest lists a
	// structure the document layout cannot read, and the open refuses it, never misreads it.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		prospect::index::options opts;
		opts.key_prefix = "Q";
		auto ix = prospect::index::create(io, opts, uuid);
		CHECK(ix.apply(tx({add("Qa", "alpha")}), {}).result == verdict::accepted);
		ix.commit();
		ix.close();
		bool refused = false;
		try {
			strata::basic_index<strata::document_layout>::options d;
			d.key_prefix = "Q";
			(void)strata::basic_index<strata::document_layout>::open(io, d);
		} catch (const strata::unsupported_index&) {
			refused = true;
		}
		CHECK(refused);
	}
	std::printf("index: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
