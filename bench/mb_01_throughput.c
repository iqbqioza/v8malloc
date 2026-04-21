/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-01 single-thread throughput benchmark (benchmarks.md §3.1).
 * A tight allocate / write / free loop measured against a wall-
 * clock budget. Reports ops/sec for each size class so a user can
 * sweep across an LD_PRELOAD chain (glibc, jemalloc, mimalloc,
 * tcmalloc, v8malloc) and compare like-for-like.
 *
 * The default configuration sweeps eight sizes spanning every
 * backend the dispatcher routes to:
 *
 *   8 B / 64 B / 512 B / 4 KiB    — slab Tiny + Small
 *   16 KiB / 64 KiB / 256 KiB     — buddy Medium
 *   2 MiB                         — Huge mmap
 *
 * Each size runs for ~250 ms by default; the per-size loop counts
 * the alloc/free pairs that fit and divides by elapsed time. The
 * write step touches the first byte so the kernel actually backs
 * the page — without that, MAP_ANONYMOUS regions stay zero-cost
 * until they're written and the allocator's mmap call dominates
 * the per-op cost in a misleading way.
 *
 * Output is one line per size, parseable into a CSV-style ingest:
 *
 *   size_bytes  iters     elapsed_us  ns_per_op   ops_per_sec
 *   8           5_000_000 250_010     50          20_000_000
 *
 * Runtime knobs come from environment variables so the LD_PRELOAD
 * comparison runs need no recompilation:
 *
 *   V8M_BENCH_DURATION_MS   per-size budget, default 250
 *   V8M_BENCH_WARMUP_MS     warmup before timing, default 50
 *
 * Multi-thread scalability is MB-02; the producer/consumer remote-
 * free pattern is MB-03; etc. Each lands as its own bench file.
 */

/* _GNU_SOURCE is already defined via target_compile_definitions in
 * bench/CMakeLists.txt; redefining it here would only trip the
 * compiler's "macro redefined" warning. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — provides clock_gettime + CLOCK_MONOTONIC */

static const size_t g_sizes[] = {
    8, 64, 512, 4096, 16384, 65536, (size_t)256 * 1024, (size_t)2 * 1024 * 1024,
};
static const size_t g_size_count = sizeof(g_sizes) / sizeof(g_sizes[0]);

static uint64_t now_ns(void)
{
	struct timespec time;
	/* CLOCK_MONOTONIC comes from <time.h> under _POSIX_C_SOURCE or
	 * _GNU_SOURCE; the latter is set globally by the bench
	 * CMakeLists. The IWYU rule loses that thread of definition. */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return ((uint64_t)time.tv_sec * 1000000000ULL) + (uint64_t)time.tv_nsec;
}

static int env_int(const char *name, int fallback)
{
	/* getenv is single-threaded by spec; the bench is too. The
	 * concurrency-mt-unsafe lint adds no signal here. */
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

/* The three int/size_t params describe distinct quantities with
 * unambiguous names; no struct wrapping needed. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void measure_one(size_t size, int duration_ms, int warmup_ms)
{
	/* Warmup: same loop, untimed. Pulls the size class's slab/
	 * buddy/large pages into cache + warms the allocator's
	 * internal lists so the measured loop sees steady state. */
	uint64_t warmup_end = now_ns() + ((uint64_t)warmup_ms * 1000000ULL);
	while (now_ns() < warmup_end) {
		void *ptr = malloc(size);
		if (ptr == NULL) {
			(void)fprintf(stderr,
				      "mb_01: malloc(%zu) returned NULL during "
				      "warmup\n",
				      size);
			return;
		}
		((volatile unsigned char *)ptr)[0] = 1U;
		free(ptr);
	}

	/* Timed loop: count alloc/free pairs that fit in the budget. */
	uint64_t deadline = now_ns() + ((uint64_t)duration_ms * 1000000ULL);
	uint64_t start = now_ns();
	uint64_t iters = 0;
	while (now_ns() < deadline) {
		void *ptr = malloc(size);
		if (ptr == NULL) {
			(void)fprintf(stderr,
				      "mb_01: malloc(%zu) returned NULL during "
				      "timed loop\n",
				      size);
			return;
		}
		((volatile unsigned char *)ptr)[0] = (unsigned char)iters;
		free(ptr);
		iters++;
	}
	uint64_t elapsed_ns = now_ns() - start;
	uint64_t elapsed_us = elapsed_ns / 1000ULL;
	uint64_t ns_per_op = (iters == 0U) ? 0U : elapsed_ns / iters;
	uint64_t ops_per_sec =
	    (elapsed_ns == 0U) ? 0U : (iters * 1000000000ULL) / elapsed_ns;

	(void)printf("%-12zu %-12llu %-12llu %-12llu %-12llu\n", size,
		     (unsigned long long)iters, (unsigned long long)elapsed_us,
		     (unsigned long long)ns_per_op,
		     (unsigned long long)ops_per_sec);
}

int main(void)
{
	int duration_ms = env_int("V8M_BENCH_DURATION_MS", 250);
	int warmup_ms = env_int("V8M_BENCH_WARMUP_MS", 50);

	(void)printf("# MB-01 single-thread throughput\n");
	(void)printf("# duration_ms=%d warmup_ms=%d\n", duration_ms, warmup_ms);
	(void)printf("%-12s %-12s %-12s %-12s %-12s\n", "size_bytes", "iters",
		     "elapsed_us", "ns_per_op", "ops_per_sec");
	for (size_t i = 0; i < g_size_count; i++) {
		measure_one(g_sizes[i], duration_ms, warmup_ms);
	}
	return 0;
}
