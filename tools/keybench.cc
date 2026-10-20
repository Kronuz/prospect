// keybench: T5's key lookups (format-13.md, gate rows). Opens an index read-only and times
// find_key over the keys on stdin, one per line: each key as a hit, and the key with "~"
// appended as a miss inside the keys' range. Best of ROUNDS, ns per lookup.
//   keybench DIR ROUNDS [PREFIX] < keys
// Uses only the index's public API, so the same file builds against format 12 and format 13.
#include "strata/search/index.hh"

#include "completion/posix.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
	if (argc < 3) {
		std::cerr << "usage: keybench DIR ROUNDS [PREFIX] < keys\n";
		return 2;
	}
	kronuz::journal::PosixIO io(argv[1], 0600, {true, true});
	strata::index::options o;
	o.key_prefix = argc > 3 ? argv[3] : "Q";
	o.auto_merge = false;
	auto ix = strata::index::open(io, o);
	std::vector<std::string> hits, misses;
	for (std::string line; std::getline(std::cin, line);) {
		if (line.empty()) { continue; }
		hits.push_back(line);
		misses.push_back(line + "~");
	}
	const int rounds = std::max(1, std::atoi(argv[2]));
	auto time = [&](const std::vector<std::string>& keys, std::size_t& found) {
		double best = 1e300;
		for (int r = 0; r < rounds; ++r) {
			found = 0;
			const auto start = std::chrono::steady_clock::now();
			for (const auto& k : keys) { found += ix.find_key(k).has_value(); }
			const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
			best = std::min(best, ns / double(keys.size()));
		}
		return best;
	};
	std::size_t found_hits = 0, found_misses = 0;
	const double hit_ns = time(hits, found_hits), miss_ns = time(misses, found_misses);
	std::printf("segments %zu keys %zu hit_ns %.1f miss_ns %.1f found %zu false %zu\n", ix.segments().size(), hits.size(), hit_ns, miss_ns, found_hits,
		found_misses);
	return found_misses == 0 ? 0 : 1;
}
