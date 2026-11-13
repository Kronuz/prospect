// Replays a fuzz target's checked-in corpus as a test: every file in the directory goes
// through LLVMFuzzerTestOneInput once, without libFuzzer. REPLAY_COST=1 adds each input's
// CPU time (a line "cost SIZE MICROSECONDS NAME") and the corpus's quantiles.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

static double cpu_us() {
	timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return double(t.tv_sec) * 1e6 + double(t.tv_nsec) / 1e3;
}

int main(int argc, char** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s CORPUS_DIR\n", argv[0]);
		return 2;
	}
	std::vector<std::filesystem::path> files;
	for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
		if (e.is_regular_file()) { files.push_back(e.path()); }
	}
	std::sort(files.begin(), files.end());
	const bool cost = std::getenv("REPLAY_COST") != nullptr;
	std::vector<double> us;
	for (const auto& f : files) {
		std::ifstream in(f, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const double began = cpu_us();
		LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
		if (cost) {
			us.push_back(cpu_us() - began);
			std::printf("cost %zu %.0f %s\n", bytes.size(), us.back(), f.filename().c_str());
		}
	}
	if (cost && !us.empty()) {
		double total = 0;
		for (const double u : us) { total += u; }
		std::sort(us.begin(), us.end());
		auto q = [&](double p) { return us[std::size_t(p * double(us.size() - 1))]; };
		std::printf("cpu: %zu inputs, %.3f s total, mean %.0f us, p50 %.0f, p90 %.0f, p99 %.0f, max %.0f\n", us.size(), total / 1e6, total / double(us.size()), q(.5), q(.9), q(.99), us.back());
	}
	std::printf("%s: %zu inputs replayed\n", argv[1], files.size());
	return files.empty() ? 1 : 0;
}
