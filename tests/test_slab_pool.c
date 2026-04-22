/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Slab pool tests. Cover bounds checks, single alloc/free per
 * class, multi-page allocation when one page exhausts, page
 * recycling on full-empty, page-heap stats moving in step with
 * fresh-page acquisition and empty-page reclamation, and
 * concurrent alloc/free from multiple threads.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"
#include "v8m_slab_pool.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_slab_pool: %s\n", msg);
	return 1;
}

#define OWNER_THREAD UINT64_C(0xABCD1234)

enum {
	CLASS_TINY = 0, /* 8 B */
	CLASS_SMALL_MID = 19, /* 512 B */
	CLASS_SMALL_LARGE = 31, /* 4 KiB - 15 per page */
	WORKER_THREADS = 8,
	OPS_PER_WORKER = 256,
	CLASS_31_PAGE_CAPACITY = 15
};

static int check_init_destroy(void)
{
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}
	v8m_slab_pool_destroy(&pool);
	return 0;
}

static int check_bounds(void)
{
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	if (v8m_slab_pool_alloc(&pool, V8M_MEDIUM_FIRST_CLASS, OWNER_THREAD) !=
	    NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("alloc with Medium-class id did not return NULL");
	}
	if (v8m_slab_pool_alloc(&pool, V8M_NUM_SIZE_CLASSES + 100,
				OWNER_THREAD) != NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail(
		    "alloc with way-out-of-range class did not return NULL");
	}

	v8m_slab_pool_destroy(&pool);
	return 0;
}

static int check_single_alloc_free_per_class(uint32_t size_class)
{
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	void *obj = v8m_slab_pool_alloc(&pool, size_class, OWNER_THREAD);
	if (obj == NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("alloc returned NULL");
	}
	struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
	if (!v8m_page_meta_valid(meta)) {
		v8m_slab_pool_destroy(&pool);
		return fail("returned pointer's page failed magic check");
	}
	if (meta->size_class != size_class) {
		v8m_slab_pool_destroy(&pool);
		return fail("page's size_class does not match request");
	}

	bool became_empty = v8m_slab_pool_free(&pool, meta, obj);
	if (!became_empty) {
		v8m_slab_pool_destroy(&pool);
		return fail("single-object free did not signal empty");
	}

	v8m_slab_pool_destroy(&pool);
	return 0;
}

static int check_multi_page_allocation(void)
{
	/* Class 31 holds 15 objects per page; allocate 16 to force a
	 * second page. Track page-heap stats around the operation to
	 * verify a fresh page was actually acquired. */
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	struct v8m_page_heap_stats before = {0};
	v8m_page_heap_get_stats(&before);

	uintptr_t addrs[CLASS_31_PAGE_CAPACITY + 1];
	for (int i = 0; i < CLASS_31_PAGE_CAPACITY + 1; i++) {
		void *obj =
		    v8m_slab_pool_alloc(&pool, CLASS_SMALL_LARGE, OWNER_THREAD);
		if (obj == NULL) {
			v8m_slab_pool_destroy(&pool);
			return fail("alloc failed mid-sequence");
		}
		addrs[i] = (uintptr_t)obj;
	}

	struct v8m_page_heap_stats after = {0};
	v8m_page_heap_get_stats(&after);
	if (after.mmap_calls < before.mmap_calls + 2) {
		v8m_slab_pool_destroy(&pool);
		return fail("mmap_calls did not advance by at least 2");
	}

	/* All issued addresses are pairwise distinct. */
	for (int i = 0; i < CLASS_31_PAGE_CAPACITY + 1; i++) {
		for (int j = i + 1; j < CLASS_31_PAGE_CAPACITY + 1; j++) {
			if (addrs[i] == addrs[j]) {
				v8m_slab_pool_destroy(&pool);
				return fail("two allocations returned the same "
					    "pointer");
			}
		}
	}

	for (int i = 0; i < CLASS_31_PAGE_CAPACITY + 1; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)addrs[i];
		struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
		(void)v8m_slab_pool_free(&pool, meta, obj);
	}

	v8m_slab_pool_destroy(&pool);
	return 0;
}

