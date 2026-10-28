// Term lists (xapiand-on-strata P2): every document's terms read back exactly, by ordinal and
// by prefix, in order; from a flush (the writer and the index's buffer) and after merges
// (plain, reordering, with deletes); the check and its cross-check against the postings catch
// damage; the plugin is off unless asked for.
#include "prospect/check.hh"
#include "prospect/index.hh"
#include "prospect/merge.hh"
#include "prospect/term_lists.hh"
#include "prospect/writer.hh"
#include "strata/core/faultfs.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace prospect;

namespace {

using terms_t = std::vector<std::pair<std::string, std::uint32_t>>;  // sorted by term

const std::array<char, 16> uuid = {'t', 'e', 'r', 'm', '-', 'l', 'i', 's', 't', 's', '-', 't', 'e', 's', 't', '!'};

template <class F>
bool throws_corrupt(F&& f) {
	try {
		f();
	} catch (const strata::corrupt_segment&) {
		return true;
	}
	return false;
}

std::vector<std::string> vocabulary(std::mt19937_64& r) {
	static const char alphabet[] = {'a', 'b', 'c', '\0', '\x7f', '\xff', 'z'};
	static const std::string prefixes[] = {"Xa", "Xab", "Xb", "Z", "", std::string("Xa\0q", 4)};
	std::vector<std::string> v;
	for (int i = 0; i < 90; ++i) {
		std::string t = prefixes[r() % std::size(prefixes)];
		for (std::size_t n = 1 + r() % 9; n--;) { t.push_back(alphabet[r() % sizeof alphabet]); }
		if (!t.starts_with('Q')) { v.push_back(t); }
	}
	std::sort(v.begin(), v.end());
	v.erase(std::unique(v.begin(), v.end()), v.end());
	return v;
}

struct doc {
	terms_t terms;
	std::vector<std::vector<std::uint32_t>> positions;
	std::uint32_t length = 0;
};

doc make_doc(std::mt19937_64& r, const std::vector<std::string>& vocab) {
	doc d;
	std::set<std::string> chosen;
	const std::size_t n = r() % 5 == 0 ? 0 : r() % 41;
	while (chosen.size() < n && chosen.size() < vocab.size()) { chosen.insert(vocab[r() % vocab.size()]); }
	for (const auto& t : chosen) {
		const std::uint32_t wdf = std::uint32_t(r() % 6);
		d.terms.emplace_back(t, wdf);
		std::vector<std::uint32_t> pos;
		if (wdf && r() % 3) {
			std::uint32_t p = 0;
			for (std::uint32_t k = 0; k < wdf; ++k) { pos.push_back(p += 1 + std::uint32_t(r() % 3)); }
		}
		d.positions.push_back(std::move(pos));
		d.length += wdf;
	}
	return d;
}

std::vector<posting_input> inputs(const doc& d) {
	std::vector<posting_input> in;
	for (std::size_t i = 0; i < d.terms.size(); ++i) { in.push_back({d.terms[i].first, d.terms[i].second, d.positions[i]}); }
	return in;
}

terms_t read_all(term_list l) {
	terms_t out;
	for (auto c = l.begin(); c.next();) { out.emplace_back(std::string(c.term()), c.wdf()); }
	return out;
}

// One list against the model: all of it, its size, prefixes, and skip_to.
void check_list(std::mt19937_64& r, term_list l, const terms_t& want) {
	if (l.size() != want.size()) { std::fprintf(stderr, "  list has %llu terms, model %zu; bytes %zu\n", (unsigned long long)l.size(), want.size(), l.bytes().size());
		for (const auto& e : read_all(l)) { std::fprintf(stderr, "    got %zu:%u\n", e.first.size(), e.second); }
		for (const auto& e : want) { std::fprintf(stderr, "    want %zu:%u\n", e.first.size(), e.second); }
	}
	CHECK(l.size() == want.size());
	CHECK(l.empty() == want.empty());
	CHECK(read_all(l) == want);
	// A cursor past the end stays there.
	{
		auto c = l.begin();
		while (c.next()) {}
		CHECK(!c.next());
	}
	std::vector<std::string> prefixes{std::string()};
	for (int k = 0; k < 6; ++k) {
		if (!want.empty() && r() % 4) {
			const std::string& t = want[r() % want.size()].first;
			prefixes.push_back(t.substr(0, r() % (t.size() + 1)));
		} else {
			std::string p;
			for (std::size_t n = r() % 4; n--;) { p.push_back(char("abXZ\0\xff"[r() % 6])); }
			prefixes.push_back(p);
		}
	}
	for (const auto& p : prefixes) {
		terms_t expected;
		for (const auto& e : want) {
			if (e.first.starts_with(p)) { expected.push_back(e); }
		}
		terms_t got;
		for (auto c = l.prefixed(p); c.next();) { got.emplace_back(std::string(c.term()), c.wdf()); }
		CHECK(got == expected);
	}
	for (int k = 0; k < 6; ++k) {
		std::string target;
		if (!want.empty() && r() % 2) {
			target = want[r() % want.size()].first;
			if (r() % 2 && !target.empty()) { target.back() = char(target.back() + 1); }
		} else {
			for (std::size_t n = r() % 5; n--;) { target.push_back(char("abXZ\0\xff"[r() % 6])); }
		}
		auto from = std::lower_bound(want.begin(), want.end(), target, [](const auto& e, const std::string& t) { return e.first < t; });
		auto c = l.begin();
		const bool found = c.skip_to(target);
		CHECK(found == (from != want.end()));
		for (; found && from != want.end(); ++from) {
			CHECK(c.term() == from->first && c.wdf() == from->second);
			if (from + 1 != want.end()) { CHECK(c.next()); }
		}
		if (found) { CHECK(!c.next()); }
	}
}

// Every document of the segment against the model by docid.
template <class Docs>
void check_segment_lists(std::mt19937_64& r, const segment& seg, const Docs& by_docid, const std::function<bool(ordinal_t)>& live = [](ordinal_t) { return true; }) {
	CHECK(seg.holds<term_lists_plugin>());
	const auto& lists = seg.get<term_lists_plugin>();
	term_lists_plugin::reader::frame frame;
	for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
		if (!live(o)) { continue; }
		const doc& d = by_docid.at(seg.core().docid(o));
		check_list(r, lists.list(o, frame), d.terms);
	}
	prospect::check_report rep;
	prospect::check_segment(seg, nullptr, rep, "Q");
	CHECK(rep.listed_documents == seg.core().doc_count());
}

