/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-03 producer/consumer benchmark (benchmarks.md §2.3). N
 * producer threads each malloc fixed-size objects and hand them to
 * a paired consumer through a bounded ring buffer; the consumer
 * frees. Every alloc / free pair crosses a thread boundary, so the
 * bench measures the cost of cross-thread free — the path the
 * future remote-free MPSC queue is meant to optimize.
 *
 * History: the first cut of this bench (written before the slab
 * pool's partials-list duplicate-insertion bug was diagnosed)
 * reliably segfaulted inside `try_partials` under sustained
 * cross-thread churn and was reverted. After the fix landed in
 * `src/v8m_slab_pool.c` the bench completes cleanly and now serves
 * as the regression gate for that class of bug: any reintroduction
 * of a stale partials-list entry will crash this bench within
 * a few seconds of the timed loop.
 *
 * v0 still has no thread cache, so every alloc / free contends on
 * the slab pool's single mutex. The bench produces a meaningful
 * baseline number: as TLC lands and the MPSC drain wires in, this
 * number should climb dramatically. Treat today's output as the
 * regression floor, not the target.
 *
 * One SPSC ring per (producer, consumer) pair, capacity 1024. A
 * pair sweep (1, 2, 4, 8, 16) capped at `min(nproc, 32)` per side
 * so a small CI box does not thrash; `V8M_BENCH_MAX_PAIRS=N` opts
 * past the cap. Size sweep matches the spec: 64 B, 256 B, 1 KiB.
 *
 * Output is one line per (pairs, size) cell, parseable into a
 * CSV-style ingest:
 *
 *   pairs  size_bytes  handoffs        handoffs_per_sec  per_pair
 *   1      64          1_600_000       1_600_000         1_600_000
 *   4      64          5_100_000       5_100_000         1_275_000
 *
 * Knobs (env vars):
 *   V8M_BENCH_DURATION_MS   per-cell budget, default 1000
 *   V8M_BENCH_WARMUP_MS     per-cell warmup,  default 100
 *   V8M_BENCH_MAX_PAIRS     cap on pairs per side
 *                           (default min(nproc/2, 32))
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — provides clock_gettime + CLOCK_MONOTONIC */
#include <unistd.h>

enum { RING_CAPACITY = 1024 };

static const int g_pair_sweep[] = {1, 2, 4, 8, 16};
static const size_t g_pair_sweep_count =
    sizeof(g_pair_sweep) / sizeof(g_pair_sweep[0]);

static const size_t g_size_sweep[] = {64, 256, 1024};
static const size_t g_size_sweep_count =
    sizeof(g_size_sweep) / sizeof(g_size_sweep[0]);

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

/* Single-producer / single-consumer ring. With one producer and
 * one consumer per ring the classic SPSC orderings apply: producer
 * uses relaxed-load on head + acquire-load on tail + release-store
 * on head; consumer is symmetric. No CAS on the hot path. */
struct ring {
	void *slots[RING_CAPACITY];
	_Atomic uint64_t head; /* producer writes */
	_Atomic uint64_t tail; /* consumer reads */
};

static void ring_init(struct ring *ring)
{
	atomic_store_explicit(&ring->head, 0, memory_order_relaxed);
	atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
}

static bool ring_push(struct ring *ring, void *ptr, uint64_t deadline_ns)
{
	for (;;) {
		uint64_t head =
		    atomic_load_explicit(&ring->head, memory_order_relaxed);
		uint64_t tail =
		    atomic_load_explicit(&ring->tail, memory_order_acquire);
		if (head - tail < RING_CAPACITY) {
			ring->slots[head % RING_CAPACITY] = ptr;
			atomic_store_explicit(&ring->head, head + 1U,
					      memory_order_release);
			return true;
		}
		if (now_ns() >= deadline_ns) {
			return false;
		}
	}
}

static void *ring_pop(struct ring *ring, uint64_t deadline_ns)
{
	for (;;) {
		uint64_t tail =
		    atomic_load_explicit(&ring->tail, memory_order_relaxed);
		uint64_t head =
		    atomic_load_explicit(&ring->head, memory_order_acquire);
		if (head - tail > 0U) {
			void *ptr = ring->slots[tail % RING_CAPACITY];
			atomic_store_explicit(&ring->tail, tail + 1U,
					      memory_order_release);
			return ptr;
		}
		if (now_ns() >= deadline_ns) {
			return NULL;
		}
	}
}

struct pair_args {
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_barrier_t *start;
	pthread_barrier_t *stop;
	struct ring *ring;
	uint64_t deadline_ns;
	uint64_t warmup_end_ns;
	size_t size;
	uint64_t handoffs; /* output (set by consumer) */
};

static void *producer(void *arg)
{
	struct pair_args *args = (struct pair_args *)arg;

	/* Warmup the producer's allocator-side state before timing. */
	while (now_ns() < args->warmup_end_ns) {
		void *ptr = malloc(args->size);
		if (ptr == NULL) {
			break;
		}
		((volatile unsigned char *)ptr)[0] = 1U;
		free(ptr);
	}

	(void)pthread_barrier_wait(args->start);

	while (now_ns() < args->deadline_ns) {
		void *ptr = malloc(args->size);
		if (ptr == NULL) {
			break;
		}
		((volatile unsigned char *)ptr)[0] = 0xAB;
		if (!ring_push(args->ring, ptr, args->deadline_ns)) {
			/* Deadline hit while spinning on a full ring; free
			 * locally so the consumer does not see a half-
			 * pushed slot. */
			free(ptr);
			break;
		}
	}

	(void)pthread_barrier_wait(args->stop);
	return NULL;
}

static void *consumer(void *arg)
{
	struct pair_args *args = (struct pair_args *)arg;

	/* Mirror the producer's warmup so the consumer's slab
	 * bookkeeping is warm before timing. */
	while (now_ns() < args->warmup_end_ns) {
		void *ptr = malloc(args->size);
		if (ptr == NULL) {
			break;
		}
		free(ptr);
	}

	(void)pthread_barrier_wait(args->start);

	uint64_t handoffs = 0;
	while (now_ns() < args->deadline_ns) {
		void *ptr = ring_pop(args->ring, args->deadline_ns);
		if (ptr == NULL) {
			break;
		}
		free(ptr);
		handoffs++;
	}
	/* Drain residue from the ring so we do not leak. The drain
	 * deadline is 1 ms past now — enough time for the producer's
	 * final pushes to land. */
	uint64_t drain_end = now_ns() + 1000000ULL;
	for (;;) {
		void *ptr = ring_pop(args->ring, drain_end);
		if (ptr == NULL) {
			break;
		}
		free(ptr);
		handoffs++;
	}
	args->handoffs = handoffs;

	(void)pthread_barrier_wait(args->stop);
	return NULL;
}

/* The four quantities passed are unambiguous by name (pair count,
 * per-op size, two timing budgets); swappable-parameters NOLINT
 * because struct wrapping is overkill for one call site. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void measure_one(int pairs, size_t size, int duration_ms, int warmup_ms)
{
	int total_threads = pairs * 2;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t *tids = calloc((size_t)total_threads, sizeof(*tids));
	struct pair_args *args = calloc((size_t)pairs, sizeof(*args));
	struct ring *rings = calloc((size_t)pairs, sizeof(*rings));
	if (tids == NULL || args == NULL || rings == NULL) {
		(void)fprintf(stderr,
			      "mb_03: failed to allocate worker arrays\n");
		free(tids);
		free(args);
		free(rings);
		return;
	}
	for (int i = 0; i < pairs; i++) {
		ring_init(&rings[i]);
	}

	pthread_barrier_t start;
	pthread_barrier_t stop;
	(void)pthread_barrier_init(&start, NULL, (unsigned)total_threads + 1U);
	(void)pthread_barrier_init(&stop, NULL, (unsigned)total_threads + 1U);

	uint64_t warmup_end_ns = now_ns() + ((uint64_t)warmup_ms * 1000000ULL);
	uint64_t deadline_ns =
	    warmup_end_ns + ((uint64_t)duration_ms * 1000000ULL);

	for (int i = 0; i < pairs; i++) {
		args[i].start = &start;
		args[i].stop = &stop;
		args[i].ring = &rings[i];
		args[i].warmup_end_ns = warmup_end_ns;
		args[i].deadline_ns = deadline_ns;
		args[i].size = size;
	}

	int created = 0;
	for (int i = 0; i < pairs; i++) {
		if (pthread_create(&tids[created++], NULL, producer,
				   &args[i]) != 0) {
			(void)fprintf(
			    stderr, "mb_03: pthread_create producer failed\n");
			goto cleanup;
		}
		if (pthread_create(&tids[created++], NULL, consumer,
				   &args[i]) != 0) {
			(void)fprintf(
			    stderr, "mb_03: pthread_create consumer failed\n");
			goto cleanup;
		}
	}

	(void)pthread_barrier_wait(&start);
	uint64_t timed_start_ns = now_ns();
	(void)pthread_barrier_wait(&stop);
	uint64_t elapsed_ns = now_ns() - timed_start_ns;

	for (int i = 0; i < created; i++) {
		(void)pthread_join(tids[i], NULL);
	}

	uint64_t total_handoffs = 0;
	for (int i = 0; i < pairs; i++) {
		total_handoffs += args[i].handoffs;
	}

	double handoffs_per_sec =
	    (elapsed_ns == 0U)
		? 0.0
		: ((double)total_handoffs * 1000000000.0 / (double)elapsed_ns);
	double per_pair = handoffs_per_sec / (double)pairs;

	(void)printf("%-6d %-11zu %-15llu %-17.0f %-15.0f\n", pairs, size,
		     (unsigned long long)total_handoffs, handoffs_per_sec,
		     per_pair);

cleanup:
	(void)pthread_barrier_destroy(&start);
	(void)pthread_barrier_destroy(&stop);
	free(tids);
	free(args);
	free(rings);
}

static int auto_max_pairs(void)
{
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online <= 0) {
		return 4;
	}
	long pairs = online / 2L;
	if (pairs < 1) {
		pairs = 1;
	}
	if (pairs > 32) {
		pairs = 32;
	}
	return (int)pairs;
}

int main(void)
{
	int duration_ms = env_int("V8M_BENCH_DURATION_MS", 1000);
	int warmup_ms = env_int("V8M_BENCH_WARMUP_MS", 100);
	int max_pairs = env_int("V8M_BENCH_MAX_PAIRS", auto_max_pairs());
	if (max_pairs < 1) {
		max_pairs = 1;
	}

	(void)printf("# MB-03 producer/consumer (cross-thread free)\n");
	(void)printf("# duration_ms=%d warmup_ms=%d max_pairs=%d ring=%d\n",
		     duration_ms, warmup_ms, max_pairs, RING_CAPACITY);
	(void)printf("%-6s %-11s %-15s %-17s %-15s\n", "pairs", "size_bytes",
		     "handoffs", "handoffs_per_sec", "per_pair");

	for (size_t pi = 0; pi < g_pair_sweep_count; pi++) {
		int pairs = g_pair_sweep[pi];
		if (pairs > max_pairs) {
			break;
		}
		for (size_t si = 0; si < g_size_sweep_count; si++) {
			measure_one(pairs, g_size_sweep[si], duration_ms,
				    warmup_ms);
		}
	}
	return 0;
}
