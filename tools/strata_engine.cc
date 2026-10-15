// strata as a search-benchmark-game engine.
//   strata_engine index DIR < corpus.json   one segment from JSON lines ("text": whitespace tokens)
//   strata_engine serve DIR                  "COMMAND\tquery" lines -> one count per line
//   strata_engine terms DIR                  every term and its document frequency
//   strata_engine dictstats DIR              the term dictionary's bytes by part
//   strata_engine check DIR                  every byte verified and every structure decoded
//   strata_engine verify DIR < queries.txt   pruned top-k equals an exhaustive scan, k = 10, 100, 1000,
//                                            ordinals and scores, Lucene and Xapian BM25
//   strata_engine bench DIR COMMAND RUNS [bm25_xapian] < queries.txt
//                                            per query: fastest of RUNS in-process runs, in µs
//   strata_engine churn DIR < ops            the soak's op stream through strata::index (bench/soak):
//                                            "P\tid\ttext" upserts, "D\tid" deletes, "C" commits and
//                                            prints "C\tµs\tsegments", "Q\tCOMMAND\tquery" prints
//                                            "Q\tµs\tresult"
// Query syntax: words are optional, +word is required, -word is excluded, "a b" is a phrase.
#include "completion/posix.h"
#include "residency.hh"
#include "strata/index.hh"
#include "strata/check.hh"
#include "strata/search.hh"
#include "strata/searcher.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <charconv>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <string>

