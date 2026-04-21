/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-02 multi-thread scalability benchmark (benchmarks.md §2.2).
 * Sweeps thread counts running the same alloc / write / free loop
 * MB-01 uses, but at the fixed 64 B size the spec calls out as the
 * common case. Reports total throughput, per-thread throughput,
 * and the scalability ratio (N-thread / 1-thread).
 *
 * Thread sweep matches the spec table — 1, 2, 4, 8, 16, 32, 64,
 * 128 — but skips entries above `nproc` so a 4-core CI box does not
 * spend wall time thrashing 128 contended threads. The env knob
 * V8M_BENCH_MAX_THREADS overrides the auto-derived cap when a user
 * deliberately wants to oversubscribe.
 *
 * Each per-thread-count run barriers the workers in, runs them for
 * the shared duration, and barriers them out. The 1-thread number
 * lands first and anchors the scalability column for everything
 * after it. Output is one line per thread count, parseable into a
 * CSV-style ingest:
 *
 *   threads  total_ops  ops_per_sec   per_thread_ops  scal_ratio
 *   1        20_000_000 20_000_000    20_000_000      1.00
 *   2        38_000_000 38_000_000    19_000_000      1.90
 *
 * Runtime knobs (env vars) so LD_PRELOAD comparison runs need no
 * recompile:
 *
 *   V8M_BENCH_DURATION_MS   per-config budget, default 1000
 *   V8M_BENCH_WARMUP_MS     per-config warmup, default 100
 *   V8M_BENCH_SIZE          per-op allocation size, default 64
 *   V8M_BENCH_MAX_THREADS   cap on the thread sweep, default
 *                           min(nproc, 32) — bumps to 128 only when
 *                           explicitly set
 *
 * The spec asks for a 10-second per-config budget; the 1-second
 * default keeps `make bench`-style smoke runs interactive while
 * still being long enough to swamp warmup noise. Long campaigns
 * pass V8M_BENCH_DURATION_MS=10000 to honour the spec exactly.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — provides clock_gettime + CLOCK_MONOTONIC */
#include <unistd.h>

static const int g_thread_sweep[] = {1, 2, 4, 8, 16, 32, 64, 128};
static const size_t g_thread_sweep_count =
    sizeof(g_thread_sweep) / sizeof(g_thread_sweep[0]);

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

struct worker_args {
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_barrier_t *start;
	pthread_barrier_t *stop;
	uint64_t deadline_ns;
	uint64_t warmup_end_ns;
	size_t size;
	uint64_t iters; /* output */
};

static void *worker(void *arg)
{
	struct worker_args *args = (struct worker_args *)arg;

	/* Warmup: pulls the size class into cache + warms the
	 * allocator's per-thread state so the timed loop sees steady
	 * state. Untimed. */
	while (now_ns() < args->warmup_end_ns) {
		void *ptr = malloc(args->size);
		if (ptr == NULL) {
			break;
		}
		((volatile unsigned char *)ptr)[0] = 1U;
		free(ptr);
	}

	(void)pthread_barrier_wait(args->start);

	uint64_t iters = 0;
	while (now_ns() < args->deadline_ns) {
		void *ptr = malloc(args->size);
		if (ptr == NULL) {
			(void)fprintf(stderr,
				      "mb_02: malloc(%zu) returned NULL\n",
				      args->size);
			break;
		}
		((volatile unsigned char *)ptr)[0] = (unsigned char)iters;
		free(ptr);
		iters++;
	}
	args->iters = iters;

	(void)pthread_barrier_wait(args->stop);
	return NULL;
}