std::string flush_docs(const std::vector<std::pair<std::uint64_t, const doc*>>& docs, bool lists, strata::format::stored_codec codec = {}) {
	segment_writer w;
	if (lists) { w.structures |= strata::format::bit::term_lists; }
	if (codec.kind != strata::format::codec{}) { w.stored_codec = codec; }
	for (const auto& [docid, d] : docs) {
		const auto in = inputs(*d);
		w.add(docid, in, d->length, {});
	}
	return w.flush({});
}

// The section's file offset and length.
std::pair<std::size_t, std::size_t> section_range(const std::string& file, strata::format::section kind) {
	for (const auto& e : strata::format::decode_toc(file, strata::format::decode_footer(file))) {
		if (e.kind == kind) { return {e.offset, e.length}; }
	}
	std::abort();
}

void reseal(std::string& file) {
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

// --- the record format -------------------------------------------------------------------

std::string rec(std::initializer_list<std::tuple<std::uint64_t, std::string, std::uint64_t>> terms, std::optional<std::uint64_t> count = {}) {
	std::string out;
	strata::format::put_leb(out, count ? *count : terms.size());
	for (const auto& [shared, suffix, wdf] : terms) {
		strata::format::put_leb(out, shared);
		strata::format::put_leb(out, suffix.size());
		out += suffix;
		strata::format::put_leb(out, wdf);
	}
	return out;
}

void records() {
	// Valid: shared prefixes, a longer term after its prefix, wdf zero.
	{
		const std::string r = rec({{0, "ab", 3}, {2, "c", 0}, {1, "z", 7}, {0, std::string("\xff", 1), 4294967295ull}});
		const term_list l(r);
		CHECK(l.size() == 4);
		const terms_t want{{"ab", 3}, {"abc", 0}, {"az", 7}, {"\xff", 4294967295u}};
		CHECK(read_all(l) == want);
	}
	CHECK(term_list().empty() && term_list().size() == 0 && !term_list().begin().next());
	// Each way a record can be damaged is corruption, from the cursor.
	auto bad = [](const std::string& r) { return throws_corrupt([&] { for (auto c = term_list(r).begin(); c.next();) {} }); };
	CHECK(bad(rec({{0, "a", 1}}, 0)));                        // a count of zero is the empty record
	CHECK(bad(rec({{0, "a", 1}}, 2)));                        // more terms than bytes
	CHECK(bad(rec({{0, "a", 1}, {0, "b", 1}}, 1)));           // bytes left over
	CHECK(bad(rec({{1, "a", 1}})));                           // the first term shares
	CHECK(bad(rec({{0, "ab", 1}, {3, "c", 1}})));             // shares more than the previous term has
	CHECK(bad(rec({{0, "ab", 1}, {1, "", 1}})));              // an empty suffix
	CHECK(bad(rec({{0, "ab", 1}, {0, "ab", 1}})));            // equal terms
	CHECK(bad(rec({{0, "ab", 1}, {1, "a", 1}})));             // smaller
	CHECK(bad(rec({{0, "b", 1}, {0, "a", 1}})));              // smaller
	CHECK(bad(rec({{0, "ab", 1}, {2, "", 1}})));              // empty suffix again
	CHECK(bad(rec({{0, "a", 4294967296ull}})));               // wdf past 32 bits
	CHECK(bad(rec({{0, std::string(strata::format::max_term_bytes + 1, 'a'), 1}})));  // a term past the limit
	{
		std::string r = rec({{0, "abc", 1}});
		for (std::size_t cut = 1; cut < r.size(); ++cut) { CHECK(bad(r.substr(0, cut))); }  // every truncation
	}
	{
		std::string r = rec({{0, "abc", 1}});
		r[2] = char(r[2] + 50);  // a suffix length past the record
		CHECK(bad(r));
	}
}

// --- a flush -----------------------------------------------------------------------------

void writer_flush() {
	for (std::uint64_t seed = 0; seed < 12; ++seed) {
		std::mt19937_64 r(seed);
		const auto vocab = vocabulary(r);
		std::map<std::uint64_t, doc> by_docid;
		std::vector<std::pair<std::uint64_t, const doc*>> order;
		const std::size_t n = seed == 0 ? 0 : seed == 1 ? 1 : 1 + r() % 700;
		std::uint64_t docid = 0;
		for (std::size_t i = 0; i < n; ++i) { by_docid[docid += 1 + r() % 3] = make_doc(r, vocab); }
		for (const auto& [id, d] : by_docid) { order.emplace_back(id, &d); }
		const std::string with = flush_docs(order, true);
		const std::string without = flush_docs(order, false);
		const segment seg(with);
		if (n) {
			check_segment_lists(r, seg, by_docid);
		} else {
			CHECK(seg.holds<term_lists_plugin>() && seg.core().doc_count() == 0);
		}
		const segment plain(without);
		CHECK(!plain.holds<term_lists_plugin>());
		bool refused = false;
		try {
			term_lists_plugin::reader::frame f;
			if (n) { (void)plain.get<term_lists_plugin>().list(0, f); }
			refused = !n;
		} catch (const std::logic_error&) {
			refused = true;
		}
		CHECK(refused);
		// The lists add a section and change nothing else: the postings are the same bytes.
		for (auto kind : {strata::format::section::terms, strata::format::section::postings, strata::format::section::positions, strata::format::section::lengths}) {
			const auto a = section_range(with, kind), b = section_range(without, kind);
			CHECK(a.second == b.second && with.compare(a.first, a.second, without, b.first, b.second) == 0);
		}
	}
	// Text tokens and split text produce the terms their postings hold.
	{
		segment_writer w;
		w.structures |= strata::format::bit::term_lists;
		const std::string text = "the quick brown the fox the";
		std::vector<segment_writer::token> tokens;
		std::uint32_t pos = 0;
		for (const char* word : {"the", "quick", "brown", "the", "fox", "the"}) { tokens.push_back({word, ++pos}); }
		(void)w.add_tokens(1, tokens, pos);
		const posting_input extra[] = {{"Xid", 0, {}}};
		(void)w.add(2, extra, 0);
		(void)w.add(3, {}, 0);
		const std::string file = w.flush({});
		const segment seg(file);
		term_lists_plugin::reader::frame f;
		const auto& l = seg.get<term_lists_plugin>();
		CHECK((read_all(l.list(0, f)) == terms_t{{"brown", 1}, {"fox", 1}, {"quick", 1}, {"the", 3}}));
		CHECK((read_all(l.list(1, f)) == terms_t{{"Xid", 0}}));
		CHECK(read_all(l.list(2, f)).empty());
		bool out_of_range = false;
		try { (void)l.list(3, f); } catch (const std::out_of_range&) { out_of_range = true; }
		CHECK(out_of_range);
	}
}

// --- an index: its buffer's flush, merges, deletes -----------------------------------------

void index_and_merges() {
	for (std::uint64_t seed = 0; seed < 10; ++seed) {
		std::mt19937_64 r(seed + 100);
		const auto vocab = vocabulary(r);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		index::options opts;
		opts.key_prefix = "Q";
		opts.opt_in = strata::format::bit::term_lists;
		auto ix = prospect::index::create(io, opts, uuid);
		std::map<std::uint64_t, doc> all;      // every document ever accepted, by docid
		std::map<std::uint64_t, std::string> live;  // live docids -> key
		for (int i = 0; i < 1500; ++i) {
			transaction t;
			operation o;
			const std::uint64_t kind = r() % 10;
			const std::string key = "Q" + std::to_string(r() % 300);
			doc d = make_doc(r, vocab);
			if (kind < 6) {
				o.type = operation::replace_key;
				o.key = key;
			} else if (kind < 7) {
				o.type = operation::delete_key;
				o.key = key;
			} else if (kind < 9) {
				o.type = operation::add;
			} else {
				o.type = operation::delete_docid;
				o.docid = 1 + r() % (ix.last_docid() + 1);
			}
			if (kind < 6 || (kind >= 7 && kind < 9)) {
				for (std::size_t k = 0; k < d.terms.size(); ++k) { o.doc.terms.push_back({d.terms[k].first, d.terms[k].second, d.positions[k]}); }
				o.doc.length = d.length;
				o.doc.key = o.key;
			}
			t.ops.push_back(o);
			const auto v = ix.apply(t, {});
			if (v.result == verdict::accepted) {
				if (o.type == operation::delete_key || o.type == operation::delete_docid) {
					if (v.docids[0]) { live.erase(v.docids[0]); }
				} else {
					all[v.docids[0]] = d;
					live[v.docids[0]] = o.key;
					if (o.type == operation::replace_key) {
						for (auto it = live.begin(); it != live.end();) { it = it->first != v.docids[0] && it->second == key ? live.erase(it) : std::next(it); }
					}
				}
			}
			if (r() % 250 == 0) { ix.refresh(); }
		}
		ix.refresh();
		auto views = ix.segments();
		CHECK(views.size() > 2);
		std::vector<merge_source> sources;
		for (const auto& s : views) {
			sources.push_back({s.seg, s.deleted});
			CHECK(s.seg->holds<term_lists_plugin>());
			check_segment_lists(r, *s.seg, all, [&](ordinal_t o) { return s.live(o); });  // the buffer's flush wrote them (a replaced document's old list is not the model's)
		}
		// A merge drops the deleted documents' lists and keeps the live ones'.
		const std::array<std::uint8_t, 16> id{9};
		std::string merged;
		{
			string_output out(merged);
			string_spill spill;
			const auto plan = merge_segments(sources, id, out, spill, {.key_prefix = "Q"});
			CHECK(plan.doc_count == live.size());
		}
		const segment ms(merged);
		CHECK(ms.core().doc_count() == live.size());
		for (ordinal_t o = 0; o < ms.core().doc_count(); ++o) { CHECK(live.count(ms.core().docid(o))); }
		check_segment_lists(r, ms, all);
		// A reordering merge: an explicit shuffled order; every document keeps its list.
		std::vector<merge_ref> order;
		for (std::size_t s = 0; s < sources.size(); ++s) {
			for (ordinal_t o = 0; o < sources[s].seg->core().doc_count(); ++o) {
				if (sources[s].live(o)) { order.push_back({std::uint32_t(s), o}); }
			}
		}
		std::shuffle(order.begin(), order.end(), r);
		std::string permuted;
		{
			string_output out(permuted);
			string_spill spill;
			(void)merge_segments(sources, id, out, spill, {.order = order, .key_prefix = "Q"});
		}
		const segment ps(permuted);
		for (std::size_t i = 0; i < order.size(); ++i) { CHECK(ps.core().docid(ordinal_t(i)) == sources[order[i].source].seg->core().docid(order[i].ord)); }
		check_segment_lists(r, ps, all);
		// The same through the scratch that spills records for out-of-order merges.
		{
			std::string again;
			string_output out(again);
			string_spill spill;
			string_scratch records;
			(void)merge_segments(sources, id, out, spill, {.order = order, .records = &records, .key_prefix = "Q"});
			CHECK(again == permuted);
		}
		// The index's own merge keeps them too, and the digest does not move.
		const std::uint64_t before = ix.digest();
		sources.clear();
		views.clear();
		ix.force_merge();
		CHECK(ix.segments().size() == 1);
		CHECK(ix.digest() == before);
		{
			const auto only = ix.segments()[0];
			check_segment_lists(r, *only.seg, all, [&](ordinal_t o) { return only.live(o); });
		}
		ix.commit();
	}
}

// --- an index without them ---------------------------------------------------------------

void off_by_default() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	index::options opts;
	opts.key_prefix = "Q";
	auto ix = prospect::index::create(io, opts, uuid);
	for (int i = 0; i < 50; ++i) {
		transaction t;
		operation o;
		o.type = operation::add;
		o.doc.terms.push_back({"t" + std::to_string(i), 1, {}});
		t.ops.push_back(o);
		CHECK(ix.apply(t, {}).result == verdict::accepted);
		if (i % 20 == 19) { ix.refresh(); }
	}
	ix.refresh();
	for (const auto& s : ix.segments()) { CHECK(!s.seg->holds<term_lists_plugin>()); }
	ix.force_merge();
	CHECK(!ix.segments()[0].seg->holds<term_lists_plugin>());
	// Asking for a bit that is not opt-in changes nothing.
	faultfs::filesystem fs2;
	faultfs::io io2(fs2);
	index::options other;
	other.key_prefix = "Q";
	other.opt_in = strata::format::bit::stored | strata::format::bit::inverted;
	auto ix2 = prospect::index::create(io2, other, uuid);
	transaction t;
	operation o;
	o.type = operation::add;
	o.doc.terms.push_back({"x", 1, {}});
	t.ops.push_back(o);
	CHECK(ix2.apply(t, {}).result == verdict::accepted);
	ix2.refresh();
	CHECK(!ix2.segments()[0].seg->holds<term_lists_plugin>());
}

