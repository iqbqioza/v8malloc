/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-06 large-allocation latency benchmark (benchmarks.md §2.6).
 * Measures the per-allocation cost of the Large / Huge mmap path:
 * how long an `alloc(size)` call takes (mmap + region-map insert),
 * how long the first-touch `memset` takes (kernel page faults +
 * physical-page allocation), and how long `free` takes (region-map
 * remove + munmap).
 *
 * The pass criterion (benchmarks.md §6) is a median allocation
 * latency at ≥1 MiB no worse than 0.5× glibc. To make that ratio
 * legible, the bench reports BOTH the median (p50) and the tail
 * (p99) for each phase across every size.
 *
 * Single-thread by design. The Large path goes through the page
 * heap's region map (mutex-protected bounded array); a multi-
 * threaded variant (the spec's "threads: 1, 8") needs the
 * page-heap region map to grow a finer-grained synchronization
 * story before it gives a fair number, and lands as a follow-on.
 *
 * Sizes match the spec exactly: 256 KiB / 512 KiB / 1 MiB / 2 MiB
 * / 4 MiB / 16 MiB / 64 MiB / 256 MiB. The first row falls just
 * inside the buddy-eligible range (256 KiB == V8M_BUDDY_MAX_BLOCK)
 * so the bench surfaces the buddy/Large boundary as well.
 *
 * Output is one line per size, parseable into a CSV-style ingest:
 *
 *   size_bytes  iters  alloc_us_p50 alloc_us_p99 fault_us_p50 fault_us_p99
 * free_us_p50 free_us_p99 1048576     100    18           42           312 580
 * 12          28
 *
 * Knobs (env vars):
 *   V8M_BENCH_ITERS         per-size iteration count, default 100
 *   V8M_BENCH_MAX_SIZE_MB   skip rows above this size (default
 *                           256 — the spec value; lower for CI
 *                           runners with limited RAM)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — provides clock_gettime + CLOCK_MONOTONIC */

static const size_t g_sizes[] = {
    (size_t)256 * 1024,	      (size_t)512 * 1024,
    (size_t)1024 * 1024,      (size_t)2 * 1024 * 1024,
    (size_t)4 * 1024 * 1024,  (size_t)16 * 1024 * 1024,
    (size_t)64 * 1024 * 1024, (size_t)256 * 1024 * 1024,
};
static const size_t g_size_count = sizeof(g_sizes) / sizeof(g_sizes[0]);

static uint64_t now_ns(void)
{
	struct timespec time;
	/* CLOCK_MONOTONIC ships under _GNU_SOURCE which the bench
	 * CMakeLists sets globally; IWYU loses that thread. */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return ((uint64_t)time.tv_sec * 1000000000ULL) + (uint64_t)time.tv_nsec;
}

static int env_int(const char *name, int fallback)
{
	/* getenv is single-threaded by spec; the bench is too. */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	const char *value = getenv(name);
	if (value == NULL || *value == '\0') {
		return fallback;
	}
	long parsed = strtol(value, NULL, 10);
	if (parsed <= 0 || parsed > INT32_MAX) {
		return fallback;
	}
	return (int)parsed;
}

static int compare_u64(const void *lhs, const void *rhs)
{
	uint64_t left = *(const uint64_t *)lhs;
	uint64_t right = *(const uint64_t *)rhs;
	if (left < right) {
		return -1;
	}
	if (left > right) {
		return 1;
	}
	return 0;
}

/* Pick percentile from an already-sorted array. Linear interpolation
 * is overkill at the precision the bench reports (microseconds);
 * nearest-rank is fine. */
static uint64_t percentile(const uint64_t *sorted, size_t count, double pct)
{
	if (count == 0U) {
		return 0U;
	}
	size_t idx = (size_t)(((pct / 100.0) * (double)(count - 1U)) + 0.5);
	if (idx >= count) {
		idx = count - 1U;
	}
	return sorted[idx];
}

/* Three uint64 buffers and a size_t length describe distinct
 * quantities by name; struct wrapping would be overkill for one
 * internal call site. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void measure_one(size_t size, int iters, uint64_t *alloc_ns,
			uint64_t *fault_ns, uint64_t *free_ns)
{
	for (int i = 0; i < iters; i++) {
		uint64_t before_alloc = now_ns();
		void *ptr = malloc(size);
		uint64_t after_alloc = now_ns();
		if (ptr == NULL) {
			(void)fprintf(
			    stderr, "mb_06: malloc(%zu) returned NULL\n", size);
			alloc_ns[i] = 0;
			fault_ns[i] = 0;
			free_ns[i] = 0;
			continue;
		}
		(void)memset(ptr, i & 0xFF, size);
		/* Force the compiler to keep the memset by making `ptr`
		 * appear escaped — without this, the memset is followed
		 * only by free() and gcc dead-code-eliminates it,
		 * which makes the page-fault number meaningless. The
		 * empty asm with a "memory" clobber is the standard
		 * compiler-only barrier. */
		__asm__ volatile("" : : "r"(ptr) : "memory");
		uint64_t after_fault = now_ns();
		free(ptr);
		uint64_t after_free = now_ns();

		alloc_ns[i] = after_alloc - before_alloc;
		fault_ns[i] = after_fault - after_alloc;
		free_ns[i] = after_free - after_fault;
	}
}

