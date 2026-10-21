// The minimum a required-plus-optional scorer hands its optional side: it may never drop a
// document that competes, whatever order the required side's scores sum in. Fake scorers with
// chosen scores sit on the boundary; the scorer's output is checked against a brute-force sum.
#include "prospect/search.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>
#include <utility>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::no_more_docs;
using strata::ordinal_t;

constexpr double inf = std::numeric_limits<double>::infinity();

// Matches the given (ordinal, score) pairs; after a minimum, skips what scores at or below it, as
// the contract allows. Its bound is the given one, which may exceed every score.
class fake_scorer final : public prospect::scorer {
public:
	fake_scorer(std::vector<std::pair<ordinal_t, double>> docs, double bound, std::uint64_t cost, std::size_t terms = 1)
		: docs_(std::move(docs)), bound_(bound), cost_(cost), terms_(terms) {}
	ordinal_t doc() const noexcept override { return i_ < docs_.size() ? docs_[i_].first : no_more_docs; }
	ordinal_t next() override {
		if (started_) { ++i_; }
		started_ = true;
		return settle();
	}
	ordinal_t advance(ordinal_t t) override {
		started_ = true;
		while (i_ < docs_.size() && docs_[i_].first < t) { ++i_; }
		return settle();
	}
	std::uint64_t cost() const noexcept override { return cost_; }
	double score() override { return docs_[i_].second; }
	double max_score(ordinal_t, ordinal_t) override { return bound_; }
	ordinal_t block_end(ordinal_t) override { return no_more_docs; }
	double max_score_total() const noexcept override { return bound_; }
	std::size_t score_terms() const noexcept override { return terms_; }
	void set_min_competitive_score(double s) override {
		min_ = s;
		++minimums;
	}
	std::size_t skipped = 0, minimums = 0;
	double min_ = 0;

private:
	ordinal_t settle() {
		while (i_ < docs_.size() && min_ > 0 && docs_[i_].second <= min_) {
			++i_;
			++skipped;
		}
		return doc();
	}
	std::vector<std::pair<ordinal_t, double>> docs_;
	double bound_;
	std::uint64_t cost_;
	std::size_t terms_, i_ = 0;
	bool started_ = false;
};

using docs_t = std::vector<std::pair<ordinal_t, double>>;

// A conjunction of fakes that all match every document of `docs`: child i scores a[i] there and
// costs cost[i], so the conjunction sums in the given order and bounds in cost order.
static prospect::scorer_ptr conjunction(const std::vector<double>& a, const std::vector<std::uint64_t>& cost, const std::vector<ordinal_t>& at) {
	std::vector<prospect::scorer_ptr> kids;
	for (std::size_t i = 0; i < a.size(); ++i) {
		docs_t d;
		for (ordinal_t o : at) { d.push_back({o, a[i]}); }
		kids.push_back(std::make_unique<fake_scorer>(std::move(d), a[i], cost[i]));
	}
	return std::make_unique<prospect::conjunction_scorer>(std::move(kids));
}

// Runs the scorer with minimum s; returns ordinal -> score of what it yields.
static std::map<ordinal_t, double> run(prospect::scorer& q, double s) {
	q.set_min_competitive_score(s);
	std::map<ordinal_t, double> out;
	for (ordinal_t d = q.next(); d != no_more_docs; d = q.next()) { out[d] = q.score(); }
	return out;
}

