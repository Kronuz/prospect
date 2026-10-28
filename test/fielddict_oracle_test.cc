// The field dictionary against a reference model (test/fielddict_oracle.hh): random dictionaries,
// wide directories included, with every read path compared with a std::map; and dictionaries with
// a directory byte changed, which are fenced or, when the deep check accepts them, read the same
// by every path. The seeded decoder bugs of tools/mutants_fields.py (N to Q) die here.
//   fielddict_oracle_test [VALID_COUNT [DAMAGE_COUNT [SEED]]]
#include "fielddict_oracle.hh"

#include <cstdio>
#include <cstdlib>

namespace fd = prospect::fielddict;
namespace oracle = prospect::fielddict::oracle;

int main(int argc, char** argv) {
	const unsigned valid = argc > 1 ? unsigned(std::atoi(argv[1])) : 300;
	const unsigned damage = argc > 2 ? unsigned(std::atoi(argv[2])) : 6;
	std::mt19937_64 rng(argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 20261028);
	oracle::report rep;
	for (unsigned i = 0; i < valid; ++i) {
		const oracle::truth_t t = oracle::random_truth(rng);
		const auto b = oracle::build(t);
		rep.context = "valid #" + std::to_string(i);
		try {
			const fd::reader r(b.blocks, b.index);
			oracle::against_truth(r, t, rng, rep);
		} catch (const std::exception& e) {
			rep.violate(std::string("a valid dictionary threw: ") + e.what());
		}
	}
	// A directory byte (header, samples, entries) set to other values: fenced, or consistent.
	static const std::uint8_t vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xEF, 0xF0, 0xFF};
	std::uint64_t accepted = 0, fenced = 0;
	for (unsigned i = 0; i < damage; ++i) {
		const oracle::truth_t t = oracle::random_truth(rng);
		const auto b = oracle::build(t);
		const std::size_t dir = std::min<std::size_t>(oracle::termdict_bytes_start(b.index), 400);
		for (std::size_t at = 0; at < dir; ++at) {
			for (const std::uint8_t v : vals) {
				if (std::uint8_t(b.index[at]) == v) { continue; }
				std::string idx = b.index;
				idx[at] = char(v);
				rep.context = "damage #" + std::to_string(i) + " index[" + std::to_string(at) + "]=" + std::to_string(v);
				try {
					const fd::reader r(b.blocks, idx);
					r.check();
					++accepted;
					oracle::agree(r, rng, rep);
				} catch (const strata::corrupt_segment&) {
					++fenced;
				} catch (const std::exception& e) {
					rep.violate(std::string("a non-corrupt exception escaped: ") + e.what());
				}
			}
		}
	}
	rep.print();
	std::printf("fielddict_oracle_test: %u valid, damaged %llu accepted %llu fenced, %zu violation classes\n", valid, (unsigned long long)accepted, (unsigned long long)fenced, rep.classes.size());
	return rep.ok() ? 0 : 1;
}