static int check_full_to_partial_transition(void)
{
	/* Fill a class-31 page (15 allocs), drop it from current by
	 * triggering one more alloc (forces a new page), then free
	 * one slot from the now-full first page. The pool should add
	 * the recovered page back to partials. The simplest way to
	 * observe that is to keep allocating after the free and watch
	 * page-heap stats: if partials are reused, no fresh mmap is
	 * required. */
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	uintptr_t first_page_objs[CLASS_31_PAGE_CAPACITY];
	for (int i = 0; i < CLASS_31_PAGE_CAPACITY; i++) {
		void *obj =
		    v8m_slab_pool_alloc(&pool, CLASS_SMALL_LARGE, OWNER_THREAD);
		if (obj == NULL) {
			v8m_slab_pool_destroy(&pool);
			return fail("alloc failed during fill");
		}
		first_page_objs[i] = (uintptr_t)obj;
	}

	/* Force a new page so the first page is dropped from current. */
	const void *forces_new =
	    v8m_slab_pool_alloc(&pool, CLASS_SMALL_LARGE, OWNER_THREAD);
	if (forces_new == NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("alloc that should have spawned a new page failed");
	}

	/* Free one object from the first page — full -> partial. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	void *first_obj = (void *)first_page_objs[0];
	struct v8m_page_meta *first_meta = v8m_ptr_to_meta(first_obj);
	(void)v8m_slab_pool_free(&pool, first_meta, first_obj);

	/* Snapshot stats, then alloc once: this should reuse the
	 * partial page (mmap_calls unchanged). */
	struct v8m_page_heap_stats before = {0};
	v8m_page_heap_get_stats(&before);
	const void *reused =
	    v8m_slab_pool_alloc(&pool, CLASS_SMALL_LARGE, OWNER_THREAD);
	if (reused == NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("post-recovery alloc failed");
	}
	struct v8m_page_heap_stats after = {0};
	v8m_page_heap_get_stats(&after);
	if (after.mmap_calls != before.mmap_calls) {
		v8m_slab_pool_destroy(&pool);
		return fail(
		    "recovered page was not reused (mmap_calls advanced)");
	}

	v8m_slab_pool_destroy(&pool);
	return 0;
}

static struct v8m_slab_pool g_pool;
static atomic_int g_worker_failed = 0;

static void *worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < OPS_PER_WORKER; i++) {
		void *obj =
		    v8m_slab_pool_alloc(&g_pool, CLASS_TINY, OWNER_THREAD);
		if (obj == NULL) {
			atomic_store(&g_worker_failed, 1);
			return NULL;
		}
		struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
		if (!v8m_page_meta_valid(meta)) {
			atomic_store(&g_worker_failed, 2);
			return NULL;
		}
		(void)v8m_slab_pool_free(&g_pool, meta, obj);
	}
	return NULL;
}

static int check_concurrent(void)
{
	if (v8m_slab_pool_init(&g_pool) != 0) {
		return fail("init returned non-zero");
	}
	atomic_store(&g_worker_failed, 0);

	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKER_THREADS];
	for (int i = 0; i < WORKER_THREADS; i++) {
		if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
			v8m_slab_pool_destroy(&g_pool);
			return fail("pthread_create failed");
		}
	}
	for (int i = 0; i < WORKER_THREADS; i++) {
		(void)pthread_join(threads[i], NULL);
	}

	int worker_status = atomic_load(&g_worker_failed);
	if (worker_status == 1) {
		v8m_slab_pool_destroy(&g_pool);
		return fail("a concurrent alloc returned NULL");
	}
	if (worker_status == 2) {
		v8m_slab_pool_destroy(&g_pool);
		return fail("a concurrent alloc returned an invalid page");
	}

	v8m_slab_pool_destroy(&g_pool);
	return 0;
}

/*
 * Verify the partials pick honours utilization: when two partial
 * pages are eligible, the most-utilized one should be chosen so
 * less-utilized pages drain back to empty (and the page heap)
 * faster.
 *
 * Layout: class 31 holds 15 objects per page. The partials code
 * path only fires when `current` cannot satisfy the allocation,
 * so the test fills three distinct pages (A, B, C) before exercising
 * the pick:
 *
 *   1. 15 allocs    — page A becomes current and fills to 15.
 *   2. 15 allocs    — alloc 16 forces B as current; the next 14
 *                     fill B. A is now off every list (full).
 *   3. 15 allocs    — alloc 31 forces C as current; the next 14
 *                     fill C. B is also off every list now.
 *   4. Free 3 in A  — A enters partials with used_count = 12.
 *   5. Free 5 in B  — B enters partials with used_count = 10 and,
 *                     per the LIFO push, becomes the head of the
 *                     list. C is current and full.
 *   6. Alloc once   — try_current returns NULL (C is full), so
 *                     try_partials walks [B, A]. With the
 *                     utilization-aware pick, A wins despite B
 *                     being at the head.
 */
