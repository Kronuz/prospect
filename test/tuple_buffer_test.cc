// The tuple buffer (the research note's inverter C) against segment_writer: the same
// documents give the same segment bytes, with hash collisions forced, long terms and far
// positions in side entries, terms given whole (an explicit wdf, positionless, keys), and
// documents left out and reordered at flush.
#include "strata/tuple_buffer.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <pthread.h>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

namespace {

struct entry {
	std::string term;
	std::uint32_t wdf;
	std::vector<std::uint32_t> positions;
};
struct doc {
	std::uint64_t docid = 0, version = 0;
	std::string text, stored, key;
	std::vector<strata::text_token> tokens;
	std::vector<entry> terms;
	std::uint32_t length = 0;
	std::vector<strata::posting_input> inputs() const {
		std::vector<strata::posting_input> in;
		for (const auto& e : terms) { in.push_back({e.term, e.wdf, e.positions}); }
		return in;
	}
};

doc random_doc(std::mt19937_64& r, std::uint64_t docid) {
	static const std::vector<std::string> words = {"a", "b", "the", "of", "rare", "octopus", "eightbyt", "ninebytes", std::string(254, 'm'),
		std::string(255, 'n'), std::string(300, 'l'), "x"};
	doc d;
	d.docid = docid;
	d.version = 1 + r() % 3;
	std::uint32_t pos = 0;
	for (int n = int(r() % 30); n >= 0; --n) {
		pos += 1 + std::uint32_t(r() % 3);
		if (r() % 40 == 0) { pos += std::uint32_t(1) << 24; }  // past a tuple's 24-bit position
		const std::string& w = words[r() % words.size()];
		d.text += r() % 4 ? " " : "  ";
		d.tokens.push_back({std::uint32_t(d.text.size()), std::uint32_t(w.size()), pos});
		d.text += w;
	}
	d.length = std::uint32_t(d.tokens.size());
	if (r() % 5) { d.key = "Q" + std::to_string(docid); }  // a key is the core's, never a term (F8)
	if (d.key.empty()) { d.version = 0; }                  // a version belongs to a key (F9)
	if (r() % 3 == 0) { d.terms.push_back({"T" + std::to_string(r() % 4), 1, {}}); }   // positionless, wdf 1
	if (r() % 3 == 0) { d.terms.push_back({"W" + std::to_string(r() % 4), 3, {pos + 5}}); }  // wdf apart from its count
	if (r() % 7 == 0) { d.terms.push_back({std::string(400, 'Z'), 2, {pos + 7, pos + 9}}); }  // a long term given whole
	d.stored = std::string(std::size_t(r() % 9), 's') + std::to_string(docid);
	return d;
}

std::string reference(const std::vector<doc>& docs, const std::vector<std::size_t>& order) {
	strata::segment_writer w;
	for (std::size_t i : order) {
		const doc& d = docs[i];
		const auto in = d.inputs();
		strata::detail::writer_access::add_text(w, d.docid, in, d.text, d.tokens, d.length, d.stored, d.version, d.key);
	}
	return w.flush({1, 2, 3});
}

std::string tuples(const std::vector<doc>& docs, std::span<const strata::ordinal_t> order, std::uint64_t mask, std::size_t batch = std::size_t(1) << 20,
	std::uint64_t run_cap = std::numeric_limits<std::uint32_t>::max()) {
	strata::detail::tuple_buffer b;
	b.hash_mask = mask;
	b.batch_tuples = batch;
	b.max_run_bytes = run_cap;
	for (const doc& d : docs) {
		const auto in = d.inputs();
		b.add(d.docid, in, d.text, d.tokens, d.length, d.stored, d.version, d.key);
	}
	strata::segment_writer w;
	std::string file;
	strata::string_output out(file);
	strata::string_spill positions;
	b.flush({1, 2, 3}, order, w, out, positions);
	return file;
}

std::string flushed(strata::detail::tuple_buffer& b) {
	strata::segment_writer w;
	std::string file;
	strata::string_output out(file);
	strata::string_spill positions;
	b.flush({1, 2, 3}, {}, w, out, positions);
	return file;
}

// Two terms of `size` bytes that differ only in their last, alone or among enough short
// terms that the batch sorts by radix first: the bytes of segment_writer.
bool long_ties(std::size_t size) {
	bool ok = true;
	for (int extra : {0, 400}) {
		doc d;
		d.docid = 1;
		d.terms.push_back({std::string(size - 1, 'p') + 'b', 1, {}});
		d.terms.push_back({std::string(size - 1, 'p') + 'a', 1, {}});
		for (int i = 0; i < extra; ++i) { d.terms.push_back({"t" + std::to_string(i), 1, {}}); }
		const std::vector<doc> docs = {d};
		ok = ok && tuples(docs, {}, ~std::uint64_t(0)) == reference(docs, {0});
	}
	return ok;
}

}  // namespace