namespace {

std::string_view text_field(std::string_view line) {
	auto at = line.find("\"text\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 9;
	return line.substr(at, line.find('"', at) - at);
}

// The "id" field, stored like the baseline engines' stored id.
std::string_view id_field(std::string_view line) {
	auto at = line.find("\"id\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 7;
	return line.substr(at, line.find('"', at) - at);
}

// Lines from stdin in reads of up to 1 MiB: macOS's libc++ reads std::cin a character at a
// time through getc, locking stdin for each, whatever sync_with_stdio says. read(2) returns
// what is there, so an interactive client gets its answer.
class LineReader {
public:
	bool next(std::string& line) {
		line.clear();
		for (;;) {
			if (at_ == end_) {
				ssize_t count;
				do { count = ::read(0, buffer_.data(), buffer_.size()); } while (count < 0 && errno == EINTR);
				end_ = count > 0 ? static_cast<std::size_t>(count) : 0; at_ = 0;
				if (!end_) { return !line.empty(); }
			}
			auto* start = buffer_.data() + at_;
			auto* newline = static_cast<char*>(std::memchr(start, '\n', end_ - at_));
			if (newline) { line.append(start, newline); at_ = static_cast<std::size_t>(newline - buffer_.data()) + 1; return true; }
			line.append(start, buffer_.data() + end_); at_ = end_;
		}
	}
private:
	std::array<char, 1 << 20> buffer_{}; std::size_t at_ = 0, end_ = 0;
};

strata::document tokens_document(std::string_view id, std::string_view text) {
	std::unordered_map<std::string_view, std::vector<std::uint32_t>> at;
	std::uint32_t pos = 0;
	for (std::size_t i = 0; i < text.size();) {
		std::size_t j = text.find(' ', i);
		if (j == std::string_view::npos) { j = text.size(); }
		if (j > i) { at[text.substr(i, j - i)].push_back(++pos); }
		i = j + 1;
	}
	strata::document d;
	d.terms.push_back({"Q" + std::string(id), 0, {}});
	for (auto& [t, positions] : at) { d.terms.push_back({std::string(t), std::uint32_t(positions.size()), std::move(positions)}); }
	d.length = pos;
	d.stored = std::string(id);
	return d;
}

// Phase 3's index: each document upserted by its key ("Q" + id, Xapiand's key prefix),
// the buffer flushing past STRATA_BUFFER_MB (default 256) and merging by the tiered policy
// (STRATA_AUTO_MERGE=0 turns merges off), committed once at the end. Segments, versions,
// keys, and merges cost what Phase 3's gate measures (T4).
int index(const std::string& dir) {
	std::ios::sync_with_stdio(false);
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	const char* mb = std::getenv("STRATA_BUFFER_MB");
	o.buffer_share = (mb ? std::strtoull(mb, nullptr, 10) : 256) << 20;
	const char* merge = std::getenv("STRATA_AUTO_MERGE");
	o.auto_merge = !merge || std::string_view(merge) != "0";
	const std::array<char, 16> uuid{'s', 't', 'r', 'a', 't', 'a', '-', 'b', 'e', 'n', 'c', 'h'};
	auto ix = strata::index::create(io, o, uuid);
	strata::transaction t;
	auto apply = [&] {
		if (t.ops.empty()) { return; }
		if (ix.apply(t, {}).result != strata::verdict::accepted) { std::cerr << "rejected\n"; std::exit(1); }
		t.ops.clear();
	};
	std::string line;
	std::uint64_t n = 0;
	for (LineReader in; in.next(line);) {
		if (line.empty()) { continue; }
		strata::operation op;
		op.type = strata::operation::replace_key;
		const auto id = id_field(line);
		op.key = "Q" + std::string(id);
		op.doc = tokens_document(id, text_field(line));
		t.ops.push_back(std::move(op));
		if (t.ops.size() == 1000) { apply(); }
		if (++n % 100000 == 0) { std::cerr << n << '\n'; }
	}
	apply();
	ix.commit();
	std::cerr << "index: " << n << " documents, " << ix.segments().size() << " segments\n";
	return 0;
}

// Merges every segment into one and commits: the "after merging" index of T8.
int merge(const std::string& dir) {
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	auto ix = strata::index::open(io, o);
	ix.force_merge(1);
	ix.commit();
	std::cerr << "merged: " << ix.segments().size() << " segments\n";
	return 0;
}

struct parsed {
	std::vector<std::vector<std::string>> must, should, must_not;  // each clause: a term or a phrase
};

parsed parse(std::string_view q) {
	parsed out;
	for (std::size_t i = 0; i < q.size();) {
		if (q[i] == ' ') { ++i; continue; }
		char op = 0;
		if (q[i] == '+' || q[i] == '-') { op = q[i++]; }
		std::vector<std::string> clause;
		if (i < q.size() && q[i] == '"') {
			std::size_t end = q.find('"', i + 1);
			if (end == std::string_view::npos) { end = q.size(); }
			std::string_view phrase = q.substr(i + 1, end - i - 1);
			for (std::size_t a = 0; a < phrase.size();) {
				std::size_t b = phrase.find(' ', a);
				if (b == std::string_view::npos) { b = phrase.size(); }
				if (b > a) { clause.emplace_back(phrase.substr(a, b - a)); }
				a = b + 1;
			}
			i = end + 1;
		} else {
			std::size_t end = q.find(' ', i);
			if (end == std::string_view::npos) { end = q.size(); }
			clause.emplace_back(q.substr(i, end - i));
			i = end;
		}
		if (clause.empty()) { continue; }
		(op == '+' ? out.must : op == '-' ? out.must_not : out.should).push_back(std::move(clause));
	}
	return out;
}

// A multi-segment search's statistics: the snapshot's totals and each term's document
// frequency over every segment (D13), cached per query.
struct snapshot_stats {
	std::span<const strata::segment_snapshot> segs;
	strata::statistics st;
	std::unordered_map<std::string, std::uint64_t> df;
	explicit snapshot_stats(std::span<const strata::segment_snapshot> s) : segs(s), st(strata::snapshot_statistics(s)) {}
	std::uint64_t of(const std::string& term) {
		auto [it, fresh] = df.try_emplace(term, 0);
		if (fresh) { it->second = strata::snapshot_df(segs, term); }
		return it->second;
	}
};

// A clause's scorer, or null when one of its terms is absent (the clause matches nothing).
// boost is how many times the clause repeats in its group (Xapian's wqf). With snapshot
// statistics, weights are the index's, not the segment's.
strata::scorer_ptr clause_scorer(const strata::segment& seg, const std::vector<std::string>& clause, double boost, strata::scheme scheme, snapshot_stats* snap = nullptr) {
	std::vector<strata::format::term_info> infos;
	for (const auto& t : clause) {
		auto info = seg.term(t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
	}
	const bool xapian = scheme == strata::scheme::bm25_xapian;
	if (infos.size() == 1) {
		if (snap) {
			const auto df = snap->of(clause[0]);
			return std::make_unique<strata::term_scorer>(seg, infos[0], xapian ? strata::weight::bm25_xapian(snap->st, df, std::uint32_t(boost)) : strata::weight::bm25(snap->st, df, boost));
		}
		if (xapian) { return std::make_unique<strata::term_scorer>(seg, infos[0], strata::weight::bm25_xapian(seg, infos[0].df, std::uint32_t(boost))); }
		return std::make_unique<strata::term_scorer>(seg, infos[0], boost);
	}
	if (!xapian) {
		if (!snap) { return std::make_unique<strata::phrase_scorer>(seg, infos); }
		std::vector<std::uint64_t> dfs;
		for (const auto& t : clause) { dfs.push_back(snap->of(t)); }
		return std::make_unique<strata::phrase_scorer>(seg, infos, std::vector<strata::weight>{}, 0, &snap->st, dfs);
	}
	std::vector<strata::weight> weights;
	for (std::size_t k = 0; k < infos.size(); ++k) {
		weights.push_back(snap ? strata::weight::bm25_xapian(snap->st, snap->of(clause[k])) : strata::weight::bm25_xapian(seg, infos[k].df));
	}
	return std::make_unique<strata::phrase_scorer>(seg, infos, std::move(weights));
}

// Repeated clauses in a group merge into one with the repeat count as boost, as Lucene's
// BooleanQuery rewrite does; BM25 is linear in the boost, so scores are unchanged. Phrases
// keep their repeats.
std::vector<std::pair<std::vector<std::string>, double>> merge_repeats(const std::vector<std::vector<std::string>>& clauses) {
	std::vector<std::pair<std::vector<std::string>, double>> out;
	for (const auto& c : clauses) {
		auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.first == c; });
		if (it != out.end() && c.size() == 1) {
			it->second += 1.0;
		} else {
			out.push_back({c, 1.0});
		}
	}
	return out;
}

strata::scorer_ptr build(const strata::segment& seg, const parsed& q, strata::scheme scheme = strata::scheme::bm25, snapshot_stats* snap = nullptr) {
	std::vector<strata::scorer_ptr> must, should, must_not;
	for (const auto& [c, boost] : merge_repeats(q.must)) {
		auto s = clause_scorer(seg, c, boost, scheme, snap);
		if (!s) { return nullptr; }
		must.push_back(std::move(s));
	}
	for (const auto& [c, boost] : merge_repeats(q.should)) {
		if (auto s = clause_scorer(seg, c, boost, scheme, snap)) { should.push_back(std::move(s)); }
	}
	for (const auto& c : q.must_not) {
		if (auto s = clause_scorer(seg, c, 1.0, scheme, snap)) { must_not.push_back(std::move(s)); }
	}
	auto combine = [](std::vector<strata::scorer_ptr> v, bool all) -> strata::scorer_ptr {
		if (v.empty()) { return nullptr; }
		if (v.size() == 1) { return std::move(v[0]); }
		if (all) { return std::make_unique<strata::conjunction_scorer>(std::move(v)); }
		return std::make_unique<strata::disjunction_scorer>(std::move(v));
	};
	strata::scorer_ptr req = combine(std::move(must), true), opt = combine(std::move(should), false);
	strata::scorer_ptr result;
	if (req && opt) {
		result = std::make_unique<strata::required_optional_scorer>(std::move(req), std::move(opt));
	} else {
		result = req ? std::move(req) : std::move(opt);
	}
	// A - (B or C) as (A - B) - C: each exclusion checks one clause, without a union.
	for (auto& ex : must_not) {
		if (result) { result = std::make_unique<strata::exclusion_scorer>(std::move(result), std::move(ex)); }
	}
	return result;
}

std::uint64_t run(const strata::segment& seg, std::string_view command, std::string_view query, strata::scheme scheme = strata::scheme::bm25) {
	const parsed q = parse(query);
	const bool counting = command == "COUNT" || command.ends_with("_COUNT");
	// A single term's count is its document frequency (one segment, no deletes).
	if (command == "COUNT" && q.must.size() + q.should.size() == 1 && q.must_not.empty() && (q.must.empty() ? q.should[0] : q.must[0]).size() == 1) {
		auto info = seg.term((q.must.empty() ? q.should[0] : q.must[0])[0]);
		return info ? info->df : 0;
	}
	auto s = build(seg, q, scheme);
	if (!s) { return counting ? 0 : 1; }
	if (command == "COUNT") { return strata::count(*s); }
	std::size_t k = 0;
	std::from_chars(command.data() + 4, command.data() + command.size(), k);
	std::uint64_t n = 0;
	strata::top_k(*s, k, counting ? &n : nullptr);
	return counting ? n : 1;
}

strata::segment open(const std::string& dir, residency::file& file) {
	file = residency::file::open(dir + "/segment.sx");
	auto bytes = file.view({0, file.size()});
	return strata::segment(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// A query over an index's segments, with the snapshot's statistics.
std::uint64_t run(std::span<const strata::segment_snapshot> segs, std::string_view command, std::string_view query, strata::scheme scheme = strata::scheme::bm25) {
	const parsed q = parse(query);
	snapshot_stats snap(segs);
	auto builder = [&](const strata::segment& seg) { return build(seg, q, scheme, &snap); };
	if (command == "COUNT") { return strata::search_count(segs, builder); }
	std::size_t k = 0;
	std::from_chars(command.data() + 4, command.data() + command.size(), k);
	std::uint64_t n = 0;
	(void)strata::search(segs, builder, k, command.ends_with("_COUNT") ? &n : nullptr);
	return command.ends_with("_COUNT") ? n : 1;
}

int serve(const std::string& dir) {
	std::string line;
	std::cout.setf(std::ios::unitbuf);
	if (!std::filesystem::exists(dir + "/segment.sx")) {  // an index of segments (Phase 3)
		kronuz::journal::PosixIO io(dir);
		strata::index::options o;
		o.key_prefix = "Q";
		auto ix = strata::index::open(io, o);
		std::vector<strata::segment_snapshot> segs;
		for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted}); }
		for (LineReader in; in.next(line);) {
			const std::size_t tab = line.find('\t');
			std::cout << run(std::span<const strata::segment_snapshot>(segs), std::string_view(line.data(), tab), std::string_view(line).substr(tab + 1)) << '\n';
		}
		return 0;
	}
	residency::file file;
	strata::segment seg = open(dir, file);
	for (LineReader in; in.next(line);) {
		const std::size_t tab = line.find('\t');
		std::cout << run(seg, std::string_view(line.data(), tab), std::string_view(line).substr(tab + 1)) << '\n';
	}
	return 0;
}

int terms(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::ios::sync_with_stdio(false);
	seg.scan_terms([](std::string_view key, const strata::format::term_info& info) {
		std::cout << key << '\t' << info.df << '\n';
		return true;
	});
	return 0;
}

// The term dictionary's bytes by part: raw keys and each encoded term-info field, against
// the terms and term_index sections; the difference is the dictionary's own structure
// (negative when prefix sharing saves more than its block headers cost).
int dictstats(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	auto leb = [](std::uint64_t v) { std::uint64_t n = 1; while (v >= 128) { v >>= 7; ++n; } return n; };
	std::uint64_t terms = 0, single = 0, absolute = 0, keys = 0, values = 0, flags = 0, df = 0, cf = 0, ordinal = 0, impacts = 0;
	std::uint64_t postings_length = 0, postings_offset = 0, positions_length = 0, positions_offset = 0;
	strata::format::term_run run;
	seg.dictionary().scan("", [&](std::string_view key, std::string_view value) {
		const auto t = strata::format::decode_term_info(value, run);
		const bool abs = t.flags & strata::format::absolute;
		// The cursors an absolute entry stored: where its own bytes start.
		strata::format::term_run run_before;
		run_before.postings = (t.flags & strata::format::single_posting) ? run.postings : t.postings;
		run_before.positions = (t.flags & strata::format::no_positions) ? run.positions : t.positions;
		++terms;
		absolute += abs;
		keys += key.size();
		values += value.size();
		df += leb(std::uint64_t(t.df) << 5);
		if (!(t.flags & strata::format::cf_is_df)) { cf += leb(t.flags & strata::format::single_posting ? t.single_wdf : t.cf); }
		if (abs) {
			postings_offset += leb(run_before.postings);
			positions_offset += leb(run_before.positions);
		}
		if (t.flags & strata::format::single_posting) {
			++single;
			ordinal += leb(t.postings);
		} else {
			postings_length += leb(t.postings_length);
		}
		if (!(t.flags & strata::format::no_positions)) { positions_length += leb(t.positions_length); }
		if (t.df >= strata::format::block) {
			std::string imp;
			strata::format::encode_impacts(imp, t.impacts);
			impacts += imp.size();
		}
		return true;
	});
	std::uint64_t section_terms = 0, section_index = 0;
	for (const auto& e : seg.sections()) {
		if (e.kind == strata::format::section::terms) { section_terms = e.length; }
		if (e.kind == strata::format::section::term_index) { section_index = e.length; }
	}
	auto row = [&](const char* name, std::uint64_t bytes) { std::printf("%-36s %14llu %8.2f B/term\n", name, (unsigned long long)bytes, double(bytes) / double(terms)); };
	std::printf("terms %llu, single-posting %llu, absolute offsets %llu\n", (unsigned long long)terms, (unsigned long long)single, (unsigned long long)absolute);
	row("keys (raw)", keys);
	row("value: df and flags", df);
	row("value: cf, or a single's wdf, unless 1", cf);
	row("value: single ordinal", ordinal);
	row("value: postings length", postings_length);
	row("value: postings cursor (absolute)", postings_offset);
	row("value: positions length", positions_length);
	row("value: positions cursor (absolute)", positions_offset);
	row("value: competitive pairs (df >= 128)", impacts);
	row("values total", values);
	row("terms section", section_terms);
	row("term_index section", section_index);
	std::printf("%-36s %14lld %8.2f B/term\n", "structure (terms - keys - values)", (long long)section_terms - (long long)(keys + values), (double(section_terms) - double(keys + values)) / double(terms));
	return values == flags + df + cf + ordinal + postings_length + postings_offset + positions_length + positions_offset + impacts ? 0 : 1;
}

// Dictionary experiments without reindexing: dictdump writes a segment's terms and
// infos to a file with this binary's reader; dictsim rebuilds the dictionary from that
// file with the termdict and term-info encoding the (possibly different) binary was built
// with, and prints its size.
int dictdump(const std::string& dir, const std::string& path) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::FILE* out = std::fopen(path.c_str(), "wb");
	if (!out) { return 1; }
	auto u64 = [&](std::uint64_t v) { std::fwrite(&v, 8, 1, out); };
	seg.scan_terms([&](std::string_view key, const strata::format::term_info& t) {
		u64(key.size());
		std::fwrite(key.data(), 1, key.size(), out);
		for (std::uint64_t v : {std::uint64_t(t.flags), std::uint64_t(t.df), t.cf, t.postings, t.positions, std::uint64_t(t.single_wdf), t.postings_length, t.positions_length,
				 std::uint64_t(t.impacts.size())}) {
			u64(v);
		}
		for (const auto& i : t.impacts) { u64(std::uint64_t(i.wdf) << 8 | i.length); }
		return true;
	});
	std::fclose(out);
	return 0;
}

