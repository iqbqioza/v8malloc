/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Thread-churn stress test (benchmarks.md §4 ST-04). Repeatedly
 * creates and destroys batches of threads, each doing a short
 * allocate / free run. The cumulative thread count crosses 10 000
 * over the test — the design target for validating TLC reclamation
 * (once the thread cache lands) and verifying that pthread_atfork,
 * the slab / buddy pools, and the region map all tolerate rapid
 * thread turnover without leaking mappings.
 *
 * The test batches thread creation so only BATCH_SIZE threads are
 * alive at once; pthread_create with thousands of simultaneously
 * live threads routinely overshoots nproc-driven scheduler limits
 * on CI runners. 100 batches × 100 threads = 10 000 creations, one
 * join between each batch.
 *
 * Post-run invariants:
 *   - Every worker thread reported success.
 *   - Live page-heap regions did not grow unboundedly. We capture
 *     region count before the churn and confirm it returns to
 *     roughly that value after — a pool bug that leaks one page
 *     per thread would surface as a 10 000-region increase.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8malloc/v8malloc.h"

enum {
	BATCH_SIZE = 100,
	TOTAL_BATCHES = 100,
	ALLOCS_PER_THREAD = 16,
	/* Soft upper bound on the post-run live-region growth.
	 * Tight because v0 doesn't yet cache per-thread pages —
	 * every free returns the empty page to the heap. Once the
	 * TLC lands with `bin_capacity` retention, we'll want this
	 * wider. */
	MAX_REGION_LEAK = 32,
};

/* Size set covering every backend; same spirit as test_threading
 * but narrower so each thread stays cheap and the test finishes
 * within a CI budget. */
static const size_t g_sizes[] = {
    16, 256, 4096, 65536, (size_t)256 * 1024,
};
static const size_t g_size_count = sizeof(g_sizes) / sizeof(g_sizes[0]);

static atomic_int g_worker_failures;

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_thread_churn: %s\n", msg);
	return 1;
}

static void *churn_worker(void *raw)
{
	uintptr_t tid = (uintptr_t)raw;
	for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
		size_t size = g_sizes[(tid + (uintptr_t)i) % g_size_count];
		unsigned char *ptr = malloc(size);
		if (ptr == NULL) {
			atomic_fetch_add_explicit(&g_worker_failures, 1,
						  memory_order_relaxed);
			return NULL;
		}
		(void)memset(ptr, (int)(tid & 0xFFU), size);
		/* Re-read the first byte to confirm the write committed
		 * to the allocation and we're not racing a concurrent
		 * free on the same address. */
		if (ptr[0] != (unsigned char)(tid & 0xFFU)) {
			free(ptr);
			atomic_fetch_add_explicit(&g_worker_failures, 1,
						  memory_order_relaxed);
			return NULL;
		}
		free(ptr);
	}
	return NULL;
}

int main(void)
{
	struct v8m_stats before = {0};
	v8m_get_stats(&before);

	for (int batch = 0; batch < TOTAL_BATCHES; batch++) {
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		pthread_t threads[BATCH_SIZE];
		for (int i = 0; i < BATCH_SIZE; i++) {
			uintptr_t tid =
			    ((uintptr_t)batch * BATCH_SIZE) + (uintptr_t)i + 1U;
			/* The void * passed to pthread_create carries an
			 * integer thread id; the int-to-ptr cast is the
			 * standard idiom for the pthreads "user data" slot. */
			/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
			void *arg = (void *)tid;
			if (pthread_create(&threads[i], NULL, churn_worker,
					   arg) != 0) {
				for (int j = 0; j < i; j++) {
					(void)pthread_join(threads[j], NULL);
				}
				return fail("pthread_create failed mid-batch");
			}
		}
		for (int i = 0; i < BATCH_SIZE; i++) {
			(void)pthread_join(threads[i], NULL);
		}
	}

	int failures =
	    atomic_load_explicit(&g_worker_failures, memory_order_relaxed);
	if (failures != 0) {
		return fail("one or more worker threads reported failure");
	}

	struct v8m_stats after = {0};
	v8m_get_stats(&after);
	uint64_t region_delta = (after.live_regions > before.live_regions)
				    ? after.live_regions - before.live_regions
				    : 0U;
	if (region_delta > MAX_REGION_LEAK) {
		(void)fprintf(
		    stderr,
		    "test_thread_churn: live_regions grew by %llu (before=%llu "
		    "after=%llu); suspected leak\n",
		    (unsigned long long)region_delta,
		    (unsigned long long)before.live_regions,
		    (unsigned long long)after.live_regions);
		return 1;
	}
	return 0;
}
