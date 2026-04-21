/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Dispatcher tests. Verify init/destroy, round-trip alloc/free for
 * representative sizes in every category (Tiny, Small, Medium,
 * Large, Huge), correct routing (slab vs buddy vs large), distinct
 * addresses across mixed-size allocations, NULL tolerance, and a
 * concurrent multi-thread mixed-size stress.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_dispatch.h"
#include "v8m_page.h"
#include "v8m_size_class.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_dispatch: %s\n", msg);
	return 1;
}

enum { WORKER_THREADS = 8, OPS_PER_WORKER = 64, MIXED_ALLOCS = 32 };

/* Representative sizes spanning every backend. The (size_t) cast on
 * the first multiplication operand keeps clang-tidy's
 * implicit-widening check happy. */
static const size_t representative_sizes[] = {
    1,			     /* Tiny class 0 (8 B) */
    8,			     /* Tiny class 0 boundary */
    64,			     /* Tiny class 7 */
    80,			     /* Small class 8 */
    512,		     /* Small mid */
    4096,		     /* Small max (class 31) */
    8192,		     /* Medium first (class 32, buddy) */
    131072,		     /* Medium high (class 36, buddy) */
    (size_t)256 * 1024,	     /* Medium max (class 37, buddy at 256 KiB) */
    (size_t)300 * 1024,	     /* Large (class 38, direct mmap) */
    (size_t)2 * 1024 * 1024, /* Large class 40 */
    (size_t)5 * 1024 * 1024  /* Huge (V8M_CLASS_HUGE) */
};

static int round_trip(struct v8m_dispatch *dispatch, size_t size)
{
	void *ptr = v8m_dispatch_alloc(dispatch, size);
	if (ptr == NULL) {
		return fail("alloc returned NULL");
	}
	/* Read+write across the full request range. */
	(void)memset(ptr, 0xC3, size > 0 ? size : 1);
	if (((const unsigned char *)ptr)[0] != 0xC3U) {
		v8m_dispatch_free(dispatch, ptr);
		return fail("first byte not writable");
	}
	if (size > 0 && ((const unsigned char *)ptr)[size - 1] != 0xC3U) {
		v8m_dispatch_free(dispatch, ptr);
		return fail("last byte not writable");
	}
	v8m_dispatch_free(dispatch, ptr);
	return 0;
}

static int check_round_trip_all_sizes(void)
{
	struct v8m_dispatch dispatch;
	if (v8m_dispatch_init(&dispatch) != 0) {
		return fail("init returned non-zero");
	}
	for (size_t i = 0;
	     i < sizeof(representative_sizes) / sizeof(representative_sizes[0]);
	     i++) {
		int status = round_trip(&dispatch, representative_sizes[i]);
		if (status != 0) {
			v8m_dispatch_destroy(&dispatch);
			return status;
		}
	}
	v8m_dispatch_destroy(&dispatch);
	return 0;
}

static int check_dispatch_routing(void)
{
	/* Confirm Tiny / Small route through the slab pool (page meta
	 * has matching size class), Large/Huge route through the
	 * direct path (page meta carries the Large marker), and
	 * Medium routes through the buddy pool (no v8m_page_meta at
	 * the page base). */
	struct v8m_dispatch dispatch;
	if (v8m_dispatch_init(&dispatch) != 0) {
		return fail("init returned non-zero");
	}

	void *tiny = v8m_dispatch_alloc(&dispatch, 8);
	void *small = v8m_dispatch_alloc(&dispatch, 200);
	void *medium = v8m_dispatch_alloc(&dispatch, 8192);
	void *large = v8m_dispatch_alloc(&dispatch, (size_t)300 * 1024);
	void *huge = v8m_dispatch_alloc(&dispatch, (size_t)5 * 1024 * 1024);

	if (tiny == NULL || small == NULL || medium == NULL || large == NULL ||
	    huge == NULL) {
		v8m_dispatch_free(&dispatch, tiny);
		v8m_dispatch_free(&dispatch, small);
		v8m_dispatch_free(&dispatch, medium);
		v8m_dispatch_free(&dispatch, large);
		v8m_dispatch_free(&dispatch, huge);
		v8m_dispatch_destroy(&dispatch);
		return fail("one of the routing-test allocs returned NULL");
	}

	const struct v8m_page_meta *tiny_meta = v8m_ptr_to_meta(tiny);
	if (!v8m_page_meta_valid(tiny_meta) || tiny_meta->size_class != 0) {
		v8m_dispatch_destroy(&dispatch);
		return fail("Tiny did not route through the slab pool");
	}
	const struct v8m_page_meta *small_meta = v8m_ptr_to_meta(small);
	if (!v8m_page_meta_valid(small_meta) ||
	    small_meta->size_class < V8M_SMALL_FIRST_CLASS ||
	    small_meta->size_class >= V8M_MEDIUM_FIRST_CLASS) {
		v8m_dispatch_destroy(&dispatch);
		return fail("Small did not route through the slab pool");
	}
	const struct v8m_page_meta *medium_meta = v8m_ptr_to_meta(medium);
	if (v8m_page_meta_valid(medium_meta)) {
		v8m_dispatch_destroy(&dispatch);
		return fail("Medium has v8m_page_meta at its page base "
			    "(expected buddy pool, no meta)");
	}
	const struct v8m_page_meta *large_meta = v8m_ptr_to_meta(large);
	if (!v8m_page_meta_valid(large_meta) ||
	    large_meta->size_class < V8M_MEDIUM_FIRST_CLASS) {
		v8m_dispatch_destroy(&dispatch);
		return fail("Large did not route through the direct mmap path");
	}
	const struct v8m_page_meta *huge_meta = v8m_ptr_to_meta(huge);
	if (!v8m_page_meta_valid(huge_meta) ||
	    huge_meta->size_class != UINT16_MAX) {
		v8m_dispatch_destroy(&dispatch);
		return fail("Huge did not route through the direct path with "
			    "the sentinel");
	}

	v8m_dispatch_free(&dispatch, tiny);
	v8m_dispatch_free(&dispatch, small);
	v8m_dispatch_free(&dispatch, medium);
	v8m_dispatch_free(&dispatch, large);
	v8m_dispatch_free(&dispatch, huge);
	v8m_dispatch_destroy(&dispatch);
	return 0;
}