int dictsim(const std::string& path) {
	std::FILE* in = std::fopen(path.c_str(), "rb");
	if (!in) { return 1; }
	auto u64 = [&] {
		std::uint64_t v = 0;
		return std::fread(&v, 8, 1, in) == 1 ? std::optional<std::uint64_t>(v) : std::nullopt;
	};
	// A first pass over the keys for the dictionary's alphabet.
	std::array<bool, 256> used{};
	std::string key;
	while (auto n = u64()) {
		key.resize(*n);
		if (std::fread(key.data(), 1, *n, in) != *n) { break; }
		for (char c : key) { used[std::uint8_t(c)] = true; }
		std::uint64_t skip[9];
		if (std::fread(skip, 8, 9, in) != 9) { break; }
		std::fseek(in, long(8 * skip[8]), SEEK_CUR);
	}
	std::rewind(in);
	std::string alphabet;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { alphabet.push_back(char(c)); }
	}
	termdict::builder b(alphabet);
	strata::format::term_run run;
	std::uint64_t terms = 0;
	while (auto n = u64()) {
		key.resize(*n);
		if (std::fread(key.data(), 1, *n, in) != *n) { break; }
		strata::format::term_info t;
		t.flags = std::uint8_t(*u64());
		t.df = std::uint32_t(*u64());
		t.cf = *u64();
		t.postings = *u64();
		t.positions = *u64();
		t.single_wdf = std::uint32_t(*u64());
		t.postings_length = *u64();
		t.positions_length = *u64();
		for (std::uint64_t k = *u64(); k; --k) {
			const std::uint64_t v = *u64();
			t.impacts.push_back({std::uint32_t(v >> 8), std::uint8_t(v)});
		}
		b.add(key, strata::format::encode_term_info(t, run, b.next_is_restart()));
		++terms;
	}
	std::fclose(in);
	const auto tb = b.finish();
	std::printf("terms %llu: blocks %zu + index %zu = %zu bytes, %.2f B/term (restart interval %zu, blocks of %zu-%zu)\n", (unsigned long long)terms, tb.blocks.size(), tb.index.size(),
		tb.blocks.size() + tb.index.size(), double(tb.blocks.size() + tb.index.size()) / double(terms), termdict::restart_interval, termdict::min_block_entries, termdict::max_block_entries);
	return 0;
}

