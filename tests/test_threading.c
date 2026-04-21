/* SPDX-License-Identifier: Apache-2.0 */
/*
 * End-to-end concurrency test through the public malloc/realloc/free
 * API. Complements the per-pool stress tests (test_slab_pool,
 * test_buddy_pool, test_dispatch) by going through the constructor-
 * installed dispatcher and the same path LD_PRELOAD users hit.
 *
 * Each worker thread:
 *
 *   1. Runs OPS_PER_WORKER allocate / write / verify / free cycles.
 *   2. Picks the size for each op from a rotating set that covers
 *      every backend (slab Tiny, slab Small, buddy Medium, large
 *      mmap, huge mmap), so the dispatcher's three-way routing is
 *      exercised by every worker.
 *   3. Stamps a per-thread byte pattern across the allocation and
 *      reads it back before freeing. Inter-thread corruption — say,
 *      two threads receiving the same pointer concurrently — would
 *      surface as a pattern mismatch.
 *   4. Mid-loop, every 8th op detours through realloc instead of
 *      free. The realloc grows the allocation by 1.5x and verifies
 *      the original bytes are preserved across the move, then frees
 *      the new pointer.
 *
 * Failure surfaces: NULL malloc, NULL realloc, pattern mismatch,
 * worker hang. The test asserts there are none of those across all
 * worker threads.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	WORKER_THREADS = 8,
	OPS_PER_WORKER = 512,
	REALLOC_INTERVAL = 8,
};

/* Sizes spanning every backend the dispatcher routes to. The
 * exhaustive coverage matters more than the specific numbers. */
static const size_t g_sizes[] = {
    16,
    96,
    256,
    1024,
    4096,
    16384,
    65536,
    (size_t)200 * 1024,
    (size_t)512 * 1024,
    (size_t)768 * 1024,
    ((size_t)2 * 1024 * 1024) + 4096U,
};
static const size_t g_size_count = sizeof(g_sizes) / sizeof(g_sizes[0]);

struct worker_arg {
	int thread_id;
	int failures;
};

static unsigned char pattern_byte(int thread_id, size_t offset)
{
	/* Mix in offset so a memmove that drops a byte still detects
	 * the corruption. */
	unsigned mixed =
	    ((unsigned)thread_id * 0x9EU) ^ ((unsigned)offset & 0xFFU);
	return (unsigned char)mixed;
}

/* stamp/verify own all three int-shaped params; swapping them would
 * surface in the very first verify(). The naming pins the role of
 * each, so the linter's "easily swappable" flag adds no signal. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void stamp(unsigned char *buf, size_t bytes, int thread_id)
{
	for (size_t i = 0; i < bytes; i++) {
		buf[i] = pattern_byte(thread_id, i);
	}
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static int verify(const unsigned char *buf, size_t bytes, int thread_id)
{
	for (size_t i = 0; i < bytes; i++) {
		if (buf[i] != pattern_byte(thread_id, i)) {
			return 1;
		}
	}
	return 0;
}

static void *worker(void *raw)
{
	struct worker_arg *arg = raw;
	for (int op = 0; op < OPS_PER_WORKER; op++) {
		size_t size = g_sizes[(size_t)op % g_size_count];
		unsigned char *ptr = malloc(size);
		if (ptr == NULL) {
			arg->failures++;
			return NULL;
		}
		stamp(ptr, size, arg->thread_id);
		if (verify(ptr, size, arg->thread_id) != 0) {
			arg->failures++;
			free(ptr);
			return NULL;
		}

		if ((op % REALLOC_INTERVAL) == 0) {
			size_t new_size = size + (size / 2U) + 1U;
			unsigned char *grown = realloc(ptr, new_size);
			if (grown == NULL) {
				arg->failures++;
				free(ptr);
				return NULL;
			}
			/* Original bytes must survive the realloc. The new
			 * tail [size, new_size) is uninitialized; don't
			 * verify it. */
			if (verify(grown, size, arg->thread_id) != 0) {
				arg->failures++;
				free(grown);
				return NULL;
			}
			free(grown);
		} else {
			free(ptr);
		}
	}
	return NULL;
}

int main(void)
{
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKER_THREADS];
	struct worker_arg args[WORKER_THREADS];

	for (int i = 0; i < WORKER_THREADS; i++) {
		args[i].thread_id = i + 1; /* avoid 0 so pattern_byte != 0 */
		args[i].failures = 0;
		if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
			(void)fprintf(
			    stderr, "test_threading: pthread_create failed\n");
			for (int j = 0; j < i; j++) {
				(void)pthread_join(threads[j], NULL);
			}
			return 1;
		}
	}

	int total_failures = 0;
	for (int i = 0; i < WORKER_THREADS; i++) {
		(void)pthread_join(threads[i], NULL);
		total_failures += args[i].failures;
	}

	if (total_failures != 0) {
		(void)fprintf(stderr,
			      "test_threading: %d failures across %d workers\n",
			      total_failures, WORKER_THREADS);
		return 1;
	}
	return 0;
}