int main() {
	// The required score r sums its leaves in the given order, the bound R in cost order: here
	// r exceeds R by an ulp. With the optional score o just below fl(s - R), a minimum rounded
	// down by one step only (no margin) drops a document whose fl(r + o) exceeds s.
	{
		const std::vector<double> a = {0.4439043102930863, 0.9861849833692796, 0.5855642789889313};
		const std::vector<std::uint64_t> cost = {30, 20, 10};  // the last child leads: bounds sum reversed
		const double o = 0.6607442951957146, s = 2.6763978678470117;
		auto probe = conjunction(a, cost, {0});
		const double R = probe->max_score_total();
		probe->next();
		const double r = probe->score();
		CHECK(r > R);                                           // the score outruns the bound
		CHECK(r + o > s);                                       // the document competes
		CHECK(o <= std::nextafter(s - R, -inf));                // a bare step below s - R would drop it
		fake_scorer* opt_ptr = nullptr;
		auto opt = std::make_unique<fake_scorer>(docs_t{{3, o}}, o, 1);
		opt_ptr = opt.get();
		prospect::required_optional_scorer q(conjunction(a, cost, {3}), std::move(opt));
		const auto got = run(q, s);
		CHECK(got.size() == 1 && got.count(3) && got.at(3) == r + o);
		CHECK(opt_ptr->minimums == 1);
	}
	// A sweep across the boundary: optional scores from four steps below fl(s - R) to four above,
	// one document each, plus decoys far below. Every document whose sum exceeds s is returned
	// with its exact score; the decoys are dropped (the optional side was told a minimum).
	{
		const std::vector<double> a = {0.4439043102930863, 0.9861849833692796, 0.5855642789889313};
		const std::vector<std::uint64_t> cost = {30, 20, 10};
		const double s = 2.6763978678470117;
		auto probe = conjunction(a, cost, {0});
		const double R = probe->max_score_total();
		probe->next();
		const double r = probe->score();
		docs_t od;
		std::vector<ordinal_t> at;
		double o = s - R;
		for (int j = 0; j < 4; ++j) { o = std::nextafter(o, -inf); }
		ordinal_t d = 0;
		for (int j = 0; j < 9; ++j, o = std::nextafter(o, inf), ++d) {
			od.push_back({d, o});
			at.push_back(d);
		}
		for (int j = 0; j < 5; ++j, ++d) {
			od.push_back({d, 1e-3 * (j + 1)});
			at.push_back(d);
		}
		auto opt = std::make_unique<fake_scorer>(od, od[8].second * 2, 1);
		fake_scorer* opt_ptr = opt.get();
		prospect::required_optional_scorer q(conjunction(a, cost, at), std::move(opt));
		const auto got = run(q, s);
		std::size_t competing = 0;
		for (const auto& [ord, os] : od) {
			if (r + os > s) {
				++competing;
				CHECK(got.count(ord) && got.at(ord) == r + os);
			}
		}
		CHECK(competing >= 1);
		CHECK(got.size() < od.size());  // the decoys never come back
		CHECK(opt_ptr->skipped >= 5);   // the optional side pruned them itself
	}
	// Random leaves, orders and boundaries: nothing that exceeds s is ever lost. The boundary is
	// each document's own sum, one step below it (it competes) and the sum itself (it does not).
	std::mt19937_64 rng(7);
	std::size_t competed = 0, pruned = 0;
	for (int trial = 0; trial < 20000; ++trial) {
		const std::size_t n = 1 + rng() % 5;
		std::vector<double> a(n);
		std::vector<std::uint64_t> cost(n);
		for (auto& x : a) { x = std::ldexp(1.0 + double(rng() % 100000) / 100000.0, int(rng() % 4) - 2); }
		for (auto& c : cost) { c = 1 + rng() % 8; }
		const std::size_t ndocs = 1 + rng() % 12;
		std::vector<ordinal_t> at;
		docs_t od;
		for (std::size_t i = 0; i < ndocs; ++i) {
			at.push_back(ordinal_t(i * 2));
			od.push_back({ordinal_t(i * 2), std::ldexp(double(1 + rng() % 4000) / 1000.0, -int(rng() % 4))});
		}
		auto probe = conjunction(a, cost, {0});
		probe->next();
		const double r = probe->score();
		const std::size_t pick = rng() % ndocs;
		double s = r + od[pick].second;
		switch (rng() % 3) {
			case 0: s = std::nextafter(s, -inf); break;
			case 1: break;
			default: s = std::nextafter(std::nextafter(s, inf), inf); break;
		}
		auto opt = std::make_unique<fake_scorer>(od, [&] { double b = 0; for (auto& p : od) { b = std::max(b, p.second); } return b; }(), 1);
		fake_scorer* opt_ptr = opt.get();
		prospect::required_optional_scorer q(conjunction(a, cost, at), std::move(opt));
		const auto got = run(q, s);
		for (const auto& [ord, os] : od) {
			if (r + os > s) {
				++competed;
				CHECK(got.count(ord) && got.at(ord) == r + os);
			}
		}
		pruned += opt_ptr->skipped;
	}
	CHECK(competed > 10000);
	CHECK(pruned > 1000);  // the property is not vacuous: the optional side does prune
	// score_terms counts every leaf a sum adds.
	{
		std::vector<prospect::scorer_ptr> kids;
		kids.push_back(std::make_unique<fake_scorer>(docs_t{}, 1.0, 1, 3));
		kids.push_back(std::make_unique<fake_scorer>(docs_t{}, 1.0, 1, 4));
		prospect::disjunction_scorer u(std::move(kids));
		CHECK(u.score_terms() == 7);
		prospect::required_optional_scorer q(std::make_unique<fake_scorer>(docs_t{}, 1.0, 1, 2), std::make_unique<prospect::disjunction_scorer>(std::vector<prospect::scorer_ptr>{}));
		CHECK(q.score_terms() == 2);
	}
	std::printf("pruning: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