std::string query_of(const std::string& line) {
	auto at = line.find("\"query\": \"");
	if (at == std::string::npos) { return {}; }
	std::string q;
	for (std::size_t i = at + 10; i < line.size() && line[i] != '"'; ++i) { q.push_back(line[i] == '\\' ? line[++i] : line[i]); }
	return q;
}

// For every query, under both scoring schemes: the pruned top-k against a scan that
// scores every match, and the counts. Scores agree within 1e-9 relative, rank by rank;
// ordinals agree exactly, except that each run of hits whose scores agree within 1e-9
// must be drawn from the scan's documents with that score (ties compare as sets, since
// summation order can perturb near-equal scores).
int verify(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::size_t queries = 0, mismatches = 0;
	auto close = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(a, b)); };
	LineReader in; for (std::string line; in.next(line);) {
		const std::string q = query_of(line);
		if (q.empty()) { continue; }
		++queries;
		const parsed p = parse(q);
		for (const auto scheme : {strata::scheme::bm25, strata::scheme::bm25_xapian}) {
			auto all = build(seg, p, scheme);
			if (!all) { break; }
			std::vector<strata::hit> scan;
			for (auto d = all->next(); d != strata::no_more_docs; d = all->next()) { scan.push_back({d, all->score()}); }
			std::sort(scan.begin(), scan.end(), [](const auto& a, const auto& b) { return a.score != b.score ? a.score > b.score : a.ord < b.ord; });
			auto counted = build(seg, p, scheme);
			bool ok = strata::count(*counted) == scan.size();
			for (std::size_t k : {10u, 100u, 1000u}) {
				auto pruned = build(seg, p, scheme);
				const auto hits = strata::top_k(*pruned, k);
				const std::size_t n = std::min<std::size_t>(k, scan.size());
				ok = ok && hits.size() == n;
				for (std::size_t a = 0; ok && a < n;) {
					std::size_t b = a + 1;
					while (b < n && close(scan[b].score, scan[a].score)) { ++b; }
					// Every scan document with this score, beyond the cut at k too.
					std::vector<strata::ordinal_t> tied;
					for (std::size_t i = a; i < scan.size() && close(scan[i].score, scan[a].score); ++i) { tied.push_back(scan[i].ord); }
					for (std::size_t i = a; i > 0 && close(scan[i - 1].score, scan[a].score); --i) { tied.push_back(scan[i - 1].ord); }
					std::sort(tied.begin(), tied.end());
					std::vector<strata::ordinal_t> got;
					for (std::size_t i = a; i < b; ++i) {
						ok = ok && close(hits[i].score, scan[i].score);
						got.push_back(hits[i].ord);
					}
					std::sort(got.begin(), got.end());
					ok = ok && std::adjacent_find(got.begin(), got.end()) == got.end() && std::includes(tied.begin(), tied.end(), got.begin(), got.end());
					a = b;
				}
				if (!ok) {
					++mismatches;
					std::cout << "MISMATCH " << (scheme == strata::scheme::bm25 ? "bm25" : "bm25_xapian") << " k=" << k << '\t' << q << '\n';
					break;
				}
			}
		}
	}
	std::cout << queries << " queries, " << mismatches << " mismatches\n";
	return mismatches ? 1 : 0;
}