// --- damage ------------------------------------------------------------------------------

void damage() {
	std::mt19937_64 r(77);
	const auto vocab = vocabulary(r);
	std::map<std::uint64_t, doc> by_docid;
	std::vector<std::pair<std::uint64_t, const doc*>> order;
	for (std::uint64_t i = 1; i <= 300; ++i) { by_docid[i] = make_doc(r, vocab); }
	for (const auto& [id, d] : by_docid) { order.emplace_back(id, &d); }
	const strata::format::stored_codec raw{strata::format::codec::raw, 0};
	// Another set with the same document count, for a section that is well-formed but not these
	// documents'.
	std::map<std::uint64_t, doc> other;
	std::vector<std::pair<std::uint64_t, const doc*>> other_order;
	for (std::uint64_t i = 1; i <= 300; ++i) { other[i] = make_doc(r, vocab); }
	for (const auto& [id, d] : other) { other_order.emplace_back(id, &d); }
	for (const bool packed : {false, true}) {
		const std::string good = flush_docs(order, true, packed ? strata::format::stored_codec{} : raw);
		{
			prospect::check_report rep;
			prospect::check_segment(segment(good), nullptr, rep, "Q");
		}
		const std::string foreign = flush_docs(other_order, true, packed ? strata::format::stored_codec{} : raw);
		const auto [fo, fl] = section_range(foreign, strata::format::section::term_lists);
		const auto [go, gl] = section_range(good, strata::format::section::term_lists);
		// Mutant: another document set's lists under these postings. Every list is well-formed;
		// only the cross-check sees the disagreement.
		if (fl == gl) {
			std::string swapped = good;
			swapped.replace(go, gl, foreign, fo, fl);
			reseal(swapped);
			const segment seg(swapped);
			bool internal_ok = true;
			try {
				prospect::check_report rep;
				strata::check_segment(seg, nullptr, rep, "Q");
			} catch (const strata::corrupt_segment&) {
				internal_ok = false;
			}
			if (internal_ok) { CHECK(throws_corrupt([&] { cross_check_term_lists(seg); })); }
		}
		// Mutant: one byte changed anywhere in the section, checksums recomputed: the check
		// throws or the lists differ from the model; nothing reads out of bounds (ASan).
		std::size_t detected = 0, total = 0;
		for (std::size_t at = 0; at < gl; at += 1 + (packed ? 7 : 1)) {
			std::string bytes = good;
			bytes[go + at] = char(bytes[go + at] ^ 0x55);
			reseal(bytes);
			++total;
			try {
				const segment seg(bytes);
				prospect::check_report rep;
				prospect::check_segment(seg, nullptr, rep, "Q");
			} catch (const strata::corrupt_segment&) {
				++detected;
			}
		}
		// A changed wdf or term byte inside a raw record keeps the list well-formed; the
		// cross-check catches it. Frame headers and structure are caught by the decoders.
		CHECK(detected == total);
		std::fprintf(stderr, "  damage (%s): %zu of %zu one-byte changes detected\n", packed ? "zstd" : "raw", detected, total);
	}
}

}  // namespace

int main() {
	records();
	writer_flush();
	index_and_merges();
	off_by_default();
	damage();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("term_lists_test: ok\n");
	return 0;
}
