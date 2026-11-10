// An allocation-counting build of any engine on glibc (PLAN, "Ratchets", Memory): preloaded
// with LD_PRELOAD, it counts every heap allocation (malloc, calloc, realloc, the aligned
// forms; operator new goes through malloc) and its requested bytes, and on every read of
// stdin appends the running totals to ALLOC_LOG. A serve loop reads one query a line, so the
// differences between consecutive log lines are each query's allocations (bench/allocs.py).
//   clang -O2 -shared -fPIC -o alloc_count.so alloc_count.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);
extern void __libc_free(void*);

static _Atomic unsigned long long count, bytes;
static int log_fd = -1;

__attribute__((constructor)) static void open_log(void) {
	const char* path = getenv("ALLOC_LOG");
	if (path) { log_fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644); }
}

static void note(size_t n) { atomic_fetch_add_explicit(&count, 1, memory_order_relaxed); atomic_fetch_add_explicit(&bytes, n, memory_order_relaxed); }

void* malloc(size_t n) { note(n); return __libc_malloc(n); }
void* calloc(size_t a, size_t b) { note(a * b); return __libc_calloc(a, b); }
void* realloc(void* p, size_t n) { note(n); return __libc_realloc(p, n); }
void free(void* p) { __libc_free(p); }
void* memalign(size_t align, size_t n) { note(n); return __libc_memalign(align, n); }
void* aligned_alloc(size_t align, size_t n) { note(n); return __libc_memalign(align, n); }
int posix_memalign(void** out, size_t align, size_t n) {
	note(n);
	void* p = __libc_memalign(align, n);
	if (!p) { return ENOMEM; }
	*out = p;
	return 0;
}

// Two decimal numbers and a newline, without stdio, which may allocate.
static size_t put(char* at, unsigned long long v) {
	char digits[24];
	size_t n = 0, k = 0;
	do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) { at[k++] = digits[--n]; }
	return k;
}

ssize_t read(int fd, void* buf, size_t n) {
	if (fd == 0 && log_fd >= 0) {
		char line[56];
		size_t k = put(line, atomic_load(&count));
		line[k++] = ' ';
		k += put(line + k, atomic_load(&bytes));
		line[k++] = '\n';
		syscall(SYS_write, log_fd, line, k);
	}
	return (ssize_t)syscall(SYS_read, fd, buf, n);
}
