/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Buddy pool tests. Cover bounds, single alloc/free round-trip,
 * multi-arena allocation when one arena exhausts, arena
 * reclamation when fully drained, the size-less free path
 * recovering the size via the bitmaps, and concurrent alloc/free
 * across multiple threads.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v8m_buddy.h"
#include "v8m_buddy_pool.h"
#include "v8m_page_heap.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_buddy_pool: %s\n", msg);
	return 1;
}

enum {
	BLOCK_SIZE = 8192, /* level 1 — 32 per arena */
	ARENA_BLOCK_CAPACITY = 32,
	WORKER_THREADS = 8,
	OPS_PER_WORKER = 64
};

static int check_init_destroy(void)
{
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}
	v8m_buddy_pool_destroy(&pool);
	return 0;
}

static int check_bounds(void)
{
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	if (v8m_buddy_pool_alloc(&pool, 0) != NULL) {
		v8m_buddy_pool_destroy(&pool);
		return fail("alloc(0) did not return NULL");
	}
	if (v8m_buddy_pool_alloc(&pool, V8M_BUDDY_MAX_BLOCK + 1U) != NULL) {
		v8m_buddy_pool_destroy(&pool);
		return fail("alloc above max block did not return NULL");
	}
	/* Tolerance: free(NULL) and free of foreign pointer. */
	if (v8m_buddy_pool_free(&pool, NULL)) {
		v8m_buddy_pool_destroy(&pool);
		return fail("free(NULL) returned true");
	}
	int local = 0;
	if (v8m_buddy_pool_free(&pool, &local)) {
		v8m_buddy_pool_destroy(&pool);
		return fail("free of stack pointer returned true");
	}

	v8m_buddy_pool_destroy(&pool);
	return 0;
}

static int check_single_round_trip(void)
{
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	struct v8m_page_heap_stats before_alloc = {0};
	v8m_page_heap_get_stats(&before_alloc);

	void *obj = v8m_buddy_pool_alloc(&pool, BLOCK_SIZE);
	if (obj == NULL) {
		v8m_buddy_pool_destroy(&pool);
		return fail("alloc returned NULL");
	}
	if ((uintptr_t)obj % BLOCK_SIZE != 0U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("alloc pointer not 8 KiB-aligned");
	}

	struct v8m_page_heap_stats before_free = {0};
	v8m_page_heap_get_stats(&before_free);

	bool freed = v8m_buddy_pool_free(&pool, obj);
	if (!freed) {
		v8m_buddy_pool_destroy(&pool);
		return fail("single free did not own the pointer");
	}

	/* Free now drains the arena (MADV_DONTNEED) and keeps the VMA
	 * for cheap revival; the actual munmap waits for the bg-purge
	 * sweep. Force the sweep with idle_ticks=0 so this test still
	 * verifies that a fully-drained arena is releasable. */
	struct v8m_page_heap_stats after_free = {0};
	v8m_page_heap_get_stats(&after_free);
	if (after_free.advise_calls <= before_free.advise_calls) {
		v8m_buddy_pool_destroy(&pool);
		return fail("single free did not MADV_DONTNEED the arena");
	}
	(void)v8m_buddy_pool_sweep_idle(&pool, 0U);
	struct v8m_page_heap_stats after_sweep = {0};
	v8m_page_heap_get_stats(&after_sweep);
	if (after_sweep.munmap_calls <= before_free.munmap_calls) {
		v8m_buddy_pool_destroy(&pool);
		return fail("forced sweep did not release the drained arena");
	}

	v8m_buddy_pool_destroy(&pool);
	return 0;
}

