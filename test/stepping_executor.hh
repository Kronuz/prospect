// A deterministic executor for crash enumeration (lifecycle review condition 7, R2). Work
// runs on one helper thread, but only while the test thread waits in step(): a baton
// passes between the two, so exactly one runs at any moment and a run replays exactly.
// Work yields the baton back at every checkpoint() and sleep_until() (each merge write
// chunk and each rate-limiter wait), so the test interleaves single pieces of merge work
// with the writer's steps at chosen points. The clock is manual: sleep_until() advances it.
#pragma once

#include "strata/core/executor.hh"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace strata::testing {

class stepping_executor final : public executor {
	struct entry {
		work fn;
		task::cancel_token stop;
		const void* owner = nullptr;
	};

public:
	stepping_executor() : worker_([this] { loop(); }) {}
	stepping_executor(const stepping_executor&) = delete;
	stepping_executor& operator=(const stepping_executor&) = delete;
	// Every group must have ended, so no work is in progress.
	~stepping_executor() override {
		{
			std::lock_guard l(mu_);
			exit_ = true;
			worker_turn_ = true;
		}
		cv_.notify_all();
		worker_.join();
	}

	std::unique_ptr<group> open() override { return std::make_unique<stepping_group>(*this); }
	std::uint64_t now() override { return now_; }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		if (t > now_) { now_ = t; }
		yield();
		return !stop.requested();
	}
	bool checkpoint(const task::cancel_token& stop) override {
		yield();
		return !stop.requested();
	}

	// Runs the work in progress to its next yield or its end, or else starts the oldest
	// queued work and runs it as far; false when there is none.
	bool step() {
		if (!running_) {
			if (queue_.empty()) { return false; }
			current_ = std::move(queue_.front());
			queue_.pop_front();
			running_ = true;
		}
		std::unique_lock l(mu_);
		worker_turn_ = true;
		cv_.notify_all();
		cv_.wait(l, [&] { return !worker_turn_; });
		if (finished_) {
			finished_ = false;
			running_ = false;
			current_ = {};
		}
		return true;
	}
	std::size_t steps_pending() const noexcept { return queue_.size() + (running_ ? 1 : 0); }

private:
	class stepping_group final : public group {
	public:
		explicit stepping_group(stepping_executor& e) : e_(&e) {}
		~stepping_group() override { cancel(); }
		void post(work w) override { e_->queue_.push_back({std::move(w), source_.token(), this}); }
		void cancel() noexcept override {
			source_.request();
			std::erase_if(e_->queue_, [&](const entry& q) { return q.owner == this; });
			while (e_->running_ && e_->current_.owner == this) { e_->step(); }
			source_ = task::cancel_source();
		}
		void wait() override {
			while (mine()) { e_->step(); }
		}

	private:
		bool mine() const {
			if (e_->running_ && e_->current_.owner == this) { return true; }
			for (const auto& q : e_->queue_) {
				if (q.owner == this) { return true; }
			}
			return false;
		}
		stepping_executor* e_;
		task::cancel_source source_;
	};

	// On the worker: hands the baton back and waits for it.
	void yield() {
		std::unique_lock l(mu_);
		worker_turn_ = false;
		cv_.notify_all();
		cv_.wait(l, [&] { return worker_turn_; });
	}
	void loop() {
		std::unique_lock l(mu_);
		for (;;) {
			cv_.wait(l, [&] { return worker_turn_; });
			if (exit_) { return; }
			work fn = std::move(current_.fn);
			const task::cancel_token stop = current_.stop;
			l.unlock();
			try {
				fn(stop);
			} catch (...) {
			}
			fn = nullptr;
			l.lock();
			finished_ = true;
			worker_turn_ = false;
			cv_.notify_all();
		}
	}

	// The test thread's state; the worker reads current_ and now_ only while it holds the
	// baton, after the handoff under mu_.
	std::deque<entry> queue_;
	entry current_;
	bool running_ = false;
	std::uint64_t now_ = 0;
	std::mutex mu_;
	std::condition_variable cv_;
	bool worker_turn_ = false, finished_ = false, exit_ = false;  // under mu_
	std::thread worker_;  // last: it starts once the rest exists
};

}  // namespace strata::testing