// queries.txt is the harness's JSON lines: {"query": "...", "tags": [...]}.
int bench(const std::string& dir, std::string_view command, int runs, strata::scheme scheme) {
	residency::file file;
	strata::segment seg = open(dir, file);
	LineReader in; for (std::string line; in.next(line);) {
		auto at = line.find("\"query\": \"");
		if (at == std::string::npos) { continue; }
		std::string unescaped;
		for (std::size_t i = at + 10; i < line.size() && line[i] != '"'; ++i) { unescaped.push_back(line[i] == '\\' ? line[++i] : line[i]); }
		double best = 1e300;
		std::uint64_t result = 0;
		for (int r = 0; r < runs; ++r) {
			auto t0 = std::chrono::steady_clock::now();
			result = run(seg, command, unescaped, scheme);
			best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
		std::cout << std::fixed << std::setprecision(1) << best << '\t' << result << '\t' << unescaped << '\n';
	}
	return 0;
}

}  // namespace

// Bytes of each section in the page cache (mincore over a fresh mapping): run after
// serving, without dropping caches, to attribute serving memory by section.
int resident(const std::string& dir) {
	const std::string path = dir + "/segment.sx";
	const int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) { return 1; }
	struct stat st;
	::fstat(fd, &st);
	const std::size_t size = std::size_t(st.st_size), page = std::size_t(::sysconf(_SC_PAGESIZE));
	void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
	::close(fd);
	if (map == MAP_FAILED) { return 1; }
	std::vector<unsigned char> in((size + page - 1) / page);
	auto core = [&]<class A, class T>(int (*f)(A, std::size_t, T*)) { return f(map, size, reinterpret_cast<T*>(in.data())); };  // char* on macOS
	if (core(&::mincore) != 0) { return 1; }
	const std::string_view bytes(static_cast<const char*>(map), size);
	const auto toc = strata::format::decode_toc(bytes, strata::format::decode_footer(bytes));
	const char* names[] = {"header", "terms", "term_index", "postings", "positions", "lengths", "stored", "docids", "stats", "norms", "versions", "pk_filter"};
	std::uint64_t total = 0;
	for (const auto& e : toc) {
		std::uint64_t held = 0;  // the section's bytes on resident pages
		for (std::uint64_t p = e.offset / page; p * page < e.offset + e.length; ++p) {
			if (in[p] & 1) { held += std::min<std::uint64_t>((p + 1) * page, e.offset + e.length) - std::max<std::uint64_t>(p * page, e.offset); }
		}
		total += held;
		const auto k = std::size_t(e.kind);
		std::printf("%-12s %12llu bytes %10.1f MB resident (%.1f%%)\n", k < std::size(names) ? names[k] : "?", (unsigned long long)e.length, double(held) / 1e6,
			e.length ? 100.0 * double(held) / double(e.length) : 0.0);
	}
	std::printf("total %.1f MB resident of %.1f MB\n", double(total) / 1e6, double(size) / 1e6);
	::munmap(map, size);
	return 0;
}

