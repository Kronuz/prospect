// Xapian parity over several fields (fields-and-keys T3b): a terms dump (xapiand_terms.py) indexed
// as documents of field parts, and the oracle's `rank-terms` vectors for term, OR and AND queries
// within and across fields. Fields are Xapiand's engine fields (the prefix through the type
// character, as fieldsize.py splits them); Q (the key) and V (the version) are not inverted terms
// but their wdf is in the document length, which is the host's (D5). Counts and the top-10
// weights must equal Xapian's at relative error 1e-9, ties by docid.
//   parity_terms TERMS QUERIES RANK_JSONL [SEGMENTS]
// QUERIES: lines "KIND<TAB>hex hex ...", KIND TERM, OR or AND, each hex a whole Xapian term.
#include "prospect/index.hh"
#include "prospect/search.hh"
#include "prospect/searcher.hh"
#include "strata/core/faultfs.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool engine_field(const std::string& t, std::size_t& prefix) {
	std::size_t i = !t.empty() && t[0] == 'Z' ? 1 : 0;
	while (i < t.size()) {
		if (t[i] >= 'A' && t[i] <= 'Z') { prefix = i + 1; return true; }
		const auto dot = t.find('.', i);
		if (dot == std::string::npos) { return false; }
		i = dot + 1;
	}
	return false;
}

std::string unhex(std::string_view h) {
	std::string out;
	auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
	for (std::size_t i = 0; i + 1 < h.size(); i += 2) { out.push_back(char(nibble(h[i]) << 4 | nibble(h[i + 1]))); }
	return out;
}

struct term_ref {
	std::string field, value;
};

term_ref split(const std::string& term) {
	std::size_t prefix = 0;
	if (!engine_field(term, prefix)) { throw std::runtime_error("a term without a field"); }
	return {term.substr(0, prefix), term.substr(prefix)};
}