static int check_multi_arena(void)
{
	/* Allocate enough 8 KiB blocks to fill one arena and start a
	 * second. Watch page-heap stats to verify two distinct arenas
	 * were acquired. */
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	struct v8m_page_heap_stats before = {0};
	v8m_page_heap_get_stats(&before);

	uintptr_t addrs[ARENA_BLOCK_CAPACITY + 1];
	for (int i = 0; i < ARENA_BLOCK_CAPACITY + 1; i++) {
		void *obj = v8m_buddy_pool_alloc(&pool, BLOCK_SIZE);
		if (obj == NULL) {
			v8m_buddy_pool_destroy(&pool);
			return fail("alloc failed mid-sequence");
		}
		addrs[i] = (uintptr_t)obj;
	}

	struct v8m_page_heap_stats after = {0};
	v8m_page_heap_get_stats(&after);
	if (after.mmap_calls < before.mmap_calls + 2) {
		v8m_buddy_pool_destroy(&pool);
		return fail("mmap_calls did not advance by at least 2");
	}

	/* Pairwise distinctness. */
	for (int i = 0; i < ARENA_BLOCK_CAPACITY + 1; i++) {
		for (int j = i + 1; j < ARENA_BLOCK_CAPACITY + 1; j++) {
			if (addrs[i] == addrs[j]) {
				v8m_buddy_pool_destroy(&pool);
				return fail("two allocations returned the same "
					    "address");
			}
		}
	}

	for (int i = 0; i < ARENA_BLOCK_CAPACITY + 1; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)addrs[i];
		(void)v8m_buddy_pool_free(&pool, obj);
	}

	v8m_buddy_pool_destroy(&pool);
	return 0;
}

static int check_arena_reclamation(void)
{
	/* Allocate enough to fill 2 arenas, then free everything from
	 * one arena. The pool should munmap that arena. Verify via
	 * page-heap stats. */
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	uintptr_t addrs[ARENA_BLOCK_CAPACITY + 1];
	for (int i = 0; i < ARENA_BLOCK_CAPACITY + 1; i++) {
		void *obj = v8m_buddy_pool_alloc(&pool, BLOCK_SIZE);
		if (obj == NULL) {
			v8m_buddy_pool_destroy(&pool);
			return fail("alloc failed during fill");
		}
		addrs[i] = (uintptr_t)obj;
	}

	struct v8m_page_heap_stats before = {0};
	v8m_page_heap_get_stats(&before);

	/* Free the last allocation — that's the only one in the second
	 * arena, so the second arena should drain and be reclaimed.
	 * The reclamation itself is observed below via page-heap
	 * stats; the boolean return only confirms that the pool
	 * recognized the pointer as one of its own. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	void *last = (void *)addrs[ARENA_BLOCK_CAPACITY];
	bool freed = v8m_buddy_pool_free(&pool, last);
	if (!freed) {
		v8m_buddy_pool_destroy(&pool);
		return fail(
		    "the last-allocation free was not recognized as owned");
	}
	/* The free drains the arena; force the sweep so munmap fires
	 * synchronously under the test's observation window. */
	(void)v8m_buddy_pool_sweep_idle(&pool, 0U);

	struct v8m_page_heap_stats after = {0};
	v8m_page_heap_get_stats(&after);
	if (after.munmap_calls <= before.munmap_calls) {
		v8m_buddy_pool_destroy(&pool);
		return fail(
		    "page-heap munmap_calls did not advance on arena reclaim");
	}

	for (int i = 0; i < ARENA_BLOCK_CAPACITY; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)addrs[i];
		(void)v8m_buddy_pool_free(&pool, obj);
	}

	v8m_buddy_pool_destroy(&pool);
	return 0;
}

static struct v8m_buddy_pool g_pool;
static atomic_int g_worker_failed = 0;

static void *worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < OPS_PER_WORKER; i++) {
		void *obj = v8m_buddy_pool_alloc(&g_pool, BLOCK_SIZE);
		if (obj == NULL) {
			atomic_store(&g_worker_failed, 1);
			return NULL;
		}
		if ((uintptr_t)obj % BLOCK_SIZE != 0U) {
			atomic_store(&g_worker_failed, 2);
			return NULL;
		}
		(void)v8m_buddy_pool_free(&g_pool, obj);
	}
	return NULL;
}

static int check_concurrent(void)
{
	if (v8m_buddy_pool_init(&g_pool) != 0) {
		return fail("init returned non-zero");
	}
	atomic_store(&g_worker_failed, 0);

	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKER_THREADS];
	for (int i = 0; i < WORKER_THREADS; i++) {
		if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
			v8m_buddy_pool_destroy(&g_pool);
			return fail("pthread_create failed");
		}
	}
	for (int i = 0; i < WORKER_THREADS; i++) {
		(void)pthread_join(threads[i], NULL);
	}

	int worker_status = atomic_load(&g_worker_failed);
	if (worker_status == 1) {
		v8m_buddy_pool_destroy(&g_pool);
		return fail("a concurrent alloc returned NULL");
	}
	if (worker_status == 2) {
		v8m_buddy_pool_destroy(&g_pool);
		return fail("a concurrent alloc was misaligned");
	}

	v8m_buddy_pool_destroy(&g_pool);
	return 0;
}