// A document of whitespace tokens: each distinct token with its wdf and positions, the
// key term (wdf 0), and the id stored.

int churn(const std::string& dir) {
	std::ios::sync_with_stdio(false);
	std::filesystem::create_directories(dir);
	// PosixIO refuses a directory others can write; a umask of 002 leaves group write on.
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	o.auto_merge = true;
	const std::array<char, 16> uuid{'s', 't', 'r', 'a', 't', 'a', '-', 's', 'o', 'a', 'k'};
	auto ix = strata::index::create(io, o, uuid);
	using clock = std::chrono::steady_clock;
	auto us = [](clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t).count(); };
	strata::transaction t;
	auto apply = [&] {
		if (t.ops.empty()) { return; }
		if (ix.apply(t, {}).result != strata::verdict::accepted) { std::cerr << "rejected\n"; std::exit(1); }
		t.ops.clear();
	};
	std::string line;
	for (LineReader in; in.next(line);) {
		const std::string_view l = line;
		if (l.empty()) { continue; }
		const std::size_t a = l.find('\t'), b = a == std::string_view::npos ? a : l.find('\t', a + 1);
		switch (l[0]) {
		case 'P': {
			strata::operation op;
			op.type = strata::operation::replace_key;
			op.key = "Q" + std::string(l.substr(a + 1, b - a - 1));
			op.doc = tokens_document(l.substr(a + 1, b - a - 1), l.substr(b + 1));
			t.ops.push_back(std::move(op));
			if (t.ops.size() == 1000) { apply(); }
			break;
		}
		case 'D': {
			strata::operation op;
			op.type = strata::operation::delete_key;
			op.key = "Q" + std::string(l.substr(a + 1));
			t.ops.push_back(std::move(op));
			if (t.ops.size() == 1000) { apply(); }
			break;
		}
		case 'C': {
			apply();
			const auto start = clock::now();
			ix.commit();
			std::cout << "C\t" << us(start) << '\t' << ix.segments().size() << std::endl;
			break;
		}
		case 'Q': {
			const std::string_view command = l.substr(a + 1, b - a - 1), query = l.substr(b + 1);
			const auto start = clock::now();
			std::vector<strata::segment_snapshot> segs;
			for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted}); }
			const parsed q = parse(query);
			auto builder = [&](const strata::segment& seg) { return build(seg, q); };
			std::uint64_t result = 1;
			if (command == "COUNT") {
				result = strata::search_count(segs, builder);
			} else {
				std::size_t k = 0;
				std::from_chars(command.data() + 4, command.data() + command.size(), k);
				std::uint64_t n = 0;
				(void)strata::search(segs, builder, k, command.ends_with("_COUNT") ? &n : nullptr);
				if (command.ends_with("_COUNT")) { result = n; }
			}
			std::cout << "Q\t" << us(start) << '\t' << result << std::endl;
			break;
		}
		}
	}
	apply();
	ix.commit();
	std::cout.flush();
	return 0;
}

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "index" && argc == 3) { return index(argv[2]); }
	if (mode == "merge" && argc == 3) { return merge(argv[2]); }
	if (mode == "serve" && argc == 3) { return serve(argv[2]); }
	if (mode == "terms" && argc == 3) { return terms(argv[2]); }
	if (mode == "dictstats" && argc == 3) { return dictstats(argv[2]); }
	if (mode == "dictdump" && argc == 4) { return dictdump(argv[2], argv[3]); }
	if (mode == "check" && argc == 3) {
		residency::file file;
		strata::segment seg = open(argv[2], file);
		strata::check_report r;
		strata::check_segment(seg, nullptr, r);
		std::printf("ok: %llu documents, %llu terms, %llu postings, %llu positions, %llu stored bytes\n", (unsigned long long)r.documents, (unsigned long long)r.terms,
			(unsigned long long)r.postings, (unsigned long long)r.positions, (unsigned long long)r.stored_bytes);
		return 0;
	}
	if (mode == "resident" && argc == 3) { return resident(argv[2]); }
	if (mode == "dictsim" && argc == 3) { return dictsim(argv[2]); }
	if (mode == "verify" && argc == 3) { return verify(argv[2]); }
	if (mode == "churn" && argc == 3) { return churn(argv[2]); }
	if (mode == "bench" && (argc == 5 || argc == 6)) {
		const bool xapian = argc == 6 && std::string_view(argv[5]) == "bm25_xapian";
		return bench(argv[2], argv[3], std::atoi(argv[4]), xapian ? strata::scheme::bm25_xapian : strata::scheme::bm25);
	}
	std::cerr << "usage: strata_engine index|serve|terms|dictstats|verify|check|resident|churn DIR | bench DIR COMMAND RUNS < queries\n";
	return 2;
}