// Minimal extraction of a number from a vector row.
std::string field_of(const std::string& line, const std::string& key) {
	auto at = line.find("\"" + key + "\":");
	if (at == std::string::npos) { return {}; }
	at += key.size() + 3;
	return line.substr(at, line.find_first_of(",}", at) - at);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 4 && argc != 5) {
		std::cerr << "usage: parity_terms TERMS QUERIES RANK_JSONL [SEGMENTS]\n";
		return 2;
	}
	const std::size_t segments = argc == 5 ? std::max<std::size_t>(1, std::stoul(argv[4])) : 1;
	struct posting {
		std::string field, value;
		std::uint32_t wdf;
	};
	std::vector<std::vector<posting>> docs;
	std::vector<std::uint32_t> lengths;
	std::vector<std::string> names;
	{
		std::ifstream in(argv[1]);
		std::string line, current;
		while (std::getline(in, line)) {
			const auto a = line.find('\t'), b = line.find('\t', a + 1);
			if (b == std::string::npos) { continue; }
			const std::string doc = line.substr(0, a);
			if (doc != current || docs.empty()) {
				docs.emplace_back();
				lengths.push_back(0);
				current = doc;
			}
			const std::uint32_t wdf = std::uint32_t(std::strtoul(line.c_str() + a + 1, nullptr, 10));
			lengths.back() += wdf;
			const term_ref t = split(unhex(std::string_view(line).substr(b + 1)));
			if (t.field == "Q" || t.field == "V") { continue; }
			if (std::find(names.begin(), names.end(), t.field) == names.end()) { names.push_back(t.field); }
			docs.back().push_back({t.field, t.value, wdf});
		}
	}
	strata::faultfs::filesystem fs;
	strata::faultfs::io io(fs);
	auto ix = prospect::index::create(io, prospect::index::options{}, std::array<char, 16>{'t', '3', 'b'});
	const std::size_t per = (docs.size() + segments - 1) / segments;
	for (std::size_t from = 0; from < docs.size(); from += per) {
		prospect::transaction t;
		for (const auto& n : names) { t.fields.push_back({n, strata::field_role::indexed, strata::field_postings::freqs, false}); }
		std::map<std::string, std::uint32_t> at;
		for (std::size_t i = 0; i < names.size(); ++i) { at[names[i]] = std::uint32_t(i); }
		for (std::size_t d = from; d < std::min(docs.size(), from + per); ++d) {
			prospect::operation op;
			op.type = prospect::operation::add;
			for (const auto& p : docs[d]) { op.doc.field(strata::field_ref::declared(at[p.field])).terms.push_back({p.value, p.wdf, {}}); }
			op.doc.length = lengths[d];
			t.ops.push_back(std::move(op));
		}
		if (ix.apply(t, {}).result != strata::verdict::accepted) { std::cerr << "rejected\n"; return 1; }
		ix.commit();
	}
	std::vector<prospect::segment_snapshot> snaps;
	for (const auto& s : ix.segments()) { snaps.push_back({s.seg(), s.deleted, s.dead, &ix.fields()}); }
	std::cout << "indexed " << docs.size() << " documents, " << names.size() << " fields, " << snaps.size() << " segment(s)\n";

	std::ifstream queries(argv[2]), rank(argv[3]);
	std::string qline, rline;
	std::size_t n = 0, count_ok = 0, top_ok = 0, shown = 0;
	double worst = 0;
	while (std::getline(queries, qline)) {
		while (std::getline(rank, rline) && rline.find("\"index\"") != std::string::npos) {}
		if (rline.empty()) { std::cerr << "fewer vectors than queries\n"; return 1; }
		++n;
		const auto tab = qline.find('\t');
		const std::string kind = qline.substr(0, tab);
		std::vector<term_ref> terms;
		std::istringstream ss(qline.substr(tab + 1));
		for (std::string h; ss >> h;) { terms.push_back(split(unhex(h))); }
		prospect::term_table table(snaps);
		auto builder = [&](const prospect::segment& seg) -> prospect::scorer_ptr {
			std::vector<prospect::scorer_ptr> kids;
			for (const auto& t : terms) {
				const auto* info = table.info(seg, t.field, t.value);
				if (!info) {
					if (kind == "AND") { return nullptr; }
					continue;
				}
				kids.push_back(std::make_unique<prospect::term_scorer>(seg.get<prospect::inverted_plugin>(), *info, prospect::weight::bm25_xapian(table.stats(), table.df(t.field, t.value))));
			}
			if (kids.empty()) { return nullptr; }
			if (kids.size() == 1) { return std::move(kids[0]); }
			if (kind == "AND") { return std::make_unique<prospect::conjunction_scorer>(std::move(kids)); }
			return std::make_unique<prospect::disjunction_scorer>(std::move(kids));
		};
		std::uint64_t got_count = 0;
		const auto got = prospect::search(snaps, builder, 10, &got_count);
		const std::uint64_t want_count = std::stoull(field_of(rline, "upper"));
		count_ok += got_count == want_count;
		std::vector<std::pair<std::uint64_t, double>> want;
		const auto hits = rline.find("\"hits\":[");
		for (std::size_t p = rline.find('[', hits + 8); p != std::string::npos && p < rline.size(); p = rline.find('[', p + 1)) {
			const std::size_t c1 = rline.find(',', p), c2 = rline.find(',', c1 + 1);
			want.push_back({std::stoull(rline.substr(p + 1, c1 - p - 1)), std::stod(rline.substr(c1 + 1, c2 - c1 - 1))});
		}
		bool ok = got.size() == want.size();
		for (std::size_t i = 0; ok && i < got.size(); ++i) {
			const double rel = std::abs(got[i].score - want[i].second) / std::max(1e-300, std::abs(want[i].second));
			worst = std::max(worst, rel);
			bool tie = false;
			for (const auto& w : want) { tie = tie || (w.first == got[i].docid && std::abs(w.second - got[i].score) <= 1e-9 * std::abs(w.second)); }
			ok = rel <= 1e-9 && (got[i].docid == want[i].first || tie);
		}
		top_ok += ok;
		if ((!ok || got_count != want_count) && shown++ < 8) { std::cout << "MISMATCH " << kind << ' ' << terms.size() << " terms: count " << got_count << " vs " << want_count << '\n'; }
	}
	std::printf("queries %zu: counts equal %zu, top-10 equal %zu, worst relative error %.3g\n", n, count_ok, top_ok, worst);
	return count_ok == n && top_ok == n ? 0 : 1;
}
