// Sliced search (include/prospect/slice.hh, docs/mechanisms/sliced-search.md):
//   1. Identity: a search sliced into windows (a 0 ns slice yields at every checkpoint), into a few
//      windows, or not at all answers exactly as the synchronous search: hits, score bits, order,
//      counts, an aggregation fed by the visitor, and a key sort; and, on a corpus of several windows
//      per segment, the model that scores every live match.
//   2. A long query yields: at least one resume per window of a counted or exhaustive scan.
//   3. A cancellation between slices ends the search at the next checkpoint, runs no more of the
//      host's callbacks, and releases the pinned view; so does destroying a suspended task.
//   4. The pin holds across suspensions; a short query finishes while a long one is suspended;
//      a suspended search may resume on another thread.
#include "shards_fixture.hh"
#include "prospect/aggregate.hh"
#include "prospect/slice.hh"
#include "task.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

static std::atomic<int> failures{0};
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace prospect;
using namespace shards_fixture;
namespace agg = prospect::aggregate;

namespace {

// --- running a search on a scheduler ---------------------------------------------------------------

constexpr std::chrono::nanoseconds one_hour = std::chrono::hours(1);

template <typename T>
struct outcome {
	std::optional<T> value;
	std::exception_ptr error;
	std::size_t resumes = 0;  // the first resume starts the task; every later one is a yield
	bool done = false;
	std::size_t yields() const { return resumes ? resumes - 1 : 0; }
	bool cancelled() const {
		if (!error) { return false; }
		try { std::rethrow_exception(error); } catch (const task::cancelled&) { return true; } catch (...) { return false; }
	}
};

// Runs make(budget) on an inline scheduler with a slice of `ns`. With `cancel_after`, the token is set
// once that many handles have run, and the search runs on from there.
template <typename T, typename Make>
outcome<T> drive(Make make, std::chrono::nanoseconds ns, std::size_t cancel_after = std::size_t(-1), std::function<void()> at_cancel = {}) {
	task::inline_scheduler sched;
	task::cancel_source cancel;
	task::budget b(ns, cancel.token());
	outcome<T> out;
	task::spawn(task::scheduler_ref::of(sched), make(b), [&](std::exception_ptr e, std::optional<T> v) {
		out.error = e;
		out.value = std::move(v);
		out.done = true;
	});
	if (cancel_after != std::size_t(-1)) {
		std::size_t calls = 0;
		out.resumes += sched.run_until([&] { return calls++ >= cancel_after; });
	}
	if (cancel_after != std::size_t(-1) && !out.done) {
		if (at_cancel) { at_cancel(); }
		cancel.request();
	}
	out.resumes += sched.run();
	return out;
}

const std::chrono::nanoseconds slices[] = {std::chrono::nanoseconds(0), std::chrono::nanoseconds(3000), one_hour};

// --- 1. identity ------------------------------------------------------------------------------------

query random_query(std::mt19937_64& r) {
	query q;
	const char* kinds[] = {"TERM", "OR", "AND", "AND_MAYBE", "AND_NOT", "PHRASE"};
	q.kind = kinds[r() % 6];
	const std::size_t want = q.kind == "TERM" ? 1 : q.kind == "PHRASE" ? 2 + r() % 2 : q.kind == "OR" || q.kind == "AND" ? 2 + r() % 3 : 2;
	std::set<std::string> used;
	while (q.terms.size() < want) {
		std::string t = vocab[r() % 9];
		if (q.kind == "PHRASE" || used.insert(t).second) { q.terms.push_back(t); }
	}
	return q;
}

// One aggregation request over a match: slot 0 is the docid modulo 7 (terms), slot 1 a real from it.
struct match_document final : agg::document {
	std::uint64_t docid = 0;
	mutable std::uint64_t u = 0;
	mutable double d = 0;
	agg::values slot(agg::slot_t s) const override {
		if (s == 0) {
			u = docid % 7;
			return std::span<const std::uint64_t>(&u, 1);
		}
		d = double(docid) * 0.25;
		return std::span<const double>(&d, 1);
	}
};

agg::request aggregation() {
	agg::request req;
	agg::spec t;
	t.type = agg::kind::terms;
	t.name = "t";
	t.slot = 0;
	const auto tn = req.add(agg::request::root, t);
	agg::spec st;
	st.type = agg::kind::stats;
	st.name = "stats";
	st.slot = 1;
	req.add(tn, st);
	agg::spec h;
	h.type = agg::kind::histogram;
	h.name = "h";
	h.slot = 1;
	h.interval = agg::number(std::in_place_type<double>, 50.0);
	req.add(agg::request::root, h);
	agg::spec avg;
	avg.type = agg::kind::avg;
	avg.name = "avg";
	avg.slot = 1;
	req.add(agg::request::root, avg);
	return req;
}

struct answer {
	search_result result;
	std::string aggregates;
	bool operator==(const answer& o) const { return result.hits == o.result.hits && result.count == o.result.count && aggregates == o.aggregates; }
};

// The variants of an exhaustive search plus the pruned one, each as a function of the slice.
struct variant {
	search_options options;
	bool aggregate = false;
	std::shared_ptr<agg::partial> partial;
};

void identity_on(const multi_snapshot& multi, const std::map<std::uint64_t, std::string>& key_of, const query& q, bool xapian, bool large, std::size_t& compared, std::size_t& yielded) {
	const auto build = builder_for(q, multi, xapian);
	const agg::request req = aggregation();
	for (const std::size_t k : {std::size_t(0), std::size_t(10), std::size_t(2000)}) {
		for (int v = 0; v < 6; ++v) {
			// 0 pruned and counted; 1 pruned, no count; 2 key; 3 descending docids; 4 visitor feeding aggregations; 5 key and visitor.
			std::shared_ptr<agg::partial> partial;
			auto options = [&]() {
				search_options o;
				o.k = k;
				o.count = v == 0;
				if (v == 2 || v == 5) { o.key = [&](const match& m) { return key_of.at(m.docid); }; }
				if (v == 3) { o.order = docid_order::descending; }
				if (v == 4 || v == 5) {
					partial = std::make_shared<agg::partial>(req);
					o.visit = [partial](const match& m) {
						match_document doc;
						doc.docid = m.docid;
						partial->collect(doc);
					};
				}
				return o;
			};
			auto render = [&]() { return partial ? agg::render(req, partial->finish()) : std::string(); };
			const auto reference = [&]() {
				answer a;
				a.result = search(multi, build, options());
				a.aggregates = render();
				return a;
			}();
			for (const auto ns : slices) {
				const auto o = options();
				const auto got = drive<search_result>([&](task::budget& b) { return search_task(multi, build, o, slice{b}); }, ns);
				CHECK(got.done && got.value && !got.error);
				if (!got.value) { continue; }
				answer a{std::move(*got.value), render()};
				CHECK(a == reference);  // exactly: ids, order, score bits, counts, aggregates
				++compared;
				yielded += got.yields();
				if (ns.count() == 0 && large && (v == 2 || v == 4)) { CHECK(got.yields() > 0); }
			}
			// The owning form agrees too.
			const auto o = options();
			auto pin = std::make_shared<int>(0);
			const auto got = drive<search_result>([&](task::budget& b) { return search_pinned(pin, multi, build, o, b); }, std::chrono::nanoseconds(0));
			CHECK(got.value && got.value->hits == reference.result.hits && got.value->count == reference.result.count);
			CHECK(pin.use_count() == 1);
		}
		const auto reference_count = search_count(multi.segments(), build);
		for (const auto ns : slices) {
			const auto got = drive<std::uint64_t>([&](task::budget& b) { return search_count_task(multi.segments(), build, slice{b}); }, ns);
			CHECK(got.value && *got.value == reference_count);
			++compared;
		}
	}
}

void identity(int rounds) {
	std::mt19937_64 r(8128);
	std::size_t compared = 0, yielded = 0;
	for (int round = 0; round < rounds; ++round) {
		const std::size_t n = 1 + r() % 4;
		const auto docs = random_docs(r, n, 150 + r() % 300, round % 3 == 0);
		std::set<std::pair<std::size_t, std::uint64_t>> dead;
		if (round % 2 == 1) {
			for (const auto& d : docs) {
				if (r() % 8 == 0) { dead.insert({d.shard, d.did}); }
			}
		}
		const auto set = build_shards(docs, n, r, 1 + round % 3, dead);
		const multi_snapshot multi(set.inputs());
		std::map<std::uint64_t, std::string> key_of;
		for (const auto& d : docs) { key_of[interleave_docid(d.did, d.shard, n)] = d.key; }
		for (int qn = 0; qn < 6; ++qn) { identity_on(multi, key_of, random_query(r), qn % 2 == 1, false, compared, yielded); }
	}
	std::printf("  identity: %zu comparisons, %zu yields\n", compared, yielded);
	CHECK(compared > 1500 && yielded > 1000);
}

// --- a corpus of several windows per segment ---------------------------------------------------------

std::vector<doc> big_docs(std::mt19937_64& r, std::size_t n, std::size_t count) {
	std::vector<doc> docs;
	std::vector<std::uint64_t> next(n, 0);
	const double odds[] = {0.7, 0.5, 0.3, 0.1, 0.02, 0.005};  // a b c d e rare
	const char* names[] = {"a", "b", "c", "d", "e", "rare"};
	for (std::size_t i = 0; i < count; ++i) {
		doc d;
		d.shard = r() % n;
		d.did = ++next[d.shard];
		d.key = shards_fixture::keys[r() % shards_fixture::keys.size()];
		const std::size_t len = 2 + r() % 6;
		for (std::size_t t = 0; t < len; ++t) {
			const std::size_t w = r() % 6;
			d.terms.push_back(double(r() % 1000) / 1000.0 < odds[w] ? names[w] : "x");
		}
		docs.push_back(std::move(d));
	}
	return docs;
}

void big(std::size_t n, unsigned parts, std::size_t count, bool deletes) {
	std::mt19937_64 r(77 + n);
	const auto docs = big_docs(r, n, count);
	std::set<std::pair<std::size_t, std::uint64_t>> dead;
	for (const auto& d : docs) {
		if (deletes && r() % 50 == 0) { dead.insert({d.shard, d.did}); }
	}
	const auto set = build_shards(docs, n, r, parts, dead);
	const multi_snapshot multi(set.inputs());
	std::map<std::uint64_t, std::string> key_of;
	for (const auto& d : docs) { key_of[interleave_docid(d.did, d.shard, n)] = d.key; }
	std::size_t windows = 0;
	for (const auto& s : multi.segments()) { windows += (s.seg->core().doc_count() + 4095) / 4096; }
	std::printf("  big corpus: %zu shards, %zu documents, %zu segments, %zu windows\n", n, docs.size(), multi.segments().size(), windows);
	CHECK(windows >= 5);
	std::size_t compared = 0, yielded = 0;
	const std::vector<query> queries = {{"TERM", {"a"}}, {"OR", {"a", "b", "c"}}, {"OR", {"b", "rare"}}, {"AND", {"a", "b"}}, {"AND", {"a", "b", "c"}},
		{"AND_MAYBE", {"a", "b"}}, {"AND_NOT", {"a", "c"}}, {"PHRASE", {"a", "b"}}, {"TERM", {"rare"}}};
	for (std::size_t qi = 0; qi < queries.size(); ++qi) {
		const auto& q = queries[qi];
		identity_on(multi, key_of, q, qi % 2 == 0, true, compared, yielded);
		// The model, which shares no code with the collectors: score every live match.
		const auto build = builder_for(q, multi, qi % 2 == 0);
		auto all = model(multi, build, nullptr);
		sort_model(all, false, false, docid_order::ascending);
		for (const std::size_t k : {std::size_t(10), std::size_t(1000)}) {
			search_options o;
			o.k = k;
			o.count = true;
			for (const auto ns : slices) {
				const auto got = drive<search_result>([&](task::budget& b) { return search_task(multi, build, o, slice{b}); }, ns);
				CHECK(got.value && got.value->count && *got.value->count == all.size());
				std::vector<doc_hit> expect;
				for (std::size_t i = 0; i < std::min(k, all.size()); ++i) { expect.push_back({all[i].docid, all[i].score}); }
				CHECK(got.value && same_hits(got.value->hits, expect));
			}
		}
		// Counting by intersection or window is sliced too: a counted top-k resumes about once per window,
		// twice where the count and the scoring are separate passes over a segment without deletions.
		if (q.kind != "TERM" && q.kind != "AND_MAYBE") {  // a term's count is its frequency; so is a required term's
			search_options o;
			o.k = 10;
			o.count = true;
			const auto got = drive<search_result>([&](task::budget& b) { return search_task(multi, build, o, slice{b}); }, std::chrono::nanoseconds(0));
			CHECK(got.value && !got.error);
			CHECK(got.yields() * 10 >= windows * 9);
			if (!deletes && ((q.kind == "OR" && q.terms.size() == 3) || q.kind == "AND_NOT")) { CHECK(got.yields() * 10 >= windows * 18); }
			const auto counted = drive<std::uint64_t>([&](task::budget& b) { return search_count_task(multi.segments(), build, slice{b}); }, std::chrono::nanoseconds(0));
			CHECK(counted.value && *counted.value == all.size());
			CHECK(counted.yields() * 10 >= windows * 9);
		}
		// Reading every posting (k above the corpus, no count, so nothing prunes) resumes about once per
		// window, whichever scorer drives it; an exhaustive scan does too.
		for (const bool exhaustive : {false, true}) {
			search_options o;
			o.k = 100000;
			std::uint64_t visited = 0;
			if (exhaustive) { o.visit = [&](const match&) { ++visited; }; }
			const auto got = drive<search_result>([&](task::budget& b) { return search_task(multi, build, o, slice{b}); }, std::chrono::nanoseconds(0));
			CHECK(got.value && !got.error);
			// A rare term's postings fit one block, which is bounded work however many windows it spans.
			if (!(q.kind == "TERM" && q.terms[0] == "rare")) { CHECK(got.yields() * 10 >= windows * 9); }
			// Against the model, not the synchronous search, which shares the code under test.
			CHECK(got.value && same_hits(got.value->hits, [&] {
				std::vector<doc_hit> all_hits;
				for (const auto& row : all) { all_hits.push_back({row.docid, row.score}); }
				return all_hits;
			}()));
			if (exhaustive) { CHECK(visited == all.size() && got.value && got.value->count && *got.value->count == all.size()); }
			yielded += got.yields();
		}
	}
	std::printf("  big identity: %zu comparisons, %zu yields\n", compared, yielded);
	CHECK(yielded > windows);
}

// A reordered segment (permuted docids): groups of blocks in term order, and the generic loop on ordinals
// that are not in docid order.
void reordered() {
	std::mt19937_64 r(31);
	std::vector<doc> docs;
	constexpr std::size_t count = 20000;
	for (std::size_t d = 0; d < count; ++d) {
		doc x;
		x.did = 1 + (d * 7919) % count;
		for (std::size_t t = 0; t < 2 + r() % 4; ++t) { x.terms.push_back(r() % 100 < 70 ? (r() % 2 ? "a" : "b") : "x"); }
		docs.push_back(std::move(x));
	}
	segment_writer w;
	w.permuted = true;
	for (const auto& d : docs) {
		const auto p = postings_of(d);
		w.add(d.did, p.inputs(), p.length);
	}
	const std::string bytes = w.flush({});
	const segment seg(bytes);
	CHECK(!seg.core().docid_ordered());
	const multi_snapshot multi(std::vector<shard_input>{{{segment_snapshot{&seg, nullptr, 0, &text_table()}}, 0}});
	for (const query& q : std::vector<query>{{"TERM", {"a"}}, {"OR", {"a", "b"}}, {"AND", {"a", "b"}}, {"AND_MAYBE", {"a", "b"}}, {"PHRASE", {"a", "b"}}}) {
		const auto build = builder_for(q, multi, false);
		auto all = model(multi, build, nullptr);
		sort_model(all, false, false, docid_order::ascending);
		for (const std::size_t k : {std::size_t(10), std::size_t(100000)}) {
			search_options o;
			o.k = k;
			o.count = k == 10;
			const auto reference = search(multi, build, o);
			std::vector<doc_hit> expect;
			for (std::size_t i = 0; i < std::min(k, all.size()); ++i) { expect.push_back({all[i].docid, all[i].score}); }
			CHECK(same_hits(reference.hits, expect));
			for (const auto ns : slices) {
				const auto got = drive<search_result>([&](task::budget& b) { return search_task(multi, build, o, slice{b}); }, ns);
				CHECK(got.value && got.value->hits == reference.hits && got.value->count == reference.count);
				if (ns.count() == 0 && k == 100000) { CHECK(got.yields() >= 3); }
			}
		}
	}
}

// --- 3. cancellation and the pin -------------------------------------------------------------------

void cancellation() {
	std::mt19937_64 r(5);
	constexpr std::size_t n = 2;
	const auto docs = big_docs(r, n, 40000);
	const auto set = build_shards(docs, n, r, 1);
	const multi_snapshot multi(set.inputs());
	const query q{"OR", {"a", "b"}};
	const auto build = builder_for(q, multi, false);
	for (const int variant : {0, 1}) {  // pruned top-k and counted; exhaustive with a visitor
		std::atomic<std::uint64_t> visited{0};
		search_options o;
		o.k = 10;
		o.count = true;
		if (variant == 1) { o.visit = [&](const match&) { ++visited; }; }
		auto pin = std::make_shared<int>(0);
		std::uint64_t at_cancel = 0;
		long pinned_while_suspended = 0;
		const auto got = drive<search_result>([&](task::budget& b) { return search_pinned(pin, multi, build, o, b); }, std::chrono::nanoseconds(0), 3, [&] {
			at_cancel = visited;
			pinned_while_suspended = pin.use_count();
		});
		CHECK(got.cancelled() && !got.value && got.done);
		CHECK(got.resumes >= 3 && got.resumes <= 5);  // stopped within a checkpoint or two of the request
		CHECK(pinned_while_suspended >= 2);           // the frame holds the view while the search waits
		CHECK(pin.use_count() == 1);                  // and lets it go when the search is cancelled
		CHECK(visited == at_cancel);                  // no host callback after the request
		if (variant == 1) { CHECK(at_cancel > 0 && at_cancel < docs.size()); }
		// Uncancelled, the same search finishes.
		const auto full = drive<search_result>([&](task::budget& b) { return search_pinned(pin, multi, build, o, b); }, std::chrono::nanoseconds(0));
		CHECK(full.value && !full.error && pin.use_count() == 1);
	}
	// A token set before the search starts stops it at its first checkpoint.
	{
		auto pin = std::make_shared<int>(0);
		task::inline_scheduler sched;
		task::cancel_source cancel;
		cancel.request();
		task::budget b(one_hour, cancel.token());
		search_options o;
		o.k = 10;
		o.count = true;
		std::exception_ptr error;
		task::spawn(task::scheduler_ref::of(sched), search_pinned(pin, multi, build, o, b), [&](std::exception_ptr e, std::optional<search_result>) { error = e; });
		sched.run();
		CHECK(error != nullptr);
		CHECK(pin.use_count() == 1);
	}
	// Destroying a task that is suspended between slices frees its view.
	{
		auto pin = std::make_shared<int>(0);
		task::inline_scheduler sched;
		task::budget b(std::chrono::nanoseconds(0));
		search_options o;
		o.k = 10;
		o.count = true;
		{
			auto t = search_pinned(pin, multi, build, o, b);
			task::running_on scope(task::scheduler_ref::of(sched));
			t.handle().resume();  // runs to its first checkpoint, which requeues it
			CHECK(!t.done() && sched.queued() == 1 && pin.use_count() == 2);
		}
		CHECK(pin.use_count() == 1);
	}
}

// --- 4. sharing a loop and moving between threads ---------------------------------------------------

void sharing() {
	std::mt19937_64 r(9);
	constexpr std::size_t n = 2;
	const auto docs = big_docs(r, n, 40000);
	const auto set = build_shards(docs, n, r, 1);
	const multi_snapshot multi(set.inputs());
	const query heavy_q{"OR", {"a", "b", "c"}}, light_q{"TERM", {"rare"}};
	const auto heavy_b = builder_for(heavy_q, multi, false), light_b = builder_for(light_q, multi, false);
	task::inline_scheduler sched;
	task::budget budget(std::chrono::nanoseconds(0));
	std::vector<int> finished;
	search_options oh, ol;
	oh.k = ol.k = 10;
	oh.count = ol.count = true;
	// The heavy search starts first; the light one, queued behind it, finishes before it.
	task::spawn(task::scheduler_ref::of(sched), search_pinned(0, multi, heavy_b, oh, budget), [&](std::exception_ptr e, std::optional<search_result>) { CHECK(!e); finished.push_back(1); });
	task::spawn(task::scheduler_ref::of(sched), search_pinned(0, multi, light_b, ol, budget), [&](std::exception_ptr e, std::optional<search_result>) { CHECK(!e); finished.push_back(2); });
	sched.run();
	CHECK((finished == std::vector<int>{2, 1}));
}

// A scheduler whose resumes run on whichever worker is free.
class pool_scheduler {
public:
	explicit pool_scheduler(std::size_t threads) {
		for (std::size_t i = 0; i < threads; ++i) { workers_.emplace_back([this] { work(); }); }
	}
	~pool_scheduler() {
		{
			std::lock_guard lock(m_);
			stop_ = true;
		}
		cv_.notify_all();
		for (auto& w : workers_) { w.join(); }
	}
	void post(std::coroutine_handle<> h) {
		{
			std::lock_guard lock(m_);
			q_.push_back(h);
		}
		cv_.notify_one();
	}
	std::uint64_t now() const noexcept { return 0; }

private:
	void work() {
		for (;;) {
			std::coroutine_handle<> h;
			{
				std::unique_lock lock(m_);
				cv_.wait(lock, [&] { return stop_ || !q_.empty(); });
				if (q_.empty()) { return; }
				h = q_.front();
				q_.pop_front();
			}
			task::running_on scope(task::scheduler_ref::of(*this));
			h.resume();
		}
	}
	std::mutex m_;
	std::condition_variable cv_;
	std::deque<std::coroutine_handle<>> q_;
	bool stop_ = false;
	std::vector<std::thread> workers_;
};

void threads() {
	std::mt19937_64 r(21);
	constexpr std::size_t n = 2;
	const auto docs = big_docs(r, n, 30000);
	const auto set = build_shards(docs, n, r, 2);
	const multi_snapshot multi(set.inputs());
	const std::vector<query> queries = {{"OR", {"a", "b"}}, {"AND", {"a", "c"}}, {"TERM", {"b"}}, {"AND_NOT", {"a", "d"}}};
	std::vector<scorer_builder> builds;
	std::vector<search_result> reference;
	search_options o;
	o.k = 25;
	o.count = true;
	for (const auto& q : queries) {
		builds.push_back(builder_for(q, multi, true));
		reference.push_back(search(multi, builds.back(), o));
	}
	std::mutex m;
	std::condition_variable cv;
	std::size_t finished = 0;
	std::vector<std::optional<search_result>> got(queries.size() * 3);
	{
		pool_scheduler pool(3);
		std::vector<task::budget> budgets(got.size(), task::budget(std::chrono::nanoseconds(0)));
		for (std::size_t i = 0; i < got.size(); ++i) {
			task::spawn(task::scheduler_ref::of(pool), search_pinned(0, multi, builds[i % queries.size()], o, budgets[i]), [&, i](std::exception_ptr e, std::optional<search_result> v) {
				CHECK(!e);
				got[i] = std::move(v);
				std::lock_guard lock(m);
				++finished;
				cv.notify_all();
			});
		}
		std::unique_lock lock(m);
		cv.wait(lock, [&] { return finished == got.size(); });
	}
	for (std::size_t i = 0; i < got.size(); ++i) { CHECK(got[i] && got[i]->hits == reference[i % queries.size()].hits && got[i]->count == reference[i % queries.size()].count); }
}

}  // namespace

int main() {
	identity(std::getenv("SLICED_ROUNDS") ? std::atoi(std::getenv("SLICED_ROUNDS")) : 12);
	big(3, 2, 54000, true);  // several segments per shard, with deletions: the multi-segment collector, live_scorer
	big(1, 1, 24000, false);  // one segment, no deletions: top_k alone, and the scorers' own bulk loops
	reordered();
	cancellation();
	sharing();
	threads();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures.load());
		return 1;
	}
	std::printf("sliced_test ok\n");
	return 0;
}
