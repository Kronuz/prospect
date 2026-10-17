// Several threads query one segment at once: lazy region verification, the term dictionary,
// stored reads, and every scorer must be safe to share (run under TSan). Each thread's
// answers must equal a single thread's.
#include "strata/search.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

int main() {
	std::mt19937_64 rng(5);
	const char* vocab[] = {"a", "b", "c", "d", "rare"};
	strata::segment_writer w;
	std::vector<std::vector<std::uint32_t>> pos(5);
	for (std::uint32_t d = 0; d < 20000; ++d) {
		std::vector<strata::posting_input> in;
		std::uint32_t at = 0, length = 0;
		for (std::size_t t = 0; t < 5; ++t) {
			pos[t].clear();
			if (rng() % 100 >= (t == 4 ? 3u : 60u)) { continue; }
			const std::uint32_t n = 1 + std::uint32_t(rng() % 3);
			for (std::uint32_t k = 0; k < n; ++k) { pos[t].push_back(at += 1 + std::uint32_t(rng() % 3)); }
			in.push_back({vocab[t], n, pos[t]});
			length += n;
		}
		w.add(d + 1, in, length, std::string(d % 50, 'x'));
	}
	const std::string file = w.flush({});
	const strata::segment seg(file);

	auto run = [&](std::uint64_t& digest) {
		digest = 0;
		auto mix = [&](double v) { digest = digest * 1000003 + std::uint64_t(v * 1e6); };
		std::vector<strata::format::term_info> infos;
		for (const char* t : vocab) { infos.push_back(*seg.get<strata::inverted_plugin>().term(t)); }
		for (std::size_t i = 0; i < infos.size(); ++i) {
			strata::term_scorer s(seg.get<strata::inverted_plugin>(), infos[i]);
			for (const auto& h : strata::top_k(seg, s, 10)) { mix(h.score + h.ord); }
			std::vector<strata::scorer_ptr> v;
			v.push_back(std::make_unique<strata::term_scorer>(seg.get<strata::inverted_plugin>(), infos[i]));
			v.push_back(std::make_unique<strata::term_scorer>(seg.get<strata::inverted_plugin>(), infos[(i + 1) % infos.size()]));
			strata::disjunction_scorer u(std::move(v));
			std::uint64_t n = 0;
			for (const auto& h : strata::top_k(seg, u, 10, &n)) { mix(h.score + h.ord); }
			mix(double(n));
			strata::phrase_scorer p(seg.get<strata::inverted_plugin>(), {infos[i], infos[(i + 1) % infos.size()]});
			for (const auto& h : strata::top_k(seg, p, 10)) { mix(h.score + h.ord); }
		}
		for (strata::ordinal_t d = 0; d < seg.core().doc_count(); d += 97) { mix(double(seg.get<strata::stored_plugin>().stored(d).size())); }
	};
	std::uint64_t want = 0;
	std::vector<std::uint64_t> got(8);
	std::vector<std::thread> threads;
	for (auto& g : got) { threads.emplace_back(run, std::ref(g)); }
	for (auto& t : threads) { t.join(); }
	run(want);
	int failures = 0;
	for (auto g : got) { failures += g != want; }
	std::printf("concurrency: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
