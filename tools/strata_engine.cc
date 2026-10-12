// strata as a search-benchmark-game engine.
//   strata_engine index DIR < corpus.json   one segment from JSON lines ("text": whitespace tokens)
//   strata_engine serve DIR                  "COMMAND\tquery" lines -> one count per line
// Query syntax: words are optional, +word is required, -word is excluded, "a b" is a phrase.
#include "residency.hh"
#include "strata/search.hh"
#include "strata/writer.hh"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string_view text_field(std::string_view line) {
	auto at = line.find("\"text\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 9;
	return line.substr(at, line.find('"', at) - at);
}

int index(const std::string& dir) {
	std::ios::sync_with_stdio(false);
	strata::segment_writer w;
	std::string line;
	std::vector<std::pair<std::string_view, std::uint32_t>> tokens;  // (token, position)
	std::vector<std::uint32_t> positions;
	std::vector<strata::posting_input> in;
	std::uint64_t n = 0;
	while (std::getline(std::cin, line)) {
		if (line.empty()) { continue; }
		std::string_view text = text_field(line);
		tokens.clear();
		std::uint32_t pos = 0;
		for (std::size_t i = 0; i < text.size();) {
			std::size_t j = text.find(' ', i);
			if (j == std::string_view::npos) { j = text.size(); }
			if (j > i) { tokens.push_back({text.substr(i, j - i), ++pos}); }
			i = j + 1;
		}
		// Group a document's tokens by sorting: each term's positions end up contiguous.
		std::sort(tokens.begin(), tokens.end());
		positions.clear();
		for (const auto& t : tokens) { positions.push_back(t.second); }
		in.clear();
		for (std::size_t i = 0; i < tokens.size();) {
			std::size_t j = i;
			while (j < tokens.size() && tokens[j].first == tokens[i].first) { ++j; }
			in.push_back({tokens[i].first, std::uint32_t(j - i), std::span<const std::uint32_t>(positions.data() + i, j - i)});
			i = j;
		}
		w.add(++n, in, pos);
		if (n % 100000 == 0) { std::cerr << n << '\n'; }
	}
	std::string bytes = w.flush({});
	std::ofstream(dir + "/segment.sx", std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
	std::cerr << "segment: " << bytes.size() << " bytes, " << n << " documents\n";
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

// A clause's scorer, or null when one of its terms is absent (the clause matches nothing).
strata::scorer_ptr clause_scorer(const strata::segment& seg, const std::vector<std::string>& clause) {
	std::vector<strata::format::term_info> infos;
	for (const auto& t : clause) {
		auto info = seg.term(t);
		if (!info) { return nullptr; }
		infos.push_back(*info);
	}
	if (infos.size() == 1) { return std::make_unique<strata::term_scorer>(seg, infos[0]); }
	return std::make_unique<strata::phrase_scorer>(seg, infos);
}

strata::scorer_ptr build(const strata::segment& seg, const parsed& q) {
	std::vector<strata::scorer_ptr> must, should, must_not;
	for (const auto& c : q.must) {
		auto s = clause_scorer(seg, c);
		if (!s) { return nullptr; }
		must.push_back(std::move(s));
	}
	for (const auto& c : q.should) {
		if (auto s = clause_scorer(seg, c)) { should.push_back(std::move(s)); }
	}
	for (const auto& c : q.must_not) {
		if (auto s = clause_scorer(seg, c)) { must_not.push_back(std::move(s)); }
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
	if (result && !must_not.empty()) { result = std::make_unique<strata::exclusion_scorer>(std::move(result), combine(std::move(must_not), false)); }
	return result;
}

int serve(const std::string& dir) {
	auto file = residency::file::open(dir + "/segment.sx");
	auto bytes = file.view({0, file.size()});
	strata::segment seg(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	std::string line;
	std::cout.setf(std::ios::unitbuf);
	while (std::getline(std::cin, line)) {
		const std::size_t tab = line.find('\t');
		const std::string_view command(line.data(), tab);
		const parsed q = parse(std::string_view(line).substr(tab + 1));
		std::uint64_t result = 1;
		const bool counting = command == "COUNT" || command.ends_with("_COUNT");
		// A single term's count is its document frequency (one segment, no deletes).
		if (command == "COUNT" && q.must.size() + q.should.size() == 1 && q.must_not.empty() &&
			(q.must.empty() ? q.should[0] : q.must[0]).size() == 1) {
			auto info = seg.term((q.must.empty() ? q.should[0] : q.must[0])[0]);
			std::cout << (info ? info->df : 0) << '\n';
			continue;
		}
		auto s = build(seg, q);
		if (!s) {
			std::cout << (counting ? 0 : 1) << '\n';
			continue;
		}
		if (command == "COUNT") {
			result = strata::count(*s);
		} else if (command.starts_with("TOP_")) {
			std::size_t k = 0;
			std::from_chars(command.data() + 4, command.data() + command.size(), k);
			std::uint64_t n = 0;
			strata::top_k(*s, k, counting ? &n : nullptr);
			if (counting) { result = n; }
		}
		std::cout << result << '\n';
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 3) {
		std::cerr << "usage: strata_engine index|serve DIR\n";
		return 2;
	}
	std::string mode = argv[1];
	return mode == "index" ? index(argv[2]) : serve(argv[2]);
}