static int check_partials_pick_most_utilized(void)
{
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}

	uintptr_t a_objs[CLASS_31_PAGE_CAPACITY];
	uintptr_t b_objs[CLASS_31_PAGE_CAPACITY];
	uintptr_t c_objs[CLASS_31_PAGE_CAPACITY];

	for (int i = 0; i < CLASS_31_PAGE_CAPACITY; i++) {
		a_objs[i] = (uintptr_t)v8m_slab_pool_alloc(
		    &pool, CLASS_SMALL_LARGE, OWNER_THREAD);
		if (a_objs[i] == 0U) {
			v8m_slab_pool_destroy(&pool);
			return fail("page A fill alloc returned NULL");
		}
	}
	for (int i = 0; i < CLASS_31_PAGE_CAPACITY; i++) {
		b_objs[i] = (uintptr_t)v8m_slab_pool_alloc(
		    &pool, CLASS_SMALL_LARGE, OWNER_THREAD);
		if (b_objs[i] == 0U) {
			v8m_slab_pool_destroy(&pool);
			return fail("page B fill alloc returned NULL");
		}
	}
	for (int i = 0; i < CLASS_31_PAGE_CAPACITY; i++) {
		c_objs[i] = (uintptr_t)v8m_slab_pool_alloc(
		    &pool, CLASS_SMALL_LARGE, OWNER_THREAD);
		if (c_objs[i] == 0U) {
			v8m_slab_pool_destroy(&pool);
			return fail("page C fill alloc returned NULL");
		}
	}

	uintptr_t a_base = a_objs[0] & V8M_PAGE_MASK;
	uintptr_t b_base = b_objs[0] & V8M_PAGE_MASK;
	uintptr_t c_base = c_objs[0] & V8M_PAGE_MASK;
	if (a_base == b_base || a_base == c_base || b_base == c_base) {
		v8m_slab_pool_destroy(&pool);
		return fail("test setup produced pages sharing a page base");
	}

	for (int i = 0; i < 3; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)a_objs[i];
		struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
		(void)v8m_slab_pool_free(&pool, meta, obj);
	}
	for (int i = 0; i < 5; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)b_objs[i];
		struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
		(void)v8m_slab_pool_free(&pool, meta, obj);
	}

	void *picked =
	    v8m_slab_pool_alloc(&pool, CLASS_SMALL_LARGE, OWNER_THREAD);
	if (picked == NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("alloc after free pair returned NULL");
	}
	uintptr_t picked_base = (uintptr_t)picked & V8M_PAGE_MASK;
	if (picked_base != a_base) {
		v8m_slab_pool_destroy(&pool);
		return fail(
		    "partials pick chose B (less utilized) instead of A");
	}

	v8m_slab_pool_destroy(&pool);
	return 0;
}

/*
 * Drained-page cache (slab-page deferred munmap). When a slab page
 * becomes empty, release_slab_page parks it in the per-pool
 * drained cache instead of releasing immediately. A subsequent
 * acquire pops from the cache, saving an mmap/munmap round trip.
 *
 * Sequence: drain a page (alloc-free a single Tiny object), assert
 * the cache holds 1 page, alloc again (which pops from drained),
 * assert the cache is empty. Then sweep with max_idle_ticks=0 to
 * force-release the next drained page, asserting the sweep return
 * count matches.
 */
static int check_drained_cache_round_trip(void)
{
	struct v8m_slab_pool pool;
	if (v8m_slab_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}
	if (v8m_slab_pool_drained_count(&pool) != 0U) {
		v8m_slab_pool_destroy(&pool);
		return fail("drained count nonzero on init");
	}

	void *obj = v8m_slab_pool_alloc(&pool, CLASS_TINY, OWNER_THREAD);
	if (obj == NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("first alloc returned NULL");
	}
	struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
	(void)v8m_slab_pool_free(&pool, meta, obj);

	if (v8m_slab_pool_drained_count(&pool) != 1U) {
		v8m_slab_pool_destroy(&pool);
		return fail("drained count should be 1 after empty-page free");
	}

	/* The next alloc should pop from drained — observable as the
	 * drained_count dropping back to 0 (no new mmap was needed). */
	void *revival = v8m_slab_pool_alloc(&pool, CLASS_TINY, OWNER_THREAD);
	if (revival == NULL) {
		v8m_slab_pool_destroy(&pool);
		return fail("revival alloc returned NULL");
	}
	if (v8m_slab_pool_drained_count(&pool) != 0U) {
		v8m_slab_pool_destroy(&pool);
		return fail("drained count should be 0 after revival pop");
	}
	struct v8m_page_meta *revival_meta = v8m_ptr_to_meta(revival);
	(void)v8m_slab_pool_free(&pool, revival_meta, revival);
	if (v8m_slab_pool_drained_count(&pool) != 1U) {
		v8m_slab_pool_destroy(&pool);
		return fail("drained count should be 1 after revival free");
	}

	/* Force-release the cache via sweep with threshold 0. */
	size_t released = v8m_slab_pool_sweep_idle(&pool, 0U);
	if (released != 1U) {
		v8m_slab_pool_destroy(&pool);
		return fail("sweep should release exactly 1 page");
	}
	if (v8m_slab_pool_drained_count(&pool) != 0U) {
		v8m_slab_pool_destroy(&pool);
		return fail("drained count should be 0 after sweep");
	}

	v8m_slab_pool_destroy(&pool);
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
	status = check_single_alloc_free_per_class(CLASS_TINY);
	if (status != 0) {
		return status;
	}
	status = check_single_alloc_free_per_class(CLASS_SMALL_MID);
	if (status != 0) {
		return status;
	}
	status = check_single_alloc_free_per_class(CLASS_SMALL_LARGE);
	if (status != 0) {
		return status;
	}
	status = check_multi_page_allocation();
	if (status != 0) {
		return status;
	}
	status = check_full_to_partial_transition();
	if (status != 0) {
		return status;
	}
	status = check_partials_pick_most_utilized();
	if (status != 0) {
		return status;
	}
	status = check_drained_cache_round_trip();
	if (status != 0) {
		return status;
	}
	return check_concurrent();
}