static int check_distinct_mixed(void)
{
	struct v8m_dispatch dispatch;
	if (v8m_dispatch_init(&dispatch) != 0) {
		return fail("init returned non-zero");
	}

	uintptr_t addrs[MIXED_ALLOCS];
	for (int i = 0; i < MIXED_ALLOCS; i++) {
		size_t size =
		    representative_sizes[(size_t)i %
					 (sizeof(representative_sizes) /
					  sizeof(representative_sizes[0]))];
		void *obj = v8m_dispatch_alloc(&dispatch, size);
		if (obj == NULL) {
			for (int j = 0; j < i; j++) {
				/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
				v8m_dispatch_free(&dispatch, (void *)addrs[j]);
			}
			v8m_dispatch_destroy(&dispatch);
			return fail("mixed-size alloc returned NULL");
		}
		addrs[i] = (uintptr_t)obj;
	}

	for (int i = 0; i < MIXED_ALLOCS; i++) {
		for (int j = i + 1; j < MIXED_ALLOCS; j++) {
			if (addrs[i] == addrs[j]) {
				v8m_dispatch_destroy(&dispatch);
				return fail("two mixed-size allocs returned "
					    "the same pointer");
			}
		}
	}

	for (int i = 0; i < MIXED_ALLOCS; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		v8m_dispatch_free(&dispatch, (void *)addrs[i]);
	}
	v8m_dispatch_destroy(&dispatch);
	return 0;
}

static int check_null_tolerance(void)
{
	struct v8m_dispatch dispatch;
	if (v8m_dispatch_init(&dispatch) != 0) {
		return fail("init returned non-zero");
	}
	v8m_dispatch_free(&dispatch, NULL); /* must not crash */
	int local = 0;
	v8m_dispatch_free(&dispatch, &local); /* foreign pointer drop */
	v8m_dispatch_destroy(&dispatch);
	return 0;
}

static struct v8m_dispatch g_dispatch;
static atomic_int g_worker_failed = 0;

static void *worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < OPS_PER_WORKER; i++) {
		size_t size =
		    representative_sizes[(size_t)i %
					 (sizeof(representative_sizes) /
					  sizeof(representative_sizes[0]))];
		void *obj = v8m_dispatch_alloc(&g_dispatch, size);
		if (obj == NULL) {
			atomic_store(&g_worker_failed, 1);
			return NULL;
		}
		((unsigned char *)obj)[0] = 0xAAU;
		v8m_dispatch_free(&g_dispatch, obj);
	}
	return NULL;
}

static int check_concurrent(void)
{
	if (v8m_dispatch_init(&g_dispatch) != 0) {
		return fail("init returned non-zero");
	}
	atomic_store(&g_worker_failed, 0);

	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKER_THREADS];
	for (int i = 0; i < WORKER_THREADS; i++) {
		if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
			v8m_dispatch_destroy(&g_dispatch);
			return fail("pthread_create failed");
		}
	}
	for (int i = 0; i < WORKER_THREADS; i++) {
		(void)pthread_join(threads[i], NULL);
	}
	if (atomic_load(&g_worker_failed) != 0) {
		v8m_dispatch_destroy(&g_dispatch);
		return fail("a concurrent alloc returned NULL");
	}

	v8m_dispatch_destroy(&g_dispatch);
	return 0;
}

int main(void)
{
	int status = check_round_trip_all_sizes();
	if (status != 0) {
		return status;
	}
	status = check_dispatch_routing();
	if (status != 0) {
		return status;
	}
	status = check_distinct_mixed();
	if (status != 0) {
		return status;
	}
	status = check_null_tolerance();
	if (status != 0) {
		return status;
	}
	return check_concurrent();
}
