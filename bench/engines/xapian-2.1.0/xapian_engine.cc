// Xapian baseline for the search-benchmark-game protocol.
//   xapian_engine index DIR < corpus.json     build the index (one JSON document per line)
//   xapian_engine serve DIR                    read "COMMAND\tquery" lines, print one count per line
//   xapian_engine churn DIR < ops              strata's soak op stream (bench/soak/soak.py): upserts and
//                                              deletes by a "Q"-prefixed id term, commits, queries
// The corpus is pre-normalized (lowercase words separated by spaces), so terms are whitespace tokens.
#include <xapian.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Returns the raw value of a JSON string or number field; the corpus has no escapes in "text".
std::string_view json_field(std::string_view line, std::string_view key) {
	std::string pattern = "\"" + std::string(key) + "\": ";
	auto at = line.find(pattern);
	if (at == std::string_view::npos) return {};
	at += pattern.size();
	if (line[at] == '"') {
		auto end = line.find('"', at + 1);
		return line.substr(at + 1, end - at - 1);
	}
	auto end = line.find_first_of(",}", at);
	return line.substr(at, end - at);
}

int index(const std::string& dir) {
	// Builds into DIR.build, then compacts into DIR (a single merged database, like the other engines).
	Xapian::WritableDatabase db(dir + ".build", Xapian::DB_CREATE_OR_OVERWRITE);
	std::string line;
	std::uint64_t n = 0, skipped = 0;
	while (std::getline(std::cin, line)) {
		if (line.empty()) continue;
		Xapian::Document doc;
		auto text = json_field(line, "text");
		Xapian::termpos pos = 0;
		for (std::size_t i = 0; i < text.size();) {
			auto j = text.find(' ', i);
			if (j == std::string_view::npos) j = text.size();
			// Xapian rejects terms over 245 bytes; Lucene and Tantivy drop such tokens too.
			if (j - i > 245) { ++skipped; } else if (j > i) { doc.add_posting(std::string(text.substr(i, j - i)), ++pos); }
			i = j + 1;
		}
		std::uint64_t sort = 0;
		auto sf = json_field(line, "sort_field");
		std::from_chars(sf.data(), sf.data() + sf.size(), sort);
		doc.add_value(0, Xapian::sortable_serialise(static_cast<double>(sort)));
		doc.set_data(std::string(json_field(line, "id")));
		db.add_document(doc);
		if (++n % 100000 == 0) std::cerr << n << '\n';
	}
	db.commit();
	std::cerr << "skipped " << skipped << " terms over 245 bytes\n";
	db.compact(dir);
	return 0;
}

// Query syntax: words are optional (OR), +word is required, -word is excluded, "a b" is a phrase.
Xapian::Query parse(std::string_view q) {
	std::vector<Xapian::Query> must, should, must_not;
	for (std::size_t i = 0; i < q.size();) {
		if (q[i] == ' ') { ++i; continue; }
		bool required = q[i] == '+', excluded = q[i] == '-';
		if (required || excluded) ++i;
		Xapian::Query clause;
		if (q[i] == '"') {
			auto end = q.find('"', i + 1);
			std::vector<std::string> words;
			auto phrase = q.substr(i + 1, end - i - 1);
			for (std::size_t a = 0; a < phrase.size();) {
				auto b = phrase.find(' ', a);
				if (b == std::string_view::npos) b = phrase.size();
				if (b > a) words.emplace_back(phrase.substr(a, b - a));
				a = b + 1;
			}
			clause = Xapian::Query(Xapian::Query::OP_PHRASE, words.begin(), words.end());
			i = end + 1;
		} else {
			auto end = q.find(' ', i);
			if (end == std::string_view::npos) end = q.size();
			clause = Xapian::Query(std::string(q.substr(i, end - i)));
			i = end;
		}
		(excluded ? must_not : required ? must : should).push_back(clause);
	}
	Xapian::Query any(Xapian::Query::OP_OR, should.begin(), should.end());
	Xapian::Query result = any;
	if (!must.empty()) {
		Xapian::Query all(Xapian::Query::OP_AND, must.begin(), must.end());
		result = should.empty() ? all : Xapian::Query(Xapian::Query::OP_AND_MAYBE, all, any);
	}
	if (must_not.empty()) return result;
	return Xapian::Query(Xapian::Query::OP_AND_NOT, result, Xapian::Query(Xapian::Query::OP_OR, must_not.begin(), must_not.end()));
}

std::uint64_t run(Xapian::Enquire& enquire, Xapian::doccount everything, std::string_view command, std::string_view query) {
	enquire.set_query(parse(query));
	std::uint64_t count = 1;
	if (command == "COUNT" || command == "UNOPTIMIZED_COUNT") {
		count = enquire.get_mset(0, 0, everything).get_matches_estimated();
	} else if (command.starts_with("TOP_")) {
		bool counting = command.ends_with("_COUNT");
		unsigned k = 0;
		std::from_chars(command.data() + 4, command.data() + command.size(), k);
		auto mset = enquire.get_mset(0, k, counting ? everything : 0);
		if (counting) count = mset.get_matches_estimated();
	}
	return count;
}

int churn(const std::string& dir) {
	Xapian::WritableDatabase db(dir, Xapian::DB_CREATE_OR_OVERWRITE);
	using clock = std::chrono::steady_clock;
	auto us = [](clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t).count(); };
	std::string line;
	while (std::getline(std::cin, line)) {
		std::string_view l = line;
		if (l.empty()) continue;
		auto a = l.find('\t'), b = a == std::string_view::npos ? a : l.find('\t', a + 1);
		if (l[0] == 'P') {
			std::string id(l.substr(a + 1, b - a - 1));
			auto text = l.substr(b + 1);
			Xapian::Document doc;
			Xapian::termpos pos = 0;
			for (std::size_t i = 0; i < text.size();) {
				auto j = text.find(' ', i);
				if (j == std::string_view::npos) j = text.size();
				if (j > i && j - i <= 245) doc.add_posting(std::string(text.substr(i, j - i)), ++pos);
				i = j + 1;
			}
			doc.add_boolean_term("Q" + id);
			doc.set_data(id);
			db.replace_document("Q" + id, doc);
		} else if (l[0] == 'D') {
			db.delete_document("Q" + std::string(l.substr(a + 1)));
		} else if (l[0] == 'C') {
			auto start = clock::now();
			db.commit();
			std::cout << "C\t" << us(start) << "\t1" << std::endl;
		} else if (l[0] == 'Q') {
			auto start = clock::now();
			Xapian::Enquire enquire(db);
			auto result = run(enquire, db.get_doccount(), l.substr(a + 1, b - a - 1), l.substr(b + 1));
			std::cout << "Q\t" << us(start) << '\t' << result << std::endl;
		}
	}
	db.commit();
	return 0;
}

int serve(const std::string& dir) {
	Xapian::Database db(dir);
	Xapian::Enquire enquire(db);
	const auto everything = db.get_doccount();
	std::string line;
	std::cout.setf(std::ios::unitbuf);
	while (std::getline(std::cin, line)) {
		auto tab = line.find('\t');
		std::cout << run(enquire, everything, std::string_view(line.data(), tab), std::string_view(line).substr(tab + 1)) << '\n';
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 3) { std::cerr << "usage: xapian_engine index|serve|churn DIR\n"; return 2; }
	std::string mode = argv[1];
	return mode == "index" ? index(argv[2]) : mode == "churn" ? churn(argv[2]) : serve(argv[2]);
}
