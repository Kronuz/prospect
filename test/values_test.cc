// The values plugin (xapiand-on-strata P1): opaque byte values by slot. A segment round-trips
// every (ordinal, slot) against a model, with presence iteration and statistics; the index's
// inverter path, merges (docid order and shuffled), and deletes keep every live document's
// values and drop the deleted ones' (a merge is the same bytes as one flush of the live
// documents); the digest does not depend on segmentation; stage 1 and the writer refuse what
// the limits name; damaged directories and columns fence as corrupt_segment.
#include "prospect/check.hh"
#include "prospect/index.hh"
#include "prospect/merge.hh"
#include "view_sources.hh"
#include "strata/core/faultfs.hh"

#include <crc32c/crc32c.hh>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <atomic>
#include <thread>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace prospect;
using model_values = std::map<std::uint32_t, std::string>;

static const std::array<char, 16> uuid = {'v', 'a', 'l', 'u', 'e', 's', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i', 'd'};

// Slots of every density, including the extremes below the reserved one.
static const std::uint32_t slot_pool[] = {0, 1, 2, 3, 9, 20, 4096, 70000, 0x7FFFFFFF, 0xFFFFFFFE};

static std::string random_value(std::mt19937_64& r, std::uint32_t slot) {
	switch (slot % 4) {
	case 0: return std::string(r() % 3, char('a' + r() % 3));  // few distinct, empty among them
	case 1: return std::to_string(r() % 50);                   // short, repeated
	case 2: return std::string(1 + r() % 40, char(r()));       // arbitrary bytes, NUL and high bits
	default: return std::string(r() % 200, char('A' + r() % 26)) + std::to_string(r());  // mostly unique
	}
}

static model_values random_values(std::mt19937_64& r) {
	model_values m;
	for (const std::uint32_t slot : slot_pool) {
		const std::uint64_t density = slot == 70000 ? 1 : slot == 4096 ? 15 : slot == 0 ? 90 : 50;  // percent
		if (r() % 100 < density) { m[slot] = random_value(r, slot); }
	}
	return m;
}

static std::vector<slot_value> as_part(const model_values& m) {
	std::vector<slot_value> out;
	for (const auto& [slot, bytes] : m) { out.push_back({slot, bytes}); }
	return out;
}

// One segment's reader against its documents' model, by ordinal.
static void expect_segment(const segment& seg, const std::vector<model_values>& docs) {
	const auto& rd = seg.get<values_plugin>();
	CHECK(seg.core().doc_count() == docs.size());
	std::map<std::uint32_t, std::vector<std::pair<ordinal_t, std::string>>> by_slot;
	for (ordinal_t o = 0; o < docs.size(); ++o) {
		for (const auto& [slot, bytes] : docs[o]) { by_slot[slot].push_back({o, bytes}); }
	}
	CHECK(rd.slot_count() == by_slot.size());
	std::size_t i = 0;
	for (const auto& [slot, list] : by_slot) {
		CHECK(rd.slot_at(i) == slot);
		CHECK(rd.documents_at(i) == list.size());
		CHECK(rd.has_slot(slot));
		const auto* c = rd.get(slot);
		CHECK(c != nullptr);
		if (!c) { continue; }
		CHECK(c->documents() == list.size());
		std::set<std::string> distinct;
		for (const auto& [o, v] : list) { distinct.insert(v); }
		CHECK(c->distinct() == distinct.size());
		CHECK(c->first() == *distinct.begin() && c->last() == *distinct.rbegin());
		// Every ordinal: its value or none.
		std::size_t at = 0;
		for (ordinal_t o = 0; o < docs.size(); ++o) {
			const auto got = rd.value(o, slot);
			if (at < list.size() && list[at].first == o) {
				CHECK(got && *got == list[at].second);
				++at;
			} else {
				CHECK(!got);
			}
		}
		// Presence iteration, in order; and advance from every other target.
		auto it = c->begin();
		std::size_t n = 0;
		for (ordinal_t o = it.next(); o != no_more_docs; o = it.next(), ++n) {
			CHECK(n < list.size() && o == list[n].first && it.value() == list[n].second);
			if (n >= list.size()) { break; }
		}
		CHECK(n == list.size());
		CHECK(it.next() == no_more_docs && it.doc() == no_more_docs);
		std::mt19937_64 r(slot + 1);
		for (int k = 0; k < 20; ++k) {
			const ordinal_t target = ordinal_t(r() % (docs.size() + 3));
			auto jump = c->begin();
			const ordinal_t got = jump.advance(target);
			const auto expect = std::lower_bound(list.begin(), list.end(), target, [](const auto& e, ordinal_t t) { return e.first < t; });
			if (expect == list.end()) {
				CHECK(got == no_more_docs);
			} else {
				CHECK(got == expect->first && jump.value() == expect->second);
				// Advancing to where it stands stays; next continues.
				CHECK(jump.advance(got) == got);
				const auto after = expect + 1;
				const ordinal_t nx = jump.next();
				CHECK(after == list.end() ? nx == no_more_docs : (nx == after->first && jump.value() == after->second));
			}
		}
		++i;
	}
	CHECK(!rd.has_slot(0xFFFFFFFFu) && rd.get(12345) == nullptr && !rd.value(0, 12345));
	// A document's values, by slot.
	for (ordinal_t o = 0; o < docs.size(); o += 1 + o / 50) {
		model_values got;
		rd.each_value(o, [&](std::uint32_t slot, std::string_view v) { got[slot] = std::string(v); });
		CHECK(got == docs[o]);
	}
	check_report rep;
	check_segment(seg, nullptr, rep);
	std::uint64_t values = 0;
	for (const auto& d : docs) { values += d.size(); }
	CHECK(rep.values == values && rep.value_slots == by_slot.size());
}

static std::string write_segment(const std::vector<model_values>& docs, std::uint64_t first_docid = 1) {
	segment_writer w;
	for (std::size_t o = 0; o < docs.size(); ++o) {
		const ordinal_t ord = w.add(first_docid + o, {}, 0, "stored " + std::to_string(o));
		for (const auto& [slot, bytes] : docs[o]) { w.builder<values_plugin>().add_value(ord, slot, bytes); }
	}
	return w.flush({});
}

static void refresh_checksums(std::string& file) {
	const std::uint32_t crc = ::kronuz::crc32c::crc32c(file.data(), 12);
	std::memcpy(file.data() + 12, &crc, 4);
	const auto footer = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, footer);
	for (auto& e : toc) { e = strata::format::describe(e.kind, e.offset, std::string_view(file).substr(e.offset, e.length), e.region_shift); }
	const std::string toc_bytes = strata::format::encode_toc(toc);
	file.replace(footer.toc_offset, toc_bytes.size(), toc_bytes);
	auto f = footer;
	f.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
	const std::string footer_bytes = strata::format::encode_footer(f);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

static strata::format::section_entry values_entry(const std::string& file) {
	for (const auto& e : strata::format::decode_toc(file, strata::format::decode_footer(file))) {
		if (e.kind == strata::format::section::values) { return e; }
	}
	std::abort();
}

// Opens, reads every value, and checks: whether the damage fenced as corrupt_segment.
static bool fenced(const std::string& file) {
	try {
		const segment seg(file);
		check_report rep;
		check_segment(seg, nullptr, rep);
		const auto& rd = seg.get<values_plugin>();
		for (std::size_t i = 0; i < rd.slot_count(); ++i) { (void)rd.at(i); }
	} catch (const strata::corrupt_segment&) {
		return true;
	}
	return false;
}

// Whether reading only the directory (slot_count) or only the columns, with no check_segment and
// so no whole-section verification, fences the damage.
static bool fenced_by(const std::string& file, bool columns) {
	try {
		const segment seg(file);
		const auto& rd = seg.get<values_plugin>();
		if (!columns) { (void)rd.slot_count(); }
		for (std::size_t i = 0; columns && i < rd.slot_count(); ++i) { (void)rd.at(i); }
	} catch (const strata::corrupt_segment&) {
		return true;
	}
	return false;
}

int main() {
	// --- A segment round-trips -------------------------------------------------------------
	for (std::uint64_t seed = 0; seed < 12; ++seed) {
		std::mt19937_64 r(seed);
		std::vector<model_values> docs(seed == 0 ? 0 : seed == 1 ? 1 : 1 + r() % (seed < 6 ? 300 : 12000));
		for (auto& d : docs) { d = random_values(r); }
		const std::string file = write_segment(docs);
		const segment seg(file);
		CHECK(seg.holds<values_plugin>());
		// An open reads nothing of the section; the directory then verifies its own region, and a
		// column its own.
		CHECK(seg.core().verified_regions() == 0);
		if (docs.size() > 1) {
			const std::size_t slots = seg.get<values_plugin>().slot_count();
			const std::size_t after_directory = seg.core().verified_regions();
			CHECK(slots > 0 && after_directory >= 1);
			(void)seg.get<values_plugin>().at(0);
			CHECK(seg.core().verified_regions() >= after_directory);
		}
		expect_segment(seg, docs);
		// Written twice, the same bytes.
		CHECK(write_segment(docs) == file);
	}
	// Documents without values anywhere: an empty section, no slots.
	{
		const std::string file = write_segment(std::vector<model_values>(5));
		const segment seg(file);
		CHECK(seg.holds<values_plugin>() && seg.get<values_plugin>().slot_count() == 0);
		CHECK(values_entry(file).length == 4);
		expect_segment(seg, std::vector<model_values>(5));
	}
	// A segment configured without values has no section and answers "no values".
	{
		segment_writer w;
		w.structures &= ~strata::format::bit::values;
		w.add(1, {}, 0, "x");
		const std::string file = w.flush({});
		const segment seg(file);
		CHECK(!seg.holds<values_plugin>() && seg.get<values_plugin>().slot_count() == 0 && !seg.get<values_plugin>().value(0, 1));
		for (const auto& e : strata::format::decode_toc(file, strata::format::decode_footer(file))) { CHECK(e.kind != strata::format::section::values); }
		// Values handed to it are refused, not dropped.
		bool threw = false;
		try {
			values_plugin::refuse({{slot_value{3, "v"}}}, false);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		CHECK(threw);
	}

	// --- Stage 1 and the builder's limits ----------------------------------------------------
	{
		const strata::validate_context vc{};
		values_plugin::part p;
		CHECK(values_plugin::validate(vc, p));
		p.set(5, "b");
		p.set(2, "a");
		p.set(5, "c");  // replaces
		CHECK(p.values.size() == 2 && p.values[0].slot == 2 && p.values[1].slot == 5 && p.values[1].bytes == "c");
		CHECK(values_plugin::validate(vc, p));
		p.values.push_back({5, "dup"});
		CHECK(!values_plugin::validate(vc, p));
		p.values.back().slot = 4;  // out of order
		CHECK(!values_plugin::validate(vc, p));
		p.values.back().slot = values_format::reserved_slot;
		CHECK(!values_plugin::validate(vc, p));
		values_plugin::part big;
		big.set(1, std::string(values_format::max_value_bytes, 'x'));
		CHECK(values_plugin::validate(vc, big));
		big.set(2, "y");  // together past the limit
		CHECK(!values_plugin::validate(vc, big));
		big.values.pop_back();
		big.values[0].bytes.push_back('x');
		CHECK(!values_plugin::validate(vc, big));
		// The builder's own contract.
		values_builder b;
		const ordinal_t a = b.begin(), c = b.begin();
		b.add_value(a, 7, "x");
		auto throws = [](auto&& f) {
			try {
				f();
			} catch (const std::invalid_argument&) {
				return true;
			}
			return false;
		};
		CHECK(throws([&] { b.add_value(a, 7, "again"); }));                     // once per slot and document
		CHECK(throws([&] { b.add_value(a, values_format::reserved_slot, "r"); }));
		CHECK(throws([&] { b.add_value(5, 1, "no such document"); }));
		b.add_value(c, 7, "z");
		CHECK(throws([&] { b.add_value(a, 7, "backwards"); }));
	}

	// --- The index: buffer, flush, replacement, delete, merge -------------------------------
	for (std::uint64_t seed = 0; seed < 8; ++seed) {
		std::mt19937_64 r(seed + 100);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options opts;
		opts.key_prefix = "Q";
		auto ix = prospect::index::create(io, opts, uuid);
		struct live {
			model_values values;
			std::string key;
			std::uint64_t version;
		};
		std::map<std::uint64_t, live> model;
		auto make = [&](const std::string& key, model_values& m) {
			document d;
			d.key = key;
			m = r() % 6 ? random_values(r) : model_values{};
			d.values = as_part(m);
			d.field().terms.push_back({"t" + std::to_string(r() % 5), 1, {1}});
			d.length = 1;
			if (r() % 3 == 0) { d.stored = std::string(r() % 300, 's'); }
			return d;
		};
		for (int i = 0; i < 2500; ++i) {
			transaction t;
			operation o;
			const std::uint64_t kind = r() % 10;
			const std::string key = "Q" + std::to_string(r() % 300);
			model_values values;
			if (kind < 6) {
				o.type = operation::replace_key;
				o.key = key;
				o.doc = make(key, values);
			} else if (kind < 7) {
				o.type = operation::delete_key;
				o.key = key;
			} else if (kind < 9) {
				o.type = operation::add;
				o.doc = make("", values);
			} else {
				o.type = operation::delete_docid;
				o.docid = 1 + r() % (ix.last_docid() + 1);
			}
			t.ops.push_back(o);
			const auto v = ix.apply(t, {});
			if (v.result == verdict::accepted) {
				if (o.type == operation::delete_key || o.type == operation::delete_docid) {
					if (v.docids[0]) { model.erase(v.docids[0]); }
				} else {
					model[v.docids[0]] = {values, o.type == operation::replace_key ? key : std::string(), v.versions[0]};
				}
			}
			if (r() % 250 == 0) { ix.refresh(); }
		}
		// Buffered documents digest as they will once flushed.
		const std::uint64_t before_refresh = ix.digest();
		ix.refresh();
		CHECK(ix.digest() == before_refresh);
		auto expect_index = [&] {
			std::size_t seen = 0;
			for (const auto& s : ix.segments()) {
				const auto& rd = s.seg()->get<values_plugin>();
				for (ordinal_t o = 0; o < s.seg()->core().doc_count(); ++o) {
					if (!s.live(o)) { continue; }
					++seen;
					const auto it = model.find(s.seg()->core().docid(o));
					CHECK(it != model.end());
					if (it == model.end()) { continue; }
					model_values got;
					rd.each_value(o, [&](std::uint32_t slot, std::string_view v) { got[slot] = std::string(v); });
					CHECK(got == it->second.values);
				}
			}
			CHECK(seen == model.size());
		};
		expect_index();
		CHECK(ix.segments().size() > 3);
		// A merge of every segment: the bytes of one flush of the live documents.
		view_sources held(ix.segments());
		auto& sources = held.sources;
		const std::array<std::uint8_t, 16> id{9};
		auto flush_direct = [&](const std::vector<std::uint64_t>& docids) {
			segment_writer w;
			std::vector<posting_input> none;
			for (const std::uint64_t docid : docids) {
				const auto& l = model.at(docid);
				// The same document the index flushed: its terms and stored come from its segment.
				const ordinal_t ord = w.add(docid, none, 0, {}, l.version, l.key);
				for (const auto& [slot, bytes] : l.values) { w.builder<values_plugin>().add_value(ord, slot, bytes); }
			}
			return w.flush(id);
		};
		std::string merged;
		{
			string_output out(merged);
			string_spill spill;
			const auto plan = merge_segments(sources, id, out, spill, {.key_prefix = "Q"});
			CHECK(plan.doc_count == model.size());
		}
		{
			// The merged values section equals the direct flush's: whole files differ in terms and
			// stored, so compare the section.
			std::vector<std::uint64_t> docids;
			for (const auto& [d, l] : model) { docids.push_back(d); }
			const std::string direct = flush_direct(docids);
			const auto a = values_entry(merged), b = values_entry(direct);
			CHECK(a.length == b.length && std::string_view(merged).substr(a.offset, a.length) == std::string_view(direct).substr(b.offset, b.length));
			// Deleted documents' values are gone: no value only they held survives.
			const segment seg(merged);
			std::set<std::string> kept;
			for (const auto& [d, l] : model) {
				for (const auto& [slot, v] : l.values) { kept.insert(std::to_string(slot) + ":" + v); }
			}
			const auto& rd = seg.get<values_plugin>();
			for (std::size_t i = 0; i < rd.slot_count(); ++i) {
				const auto& c = rd.at(i);
				for (auto it = c.begin(); it.next() != no_more_docs;) { CHECK(kept.count(std::to_string(rd.slot_at(i)) + ":" + std::string(it.value()))); }
				// The dictionary holds nothing but live values.
				std::set<std::string> live_values;
				for (const auto& [d, l] : model) {
					if (const auto f = l.values.find(rd.slot_at(i)); f != l.values.end()) { live_values.insert(f->second); }
				}
				CHECK(c.distinct() == live_values.size());
			}
		}
		// A shuffled order: ordinals follow it.
		std::vector<merge_ref> order;
		for (std::size_t s = 0; s < sources.size(); ++s) {
			for (ordinal_t o = 0; o < sources[s].seg->core().doc_count(); ++o) {
				if (sources[s].live(o)) { order.push_back({std::uint32_t(s), o}); }
			}
		}
		std::shuffle(order.begin(), order.end(), r);
		{
			std::string permuted;
			string_output out(permuted);
			string_spill spill;
			(void)merge_segments(sources, id, out, spill, {.order = order, .key_prefix = "Q"});
			const segment seg(permuted);
			const auto& rd = seg.get<values_plugin>();
			for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
				model_values got;
				rd.each_value(o, [&](std::uint32_t slot, std::string_view v) { got[slot] = std::string(v); });
				CHECK(got == model.at(seg.core().docid(o)).values);
			}
			check_report rep;
			check_segment(seg, nullptr, rep, "Q");
		}
		// The digest is the same through merging to one segment.
		const std::uint64_t before = ix.digest();
		sources.clear();
		ix.force_merge();
		CHECK(ix.segments().size() == 1);
		CHECK(ix.digest() == before);
		expect_index();
	}

	// --- Damage fences ------------------------------------------------------------------------
	{
		std::mt19937_64 r(77);
		std::vector<model_values> docs(700);
		for (auto& d : docs) { d = random_values(r); }
		const std::string base = write_segment(docs);
		CHECK(!fenced(base));
		const auto e = values_entry(base);
		const std::uint32_t n = strata::format::get<std::uint32_t>(base.data() + e.offset + e.length - 4);
		const std::uint64_t dir = e.length - 4 - std::uint64_t(n) * values_format::entry_bytes;
		auto mutate = [&](std::uint64_t at, auto f) {
			std::string file = base;
			f(file.data() + e.offset + at);
			refresh_checksums(file);
			return file;
		};
		auto entry_at = [&](std::uint32_t i) { return dir + std::uint64_t(i) * values_format::entry_bytes; };
		// A count the section cannot hold; the checksum catches damage without resealing too.
		{
			std::string file = base;
			file[e.offset + e.length - 1] ^= 0x40;
			CHECK(fenced(file));  // checksum
			CHECK(fenced(mutate(e.length - 4, [](char* p) { std::uint32_t v = 0xFFFFFFFFu; std::memcpy(p, &v, 4); })));
			CHECK(fenced(mutate(e.length - 4, [&](char* p) { std::uint32_t v = n + 1; std::memcpy(p, &v, 4); })));
			CHECK(fenced(mutate(e.length - 4, [&](char* p) { std::uint32_t v = n - 1; std::memcpy(p, &v, 4); })));
		}
		// A changed byte inside a column, checksums left alone: the column's region fails.
		for (std::uint64_t at = 3; at < dir; at += 1 + dir / 25) {
			std::string file = base;
			file[e.offset + at] ^= 0x21;
			CHECK(fenced(file));
		}
		// A change that keeps the column well-formed, found by the first read of a column alone
		// (no whole-section check): one value's last byte, left in order.
		{
			// A second slot's bulk puts the directory in a later region than the first column.
			std::vector<model_values> docs(300);
			docs[0] = {{4, "aaaa"}};
			docs[1] = {{4, "bbbb"}};
			docs[2] = {{4, "cccc"}};
			for (std::size_t d = 0; d < docs.size(); ++d) { docs[d][9] = std::string(100, char('0' + d % 10)) + std::to_string(d); }
			std::string file = write_segment(docs);
			CHECK(!fenced_by(file, true) && !fenced_by(file, false));
			const std::size_t at = file.find("bbbb");
			CHECK(at != std::string::npos);
			file[at + 3] = 'c';  // "bbbc": still between "aaaa" and "cccc"
			CHECK(fenced_by(file, true));
			// A failed first read fails every read the same way (TSan's pthread_once hung on a
			// retry after a throw).
			const segment seg(file);
			for (int again = 0; again < 3; ++again) {
				bool threw = false;
				try {
					(void)seg.get<values_plugin>().at(0);
				} catch (const strata::corrupt_segment&) {
					threw = true;
				}
				CHECK(threw);
			}
		}
		// Bytes between the last column and the directory, or a last column short of them:
		// the directory fences on its own, with no column opened.
		CHECK(fenced_by(mutate(entry_at(n - 1) + 24, [](char* p) { std::uint64_t v; std::memcpy(&v, p, 8); --v; std::memcpy(p, &v, 8); }), false));
		// Each directory field.
		CHECK(n >= 3);
		for (std::uint32_t i : {0u, 1u, n - 1}) {
			const auto at = entry_at(i);
			CHECK(fenced(mutate(at + 4, [](char* p) { *p = 2; })));    // kind
			CHECK(fenced(mutate(at + 5, [](char* p) { *p = 1; })));    // flags
			CHECK(fenced(mutate(at + 6, [](char* p) { *p = 1; })));    // reserved
			CHECK(fenced(mutate(at + 12, [](char* p) { *p = 1; })));   // reserved
			CHECK(fenced(mutate(at + 8, [](char* p) { std::uint32_t v = 0; std::memcpy(p, &v, 4); })));            // no documents
			CHECK(fenced(mutate(at + 8, [](char* p) { std::uint32_t v = 0xFFFFFFFFu; std::memcpy(p, &v, 4); })));  // more than the segment's
			CHECK(fenced(mutate(at + 8, [](char* p) { std::uint32_t v; std::memcpy(&v, p, 4); ++v; std::memcpy(p, &v, 4); })));  // not the column's
			CHECK(fenced(mutate(at + 16, [](char* p) { std::uint64_t v; std::memcpy(&v, p, 8); ++v; std::memcpy(p, &v, 8); })));
			CHECK(fenced(mutate(at + 24, [](char* p) { std::uint64_t v = 0; std::memcpy(p, &v, 8); })));
			CHECK(fenced(mutate(at + 24, [](char* p) { std::uint64_t v = ~std::uint64_t(0); std::memcpy(p, &v, 8); })));
			CHECK(fenced(mutate(at + 24, [](char* p) { std::uint64_t v; std::memcpy(&v, p, 8); --v; std::memcpy(p, &v, 8); })));
		}
		// Slots out of order and the reserved one.
		CHECK(fenced(mutate(entry_at(1), [&](char* p) { std::memcpy(p, base.data() + e.offset + entry_at(0), 4); })));
		CHECK(fenced(mutate(entry_at(n - 1), [](char* p) { std::uint32_t v = 0xFFFFFFFFu; std::memcpy(p, &v, 4); })));
		// Column bytes: the header's version, kind, and flags; then each byte of every column
		// moved by one, which must fence or leave a column that still reads.
		for (const std::uint64_t at : {0u, 1u, 2u}) {
			CHECK(fenced(mutate(at, [](char* p) { *p = char(*p ^ 0x10); })));
		}
		CHECK(fenced(mutate(2, [](char* p) { *p = char(*p | 1); })));  // multi-valued
		for (std::uint64_t at = 0; at < dir; at += 1 + at / 40) {
			const std::string file = mutate(at, [](char* p) { *p = char(*p ^ 0x5A); });
			(void)fenced(file);  // fences or reads; must not crash (ASan/UBSan judge)
		}
	}

	// --- Readers from several threads open a column once ------------------------------------
	{
		std::mt19937_64 r(5);
		std::vector<model_values> docs(2000);
		for (auto& d : docs) { d = random_values(r); }
		const std::string file = write_segment(docs);
		const segment seg(file);
		std::vector<std::thread> threads;
		std::atomic<int> bad{0};
		for (int t = 0; t < 4; ++t) {
			threads.emplace_back([&, t] {
				std::mt19937_64 q(t);
				for (int k = 0; k < 20000; ++k) {
					const ordinal_t o = ordinal_t(q() % docs.size());
					const std::uint32_t slot = slot_pool[q() % std::size(slot_pool)];
					const auto got = seg.get<values_plugin>().value(o, slot);
					const auto it = docs[o].find(slot);
					if (it == docs[o].end() ? got.has_value() : (!got || *got != it->second)) { ++bad; }
				}
			});
		}
		for (auto& t : threads) { t.join(); }
		CHECK(bad == 0);
	}
	std::printf(failures ? "values: FAILED\n" : "values: ok\n");
	return failures ? 1 : 0;
}