static void report(size_t size, int iters, uint64_t *alloc_ns,
		   uint64_t *fault_ns, uint64_t *free_ns)
{
	qsort(alloc_ns, (size_t)iters, sizeof(*alloc_ns), compare_u64);
	qsort(fault_ns, (size_t)iters, sizeof(*fault_ns), compare_u64);
	qsort(free_ns, (size_t)iters, sizeof(*free_ns), compare_u64);

	uint64_t alloc_p50 = percentile(alloc_ns, (size_t)iters, 50.0) / 1000U;
	uint64_t alloc_p99 = percentile(alloc_ns, (size_t)iters, 99.0) / 1000U;
	uint64_t fault_p50 = percentile(fault_ns, (size_t)iters, 50.0) / 1000U;
	uint64_t fault_p99 = percentile(fault_ns, (size_t)iters, 99.0) / 1000U;
	uint64_t free_p50 = percentile(free_ns, (size_t)iters, 50.0) / 1000U;
	uint64_t free_p99 = percentile(free_ns, (size_t)iters, 99.0) / 1000U;

	(void)printf(
	    "%-12zu %-6d %-13llu %-13llu %-13llu %-13llu %-12llu %-12llu\n",
	    size, iters, (unsigned long long)alloc_p50,
	    (unsigned long long)alloc_p99, (unsigned long long)fault_p50,
	    (unsigned long long)fault_p99, (unsigned long long)free_p50,
	    (unsigned long long)free_p99);
}

int main(void)
{
	int iters = env_int("V8M_BENCH_ITERS", 100);
	int max_size_mb = env_int("V8M_BENCH_MAX_SIZE_MB", 256);
	if (max_size_mb < 1) {
		max_size_mb = 1;
	}
	/* Bound `iters` to a sane range so a tainted env-var value
	 * cannot drive the latency-buffer malloc beyond what's
	 * meaningful for percentile reporting (10 000 samples is more
	 * than enough; below 10 the percentiles are noise). */
	if (iters < 10) {
		iters = 10;
	}
	if (iters > 10000) {
		iters = 10000;
	}
	size_t max_bytes = (size_t)max_size_mb * 1024U * 1024U;

	/* `iters` is bounded to [10, 10000] above; the analyzer can't
	 * see the bound flow through both branches, so suppress the
	 * tainted-alloc warning explicitly here. */
	/* NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedAlloc) */
	uint64_t *alloc_ns = malloc((size_t)iters * sizeof(*alloc_ns));
	/* NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedAlloc) */
	uint64_t *fault_ns = malloc((size_t)iters * sizeof(*fault_ns));
	/* NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedAlloc) */
	uint64_t *free_ns = malloc((size_t)iters * sizeof(*free_ns));
	if (alloc_ns == NULL || fault_ns == NULL || free_ns == NULL) {
		(void)fprintf(stderr,
			      "mb_06: failed to allocate latency buffers\n");
		free(alloc_ns);
		free(fault_ns);
		free(free_ns);
		return 1;
	}

	(void)printf("# MB-06 large allocation latency\n");
	(void)printf("# iters=%d max_size_mb=%d\n", iters, max_size_mb);
	(void)printf("%-12s %-6s %-13s %-13s %-13s %-13s %-12s %-12s\n",
		     "size_bytes", "iters", "alloc_us_p50", "alloc_us_p99",
		     "fault_us_p50", "fault_us_p99", "free_us_p50",
		     "free_us_p99");

	for (size_t i = 0; i < g_size_count; i++) {
		size_t size = g_sizes[i];
		if (size > max_bytes) {
			break;
		}
		measure_one(size, iters, alloc_ns, fault_ns, free_ns);
		report(size, iters, alloc_ns, fault_ns, free_ns);
	}

	free(alloc_ns);
	free(fault_ns);
	free(free_ns);
	return 0;
}
