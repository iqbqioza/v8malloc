/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Coverage for the thread-cache scaffolding (architecture.md §2.1
 * + thread-cache.md §2.1). v0 only ships the TLS slot, the lazy
 * initializer, and the pthread_key destructor; the cache itself
 * is not yet wired into malloc/free, so this test asserts the
 * plumbing rather than any allocator behaviour:
 *
 *   1. Same thread sees the same cache pointer across two calls.
 *   2. Two different threads see distinct cache pointers.
 *   3. v8m_thread_cache_peek returns NULL on a fresh thread that
 *      has not yet called get_or_create.
 *   4. The pthread_key destructor fires on thread exit — verified
 *      via the reclamation counter, which advances by exactly the
 *      number of threads we joined.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "v8m_thread_cache.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_thread_cache: %s\n", msg);
	return 1;
}

static int check_same_thread_returns_same_cache(void)
{
	const struct v8m_thread_cache *first = v8m_thread_cache_get_or_create();
	if (first == NULL) {
		return fail("get_or_create returned NULL on first call");
	}
	const struct v8m_thread_cache *second =
	    v8m_thread_cache_get_or_create();
	if (second != first) {
		return fail("get_or_create returned a different cache on the "
			    "second call from the same thread");
	}
	if (v8m_thread_cache_peek() != first) {
		return fail(
		    "peek diverged from get_or_create on the same thread");
	}
	return 0;
}

/* NOLINTBEGIN(misc-include-cleaner) — pthread.h IS included above;
 * the IWYU-style cleaner does not always recognize pthread_t /
 * pthread_barrier_t as coming from it. */
struct worker_arg {
	struct v8m_thread_cache *observed;
	struct v8m_thread_cache *peeked_before_create;
	/* Optional barrier — when non-NULL the worker waits on it
	 * AFTER recording the observation but BEFORE returning. The
	 * parent uses this to keep all workers alive simultaneously
	 * while it inspects each cache pointer; without it the OS
	 * is free to schedule the workers serially, in which case
	 * each worker's destructor frees its cache slot before the
	 * next worker's malloc fires and the slot gets reused. */
	pthread_barrier_t *gate;
};
/* NOLINTEND(misc-include-cleaner) */

static void *worker_observe(void *raw)
{
	struct worker_arg *arg = raw;
	arg->peeked_before_create = v8m_thread_cache_peek();
	arg->observed = v8m_thread_cache_get_or_create();
	if (arg->gate != NULL) {
		(void)pthread_barrier_wait(arg->gate);
	}
	return NULL;
}

static int check_distinct_threads_get_distinct_caches(void)
{
	const struct v8m_thread_cache *parent_cache =
	    v8m_thread_cache_get_or_create();
	if (parent_cache == NULL) {
		return fail("parent cache get failed");
	}

	/* Three-party barrier: parent + 2 workers. Workers wait on
	 * the barrier after recording observations so both caches
	 * stay live simultaneously; the parent reads observations
	 * after waiting, then joins. */
	pthread_barrier_t gate;
	if (pthread_barrier_init(&gate, NULL, 3) != 0) {
		return fail("pthread_barrier_init failed");
	}
	struct worker_arg arg_a = {.gate = &gate};
	struct worker_arg arg_b = {.gate = &gate};
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t thread_a;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t thread_b;
	if (pthread_create(&thread_a, NULL, worker_observe, &arg_a) != 0) {
		(void)pthread_barrier_destroy(&gate);
		return fail("pthread_create A failed");
	}
	if (pthread_create(&thread_b, NULL, worker_observe, &arg_b) != 0) {
		(void)pthread_barrier_wait(&gate);
		(void)pthread_join(thread_a, NULL);
		(void)pthread_barrier_destroy(&gate);
		return fail("pthread_create B failed");
	}
	(void)pthread_barrier_wait(&gate);

	int result = 0;
	if (arg_a.peeked_before_create != NULL) {
		result =
		    fail("worker A's peek-before-create returned non-NULL");
	} else if (arg_b.peeked_before_create != NULL) {
		result =
		    fail("worker B's peek-before-create returned non-NULL");
	} else if (arg_a.observed == NULL || arg_b.observed == NULL) {
		result = fail("worker get_or_create returned NULL");
	} else if (arg_a.observed == arg_b.observed) {
		result = fail("two workers shared the same cache pointer");
	} else if (arg_a.observed == parent_cache ||
		   arg_b.observed == parent_cache) {
		result = fail("worker shared parent's cache pointer");
	}

	(void)pthread_join(thread_a, NULL);
	(void)pthread_join(thread_b, NULL);
	(void)pthread_barrier_destroy(&gate);
	return result;
}

static int check_destructor_fires_on_thread_exit(void)
{
	uint64_t before = v8m_thread_cache_destructor_calls();

	enum { WORKERS = 4 };
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKERS];
	struct worker_arg args[WORKERS] = {0};
	for (int i = 0; i < WORKERS; i++) {
		if (pthread_create(&threads[i], NULL, worker_observe,
				   &args[i]) != 0) {
			for (int j = 0; j < i; j++) {
				(void)pthread_join(threads[j], NULL);
			}
			return fail("pthread_create failed mid-batch");
		}
	}
	for (int i = 0; i < WORKERS; i++) {
		(void)pthread_join(threads[i], NULL);
	}

	uint64_t after = v8m_thread_cache_destructor_calls();
	if (after - before != (uint64_t)WORKERS) {
		(void)fprintf(stderr,
			      "test_thread_cache: destructor calls advanced "
			      "by %llu, expected %d\n",
			      (unsigned long long)(after - before), WORKERS);
		return 1;
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_same_thread_returns_same_cache();
	result |= check_distinct_threads_get_distinct_caches();
	result |= check_destructor_fires_on_thread_exit();
	if (result == 0) {
		(void)printf("test_thread_cache: OK\n");
	}
	return result;
}
