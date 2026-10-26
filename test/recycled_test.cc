// recycled<T>: the per-thread block stack keeps at most 8 blocks, frees them at thread exit,
// takes a block back after a constructor throws, and lets another thread free a block. Blocks of
// the test types are counted by a replaced global operator new/delete. Sanitizer builds bypass
// the stack (prospect::recycle_blocks), so they expect no block to outlive its object.
#include "prospect/reader.hh"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t slots = 4096;
std::mutex mu;
void* tracked[slots];
std::size_t live_blocks = 0;
std::size_t tracked_size = 0;  // set before any thread starts

void* take(std::size_t n) {
	void* p = std::malloc(n ? n : 1);
	if (!p) { throw std::bad_alloc(); }
	if (n == tracked_size) {
		std::lock_guard lock(mu);
		for (auto& t : tracked) {
			if (!t) {
				t = p;
				++live_blocks;
				break;
			}
		}
	}
	return p;
}
void give(void* p) noexcept {
	if (tracked_size && p) {
		std::lock_guard lock(mu);
		for (auto& t : tracked) {
			if (t == p) {
				t = nullptr;
				--live_blocks;
				break;
			}
		}
	}
	std::free(p);
}
std::size_t live() {
	std::lock_guard lock(mu);
	return live_blocks;
}

struct blob : prospect::recycled<blob> {
	char bytes[1500];
};
struct throwing : prospect::recycled<throwing> {
	char bytes[1600];
	explicit throwing(bool t) { bytes[0] = 0; if (t) { throw std::runtime_error("ctor"); } }
};
static_assert(sizeof(blob) != sizeof(throwing));

int failures = 0;
void check(bool ok, const char* what, long got = 0) {
	if (!ok) {
		std::printf("FAIL: %s (got %ld)\n", what, got);
		++failures;
	}
}
// Blocks left alive by one thread after it exits and its objects are freed.
template <class F>
void in_thread(F f) {
	std::thread(f).join();
}
}  // namespace

// TSan's runtime defines the replaceable operators itself, so the counts can't be taken there.
#if defined(__SANITIZE_THREAD__)
#define RECYCLED_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RECYCLED_TEST_TSAN 1
#endif
#endif

#ifndef RECYCLED_TEST_TSAN
void* operator new(std::size_t n) { return take(n); }
void* operator new[](std::size_t n) { return take(n); }
void operator delete(void* p) noexcept { give(p); }
void operator delete(void* p, std::size_t) noexcept { give(p); }
void operator delete[](void* p) noexcept { give(p); }
void operator delete[](void* p, std::size_t) noexcept { give(p); }
#endif

int main() {
#ifdef RECYCLED_TEST_TSAN
	std::printf("recycled: SKIP under TSan (no allocation counts)\n");
	return 77;
#endif
	// One size is tracked at a time: blob's, then throwing's.
	tracked_size = sizeof(blob);
	const std::size_t kept = prospect::recycle_blocks ? 8 : 0;

	// 1. The stack keeps at most 8 blocks; the 9th and later are freed.
	in_thread([&] {
		std::vector<blob*> v;
		for (int i = 0; i < 20; ++i) { v.push_back(new blob); }
		check(live() == 20, "20 live blocks", long(live()));
		for (auto* p : v) { delete p; }
		check(live() == kept, "cap of 8 kept after 20 freed", long(live()));
		// A block taken from the stack comes back without a new allocation.
		std::vector<blob*> w;
		for (int i = 0; i < 8; ++i) { w.push_back(new blob); }
		check(live() == 8, "8 new blocks reuse the kept ones", long(live()));
		for (auto* p : w) { delete p; }
		check(live() == kept, "still the cap after reuse", long(live()));
		if (prospect::recycle_blocks) {
			// LIFO: the block freed last is the next one handed out.
			auto* a = new blob;
			auto* b = new blob;
			delete a;
			delete b;
			auto* c = new blob;
			check(c == b, "last freed block is taken first");
			delete c;
		}
	});

	// 2. Thread exit releases the stack: nothing stays after 100 short-lived threads.
	for (int round = 0; round < 100; ++round) {
		in_thread([] { for (int i = 0; i < 5; ++i) { delete new blob; } });
	}
	check(live() == 0, "no blocks after threads exit", long(live()));

	// 3. A block freed on another thread: bounded there, released when that thread exits.
	{
		std::vector<blob*> made;
		in_thread([&] { for (int i = 0; i < 1000; ++i) { made.push_back(new blob); } });
		check(live() == 1000, "1000 blocks made on one thread", long(live()));
		in_thread([&] {
			for (auto* p : made) { delete p; }
			check(live() == kept, "freeing thread keeps at most 8", long(live()));
		});
		check(live() == 0, "all released when the freeing thread exits", long(live()));
	}

	// 4. A constructor that throws gives the block back: nothing grows over many failures.
	tracked_size = sizeof(throwing);
	const std::size_t one = prospect::recycle_blocks ? 1 : 0;
	in_thread([&] {
		delete new throwing(false);
		check(live() == one, "one kept block", long(live()));
		for (int i = 0; i < 100; ++i) {
			try {
				(void)new throwing(true);
				check(false, "constructor should throw");
			} catch (const std::runtime_error&) {
			}
		}
		check(live() == one, "throwing constructors keep the count", long(live()));
	});
	check(live() == 0, "no blocks after the throwing thread exits", long(live()));
	// With an empty stack, a throwing constructor's block is stashed (or freed when bypassed).
	in_thread([&] {
		for (int i = 0; i < 3; ++i) {
			try { (void)new throwing(true); } catch (const std::runtime_error&) {}
		}
		check(live() == one, "empty stack, throwing constructor", long(live()));
	});
	check(live() == 0, "no blocks after the second throwing thread", long(live()));

	if (failures) { return 1; }
	std::printf("recycled_test ok (recycling %s)\n", prospect::recycle_blocks ? "on" : "off");
	return 0;
}
