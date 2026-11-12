// The stems Xapiand's engine gives (snowball::stemmer, by the schema's language name, as
// src/engine/text.cc's stemmer_for does) against the oracle's Xapian stems.
//   stem_parity NAME < oracle.jsonl   compares every {"word", "stem"} row; exit 1 on a difference
//   stem_parity --accepts NAME        prints the canonical language; exit 1 when snowball has none
// The oracle's rows are `oracle stem NAME WORDS` (strata-oracle): raw UTF-8 with \", \\, \n, \t
// and \u00XX escapes.
#include "snowball.hh"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// The string value of "key" in a JSON line, unescaped.
std::string field(std::string_view line, std::string_view key) {
	const std::string open = "\"" + std::string(key) + "\":\"";
	const auto at = line.find(open);
	if (at == std::string_view::npos) { throw std::runtime_error("no " + std::string(key) + " in " + std::string(line)); }
	std::string out;
	for (std::size_t i = at + open.size(); i < line.size(); ++i) {
		const char c = line[i];
		if (c == '"') { return out; }
		if (c != '\\') {
			out.push_back(c);
			continue;
		}
		if (++i >= line.size()) { break; }
		switch (line[i]) {
		case 'n': out.push_back('\n'); break;
		case 't': out.push_back('\t'); break;
		case 'u':
			if (i + 4 >= line.size()) { throw std::runtime_error("short \\u escape"); }
			out.push_back(char(std::stoul(std::string(line.substr(i + 1, 4)), nullptr, 16)));
			i += 4;
			break;
		default: out.push_back(line[i]);
		}
	}
	throw std::runtime_error("unterminated string in " + std::string(line));
}

}  // namespace

int main(int argc, char** argv) {
	if (argc == 3 && std::string_view(argv[1]) == "--accepts") {
		const auto c = snowball::stemmer::canonical(argv[2]);
		std::printf("%s\n", std::string(c).c_str());
		return c.empty() ? 1 : 0;
	}
	if (argc != 2) {
		std::fprintf(stderr, "usage: stem_parity NAME < oracle.jsonl | stem_parity --accepts NAME\n");
		return 2;
	}
	const std::string name = argv[1];
	snowball::stemmer stem(name);
	std::size_t words = 0, equal = 0, shown = 0;
	for (std::string line; std::getline(std::cin, line);) {
		if (line.empty()) { continue; }
		const std::string word = field(line, "word"), want = field(line, "stem");
		const std::string_view got = stem(word);
		++words;
		if (got == want) {
			++equal;
		} else if (shown++ < 10) {
			std::printf("DIFF %s: %s -> %s, Xapian %s\n", name.c_str(), word.c_str(), std::string(got).c_str(), want.c_str());
		}
	}
	std::printf("%s (%s): %zu words, %zu equal, %zu differ\n", name.c_str(), std::string(stem.language()).c_str(), words, equal, words - equal);
	return words > 0 && equal == words ? 0 : 1;
}
