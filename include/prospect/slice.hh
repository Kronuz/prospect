// The time slice a search runs in (docs/mechanisms/sliced-search.md): searches are coroutines that
// await slice::checkpoint() at window boundaries, so a long query shares its reactor with the
// rest of the loop. A slice without a budget never suspends, which is how the synchronous entry
// points run the same coroutines to completion.
#pragma once

#include "task.hh"

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace prospect {

// Windows of this many ordinals are the unit a scan yields at.
inline constexpr std::uint64_t window_ordinals = 4096;

class slice {
	using checkpoint_type = decltype(std::declval<task::budget&>().checkpoint());

public:
	slice() = default;
	explicit slice(task::budget& b) noexcept : budget_(&b) {}

	bool bounded() const noexcept { return budget_ != nullptr; }

	// Free while the slice lasts; once it is spent, requeues the search as demoted. Throws
	// task::cancelled when the budget's token is set, spent or not.
	struct awaiter {
		std::optional<checkpoint_type> inner;
		bool await_ready() noexcept { return !inner || inner->await_ready(); }
		void await_suspend(std::coroutine_handle<> h) { inner->await_suspend(h); }
		void await_resume() {
			if (inner) { inner->await_resume(); }
		}
	};
	awaiter checkpoint() const noexcept {
		awaiter a;
		if (budget_) { a.inner.emplace(budget_->checkpoint()); }
		return a;
	}

private:
	task::budget* budget_ = nullptr;
};

// Yields at most once per window of ordinals in a scan whose positions only grow: due(d) is true
// the first time d reaches a window not yet checked.
class window_gate {
public:
	bool due(std::uint64_t ord) noexcept {
		if (ord < next_) { return false; }
		next_ = (ord | (window_ordinals - 1)) + 1;
		return true;
	}

private:
	std::uint64_t next_ = 0;
};

// Runs a search coroutine to completion on the calling thread. Its slice must be unbounded:
// a coroutine that suspends cannot finish here.
template <typename T>
T run_unsliced(task::task<T> t) {
#if !TASK_SYMMETRIC_TRANSFER
	// Inside a trampoline loop a transfer only queues; run this task on a fresh loop.
	task::detail::trampoline outer;
	task::detail::trampoline& local = task::detail::local_trampoline();
	const bool nested = local.active;
	if (nested) { std::swap(local, outer); }
	struct restore {
		task::detail::trampoline& local;
		task::detail::trampoline& outer;
		bool nested;
		~restore() {
			if (nested) { std::swap(local, outer); }
		}
	} guard{local, outer, nested};
#endif
	auto h = t.handle();
	h.resume();
	assert(h.done() && "an unsliced search suspended");
	return h.promise().take();
}

}  // namespace prospect