/*
 * Drain-and-revive cycle. After an arena fully drains, the next
 * alloc should land on the same arena (revival) without triggering
 * a fresh page-heap mmap. The sweep walks one tick at a time; an
 * arena revived between sweeps must reset its idle counter so the
 * sweep-with-default-threshold does not race the revival.
 */
static int check_drain_and_revive(void)
{
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	void *first = v8m_buddy_pool_alloc(&pool, BLOCK_SIZE);
	if (first == NULL) {
		v8m_buddy_pool_destroy(&pool);
		return fail("first alloc returned NULL");
	}

	struct v8m_page_heap_stats after_first_alloc = {0};
	v8m_page_heap_get_stats(&after_first_alloc);

	if (!v8m_buddy_pool_free(&pool, first)) {
		v8m_buddy_pool_destroy(&pool);
		return fail("free did not own the pointer");
	}

	struct v8m_buddy_pool_arena_stats stats = {0};
	v8m_buddy_pool_get_arena_stats(&pool, &stats);
	if (stats.drained != 1U || stats.live != 0U ||
	    stats.total_in_use != 1U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("arena did not enter drained state on free");
	}

	/* A second alloc must reuse the drained arena without a fresh
	 * page-heap mmap. */
	void *second = v8m_buddy_pool_alloc(&pool, BLOCK_SIZE);
	if (second == NULL) {
		v8m_buddy_pool_destroy(&pool);
		return fail("revive alloc returned NULL");
	}
	struct v8m_page_heap_stats after_revive = {0};
	v8m_page_heap_get_stats(&after_revive);
	if (after_revive.mmap_calls != after_first_alloc.mmap_calls) {
		v8m_buddy_pool_destroy(&pool);
		return fail("revive issued a fresh page-heap mmap");
	}
	v8m_buddy_pool_get_arena_stats(&pool, &stats);
	if (stats.drained != 0U || stats.live != 1U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("revive did not clear drained state");
	}

	(void)v8m_buddy_pool_free(&pool, second);
	v8m_buddy_pool_destroy(&pool);
	return 0;
}

/*
 * Sweep with a non-zero idle threshold drops drained arenas only
 * after they have aged enough ticks. A sweep called immediately
 * after the drain does NOT release the arena.
 */
static int check_sweep_threshold(void)
{
	struct v8m_buddy_pool pool;
	if (v8m_buddy_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	void *obj = v8m_buddy_pool_alloc(&pool, BLOCK_SIZE);
	if (obj == NULL) {
		v8m_buddy_pool_destroy(&pool);
		return fail("alloc returned NULL");
	}
	(void)v8m_buddy_pool_free(&pool, obj);

	/* First sweep with threshold=4: arena's idle_ticks goes 0 -> 1,
	 * still below threshold, so it stays drained. */
	size_t released = v8m_buddy_pool_sweep_idle(&pool, 4U);
	if (released != 0U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("sweep released a too-young arena");
	}
	struct v8m_buddy_pool_arena_stats stats = {0};
	v8m_buddy_pool_get_arena_stats(&pool, &stats);
	if (stats.drained != 1U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("arena lost drained state under-threshold");
	}

	/* Three more sweeps push idle_ticks to 4 → release. */
	for (int i = 0; i < 3; i++) {
		released = v8m_buddy_pool_sweep_idle(&pool, 4U);
	}
	if (released != 1U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("sweep at threshold did not release the arena");
	}
	v8m_buddy_pool_get_arena_stats(&pool, &stats);
	if (stats.total_in_use != 0U) {
		v8m_buddy_pool_destroy(&pool);
		return fail("released arena still tracked as in-use");
	}

	v8m_buddy_pool_destroy(&pool);
	return 0;
}

int main(void)
{
	int status = check_init_destroy();
	if (status != 0) {
		return status;
	}
	status = check_bounds();
	if (status != 0) {
		return status;
	}
	status = check_single_round_trip();
	if (status != 0) {
		return status;
	}
	status = check_multi_arena();
	if (status != 0) {
		return status;
	}
	status = check_arena_reclamation();
	if (status != 0) {
		return status;
	}
	status = check_drain_and_revive();
	if (status != 0) {
		return status;
	}
	status = check_sweep_threshold();
	if (status != 0) {
		return status;
	}
	return check_concurrent();
}