/* Five-arg helper (size + two durations + thread count + the
 * baseline ops/sec) describes distinct quantities by name; struct
 * wrapping would be overkill for a single internal call site. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static double measure_one(int threads, size_t size, int duration_ms,
			  int warmup_ms, double baseline_ops_per_sec)
{
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t *tids = calloc((size_t)threads, sizeof(*tids));
	struct worker_args *args = calloc((size_t)threads, sizeof(*args));
	if (tids == NULL || args == NULL) {
		(void)fprintf(stderr,
			      "mb_02: failed to allocate worker arrays\n");
		free(tids);
		free(args);
		return 0.0;
	}

	pthread_barrier_t start;
	pthread_barrier_t stop;
	(void)pthread_barrier_init(&start, NULL, (unsigned)threads + 1U);
	(void)pthread_barrier_init(&stop, NULL, (unsigned)threads + 1U);

	uint64_t warmup_end_ns = now_ns() + ((uint64_t)warmup_ms * 1000000ULL);
	uint64_t deadline_ns =
	    warmup_end_ns + ((uint64_t)duration_ms * 1000000ULL);

	for (int i = 0; i < threads; i++) {
		args[i].start = &start;
		args[i].stop = &stop;
		args[i].warmup_end_ns = warmup_end_ns;
		args[i].deadline_ns = deadline_ns;
		args[i].size = size;
		int err = pthread_create(&tids[i], NULL, worker, &args[i]);
		if (err != 0) {
			/* strerror is single-call thread-unsafe; the bench
			 * is single-threaded at this point so the lint adds
			 * no signal. */
			/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
			const char *msg = strerror(err);
			(void)fprintf(
			    stderr, "mb_02: pthread_create failed: %s\n", msg);
			free(tids);
			free(args);
			(void)pthread_barrier_destroy(&start);
			(void)pthread_barrier_destroy(&stop);
			return 0.0;
		}
	}

	(void)pthread_barrier_wait(&start);
	uint64_t timed_start_ns = now_ns();
	(void)pthread_barrier_wait(&stop);
	uint64_t elapsed_ns = now_ns() - timed_start_ns;

	for (int i = 0; i < threads; i++) {
		(void)pthread_join(tids[i], NULL);
	}

	uint64_t total_iters = 0;
	for (int i = 0; i < threads; i++) {
		total_iters += args[i].iters;
	}

	double ops_per_sec =
	    (elapsed_ns == 0U)
		? 0.0
		: ((double)total_iters * 1000000000.0 / (double)elapsed_ns);
	double per_thread_ops_per_sec = ops_per_sec / (double)threads;
	double scal_ratio = (baseline_ops_per_sec == 0.0)
				? 1.0
				: ops_per_sec / baseline_ops_per_sec;

	(void)printf("%-8d %-14llu %-14.0f %-16.0f %-10.2f\n", threads,
		     (unsigned long long)total_iters, ops_per_sec,
		     per_thread_ops_per_sec, scal_ratio);

	(void)pthread_barrier_destroy(&start);
	(void)pthread_barrier_destroy(&stop);
	free(tids);
	free(args);

	return ops_per_sec;
}

static int auto_max_threads(void)
{
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online <= 0) {
		return 8;
	}
	if (online > 32) {
		return 32; /* the explicit env knob is the only way past this */
	}
	return (int)online;
}

int main(void)
{
	int duration_ms = env_int("V8M_BENCH_DURATION_MS", 1000);
	int warmup_ms = env_int("V8M_BENCH_WARMUP_MS", 100);
	int size_arg = env_int("V8M_BENCH_SIZE", 64);
	int max_threads = env_int("V8M_BENCH_MAX_THREADS", auto_max_threads());
	if (max_threads < 1) {
		max_threads = 1;
	}
	size_t size = (size_t)size_arg;

	(void)printf("# MB-02 multi-thread scalability\n");
	(void)printf("# duration_ms=%d warmup_ms=%d size=%zu max_threads=%d\n",
		     duration_ms, warmup_ms, size, max_threads);
	(void)printf("%-8s %-14s %-14s %-16s %-10s\n", "threads", "total_ops",
		     "ops_per_sec", "per_thread_ops", "scal");

	double baseline_ops_per_sec = 0.0;
	for (size_t i = 0; i < g_thread_sweep_count; i++) {
		int threads = g_thread_sweep[i];
		if (threads > max_threads) {
			break;
		}
		double ops_per_sec =
		    measure_one(threads, size, duration_ms, warmup_ms,
				baseline_ops_per_sec);
		if (i == 0) {
			baseline_ops_per_sec = ops_per_sec;
		}
	}
	return 0;
}