int main() {
	for (std::uint64_t seed = 0; seed < 6; ++seed) {
		std::mt19937_64 r(seed);
		std::vector<doc> docs;
		for (std::uint64_t i = 0; i < 2000; ++i) { docs.push_back(random_doc(r, 10 + 3 * i)); }
		std::vector<std::size_t> all(docs.size());
		std::iota(all.begin(), all.end(), 0);
		const std::string expected = reference(docs, all);
		// Every document in order: the whole hash, then hashes narrowed to collide.
		for (std::uint64_t mask : {~std::uint64_t(0), std::uint64_t(0xFF00000000000003), std::uint64_t(0x0100000000000000), std::uint64_t(0)}) {
			CHECK(tuples(docs, {}, mask) == expected);
		}
		// Batches drained into the term store every few tuples: the same bytes.
		for (std::size_t batch : {std::size_t(1), std::size_t(7), std::size_t(500)}) {
			CHECK(tuples(docs, {}, ~std::uint64_t(0), batch) == expected);
			CHECK(tuples(docs, {}, std::uint64_t(0xFF00000000000003), batch) == expected);
		}
		// Some left out and the docids shuffled: the flush takes the rest in docid order.
		std::vector<std::uint64_t> ids(docs.size());
		for (std::size_t i = 0; i < docs.size(); ++i) { ids[i] = docs[i].docid; }
		std::shuffle(ids.begin(), ids.end(), r);
		for (std::size_t i = 0; i < docs.size(); ++i) {
			docs[i].docid = ids[i];
			if (!docs[i].key.empty()) { docs[i].key = "Q" + std::to_string(ids[i]); }
		}
		std::vector<std::size_t> live;
		for (std::size_t i = 0; i < docs.size(); ++i) {
			if (r() % 5) { live.push_back(i); }
		}
		std::sort(live.begin(), live.end(), [&](std::size_t a, std::size_t b) { return docs[a].docid < docs[b].docid; });
		std::vector<strata::ordinal_t> order(live.begin(), live.end());
		const std::string kept = reference(docs, live);
		for (std::uint64_t mask : {~std::uint64_t(0), std::uint64_t(0xFF00000000000003)}) {
			CHECK(tuples(docs, order, mask) == kept);
			CHECK(tuples(docs, order, mask, 300) == kept);
		}
		// Only some left out, the rest in arrival order (renumbered, never resorted).
		std::vector<std::size_t> in_order;
		for (std::size_t i = 0; i < docs.size(); ++i) {
			if (i % 4) { in_order.push_back(i); }
		}
		std::vector<doc> ordered = docs;
		for (std::size_t i = 0; i < ordered.size(); ++i) { ordered[i].docid = 100 + i; }
		std::vector<strata::ordinal_t> monotone(in_order.begin(), in_order.end());
		CHECK(tuples(ordered, monotone, ~std::uint64_t(0)) == reference(ordered, in_order));
	}
	// The key-range steps (a flush on several threads): ranges cut at splitters() give the
	// terms and postings each_term() gives whole, with and without documents left out.
	for (std::size_t batch : {std::size_t(1) << 20, std::size_t(300), std::size_t(1)}) {
		std::mt19937_64 r(41);
		std::vector<doc> docs;
		for (std::uint64_t i = 0; i < 1500; ++i) { docs.push_back(random_doc(r, 1 + i)); }
		strata::detail::tuple_buffer b;
		b.batch_tuples = batch;
		for (const doc& d : docs) {
			const auto in = d.inputs();
			b.add(d.docid, in, d.text, d.tokens, d.length, d.stored, d.version, d.key);
		}
		b.drain_for_flush(nullptr);
		std::vector<strata::ordinal_t> some;
		for (strata::ordinal_t o = 0; o < docs.size(); o += 3) { some.push_back(o); }
		for (const auto& order : {std::vector<strata::ordinal_t>{}, some}) {
			const auto plan = b.plan(order);
			auto collect = [&](std::string_view lo, std::string_view hi) {
				std::vector<std::string> out;
				b.each_term(lo, hi, plan, [&](std::string_view term, strata::term_postings& tp) {
					std::string row(term);
					for (std::size_t i = 0; i < tp.ords.size(); ++i) { row += ":" + std::to_string(tp.ords[i]) + "," + std::to_string(tp.wdfs[i]) + "," + std::to_string(tp.counts[i]); }
					for (auto p : tp.positions) { row += "/" + std::to_string(p); }
					out.push_back(row);
				});
				return out;
			};
			const auto whole = collect({}, {});
			CHECK(!whole.empty());
			for (std::size_t ranges : {std::size_t(2), std::size_t(7), std::size_t(64)}) {
				const auto cut = b.splitters(ranges);
				CHECK(cut.size() < ranges && std::is_sorted(cut.begin(), cut.end()));
				std::vector<std::string> joined;
				for (std::size_t k = 0; k <= cut.size(); ++k) {
					const auto part = collect(k ? cut[k - 1] : std::string_view(), k < cut.size() ? cut[k] : std::string_view());
					joined.insert(joined.end(), part.begin(), part.end());
				}
				CHECK(joined == whole);
			}
		}
	}
	// A batch whose run passes a log page (1 MiB): keys and bytes still those of segment_writer.
	{
		std::mt19937_64 r(99);
		std::vector<doc> docs;
		for (std::uint64_t i = 0; i < 40000; ++i) { docs.push_back(random_doc(r, 1 + i)); }
		std::vector<std::size_t> all(docs.size());
		std::iota(all.begin(), all.end(), 0);
		CHECK(tuples(docs, {}, ~std::uint64_t(0)) == reference(docs, all));
	}
	// add_split() splits a text at spaces into the tokens add() is given for its words.
	for (std::size_t batch : {std::size_t(1) << 20, std::size_t(37)}) {
		std::mt19937_64 r(7);
		const std::string words[] = {"a", "of", "eightbyt", "ninebytes", std::string(254, 'm'), std::string(255, 'n'), std::string(300, 'l')};
		strata::detail::tuple_buffer split, given;
		split.batch_tuples = given.batch_tuples = batch;
		for (std::uint64_t i = 0; i < 3000; ++i) {
			std::string text = r() % 3 ? "" : "  ";
			for (int w = int(r() % 50); w > 0; --w) { text += words[r() % std::size(words)] + std::string(1 + r() % 2, ' '); }
			if (r() % 2 && !text.empty()) { text.pop_back(); }
			std::vector<strata::text_token> tokens;
			std::uint32_t pos = 0;
			for (std::size_t at = 0; at < text.size();) {
				std::size_t end = text.find(' ', at);
				if (end == std::string::npos) { end = text.size(); }
				if (end > at) { tokens.push_back({std::uint32_t(at), std::uint32_t(end - at), ++pos}); }
				at = end + 1;
			}
			const std::string key = "Q" + std::to_string(i);
			CHECK(split.add_split(1 + i, {}, text, "s", 1, key) == given.add(1 + i, {}, text, tokens, pos, "s", 1, key));
			CHECK(split.length(strata::ordinal_t(i)) == pos);
		}
		auto bytes = [](strata::detail::tuple_buffer& b) {
			strata::segment_writer w;
			std::string file;
			strata::string_output out(file);
			strata::string_spill positions;
			b.flush({1, 2, 3}, {}, w, out, positions);
			return file;
		};
		CHECK(bytes(split) == bytes(given));
	}
	// scan() reports every posting with its document's positions, drained or not, and keys
	// resolve before and after their batch drains.
	for (std::size_t batch : {std::size_t(1) << 20, std::size_t(1)}) {
		strata::detail::tuple_buffer b;
		b.batch_tuples = batch;
		const std::string text = " a b a";
		const std::vector<strata::text_token> tokens = {{1, 1, 1}, {3, 1, 2}, {5, 1, 3}};
		b.add(7, {}, text, tokens, 3, "s", 1, "Qk");
		CHECK(b.key(0) == "Qk");
		b.add(8, {}, text, tokens, 3, "t", 0, "");  // drains the first when batches are 1 tuple
		CHECK(b.key(0) == "Qk" && b.key(1).empty());
		std::vector<std::string> seen;
		b.scan([&](std::string_view term, strata::ordinal_t o, std::uint32_t wdf, std::span<const std::uint32_t> positions) {
			std::string s = std::string(term) + ":" + std::to_string(o) + ":" + std::to_string(wdf);
			for (auto p : positions) { s += "," + std::to_string(p); }
			seen.push_back(s);
		});
		std::sort(seen.begin(), seen.end());
		CHECK((seen == std::vector<std::string>{"a:0:2,1,3", "a:1:2,1,3", "b:0:1,2", "b:1:1,2"}));
		CHECK(b.key(0) == "Qk" && b.stored(0) == "s" && b.docid(0) == 7 && b.version(0) == 1 && b.length(0) == 3);
	}
	// Long terms tied on all but their last byte sort without deep recursion: 1 MiB here, and
	// 64 KiB on a thread with a 512 KiB stack.
	CHECK(long_ties(std::size_t(1) << 20));
	{
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, std::size_t(512) << 10);
		bool ok = false;
		pthread_t t;
		const int rc = pthread_create(&t, &attr, [](void* p) -> void* {
			*static_cast<bool*>(p) = long_ties(std::size_t(64) << 10);
			return nullptr;
		}, &ok);
		CHECK(rc == 0);
		if (rc == 0) { pthread_join(t, nullptr); }
		pthread_attr_destroy(&attr);
		CHECK(ok);
	}
	// Collection frequency past 32 bits in one batch: two explicit wdfs of 0x90000000.
	{
		std::vector<doc> docs(2);
		for (std::size_t i = 0; i < docs.size(); ++i) {
			docs[i].docid = 1 + i;
			docs[i].terms.push_back({"x", 0x90000000u, {}});
		}
		CHECK(tuples(docs, {}, ~std::uint64_t(0)) == reference(docs, {0, 1}));
	}
	// Runs capped small (CR9): batches drain early and cut their runs between terms, and the
	// segment is the one built uncapped.
	{
		std::mt19937_64 r(5);
		std::vector<doc> docs;
		for (std::uint64_t i = 0; i < 2000; ++i) { docs.push_back(random_doc(r, 1 + i)); }
		for (std::uint64_t i = 0; i < 64; ++i) {  // 64 distinct 500-byte terms a document: a run passes the cap
			doc d;
			d.docid = 3000 + i;
			for (std::uint32_t t = 0; t < 64; ++t) { d.terms.push_back({std::string(500, 'd') + std::to_string(i * 64 + t), 1, {t + 1}}); }
			docs.push_back(d);
		}
		std::vector<std::size_t> all(docs.size());
		std::iota(all.begin(), all.end(), 0);
		const std::string whole = reference(docs, all);
		for (std::uint64_t cap : {std::uint64_t(16) << 10, std::uint64_t(64) << 10}) { CHECK(tuples(docs, {}, ~std::uint64_t(0), std::size_t(1) << 20, cap) == whole); }
	}
	// A batch past the cap is cut between terms (CR13): 500 distinct 30-byte terms encode to
	// about 20 KiB while their bound (10 KiB) stays under 16 KiB, so the next document's early
	// drain makes two runs, one without the cut.
	{
		strata::detail::tuple_buffer b, given;
		b.max_run_bytes = 16 << 10;
		std::vector<std::string> names;
		for (int i = 0; i < 500; ++i) { names.push_back(std::string(26, 'a' + i % 26) + std::to_string(1000 + i)); }
		std::vector<strata::posting_input> wide;
		for (const auto& n : names) { wide.push_back({n, 1, {}}); }
		std::vector<std::uint32_t> far(4000);
		std::iota(far.begin(), far.end(), 1u);
		const std::vector<strata::posting_input> many = {{"p", 4000, far}};
		for (auto* x : {&b, &given}) {
			x->add(1, wide, "", {}, 0, "", 0, "");
			x->add(2, many, "", {}, 0, "", 0, "");
		}
		CHECK(b.run_count() == 2 && given.run_count() == 0);
		CHECK(flushed(b) == flushed(given));
	}
	// An entry past the cap takes a run alone (CR12): a long term, many given positions, and a
	// long word flush as with no cap.
	{
		strata::detail::tuple_buffer b, given;
		b.max_run_bytes = 4096;
		const std::string big(5000, 'b');
		std::vector<std::uint32_t> far(3000);
		for (std::uint32_t i = 0; i < far.size(); ++i) { far[i] = 200 * (i + 1); }  // 2-byte deltas
		const std::vector<strata::posting_input> k = {{"k", 1, {}}}, long_term = {{big, 1, {}}}, many = {{"p", 3000, far}};
		for (auto* x : {&b, &given}) {
			x->add(1, k, "", {}, 0, "", 0, "");
			x->add(2, long_term, "", {}, 0, "", 0, "");
			x->add(3, many, "", {}, 0, "", 0, "");
			x->add_split(4, k, "a " + big, "", 0, "");
			x->add(5, k, "", {}, 0, "", 0, "");
		}
		CHECK(b.doc_count() == 5 && flushed(b) == flushed(given));
	}
	// The radix path (256 terms or more a batch) on terms of 7 to 10 bytes from {a, b} that
	// share 8-byte prefixes: wrong without its length-class pass or its ties' re-sort (CR10).
	for (std::uint64_t seed : {1, 2, 3}) {
		std::mt19937_64 r(seed);
		std::vector<std::string> words;
		for (unsigned p = 0; p < 48; ++p) {
			std::string prefix;
			for (unsigned b = 0; b < 8; ++b) { prefix += char('a' + (r() & 1)); }
			for (const char* suffix : {"", "a", "b", "aa", "ab", "ba", "bb"}) { words.push_back(prefix + suffix); }
			words.push_back(prefix.substr(0, 7));
		}
		std::sort(words.begin(), words.end());
		words.erase(std::unique(words.begin(), words.end()), words.end());
		std::vector<doc> docs(3);
		for (std::size_t i = 0; i < docs.size(); ++i) {
			std::shuffle(words.begin(), words.end(), r);
			docs[i].docid = 1 + i;
			for (const std::string& w : words) {
				docs[i].text += ' ';
				docs[i].tokens.push_back({std::uint32_t(docs[i].text.size()), std::uint32_t(w.size()), std::uint32_t(docs[i].tokens.size() + 1)});
				docs[i].text += w;
			}
			docs[i].length = std::uint32_t(docs[i].tokens.size());
		}
		CHECK(words.size() >= 256 && tuples(docs, {}, ~std::uint64_t(0)) == reference(docs, {0, 1, 2}));
	}
	std::puts(failures ? "tuple_buffer: FAILED" : "tuple_buffer: ok");
	return failures ? 1 : 0;
}
