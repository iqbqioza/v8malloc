/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Verify the bootstrap allocator's contract: allocations are
 * V8M_BOOTSTRAP_ALIGN-aligned, monotonically advance, don't overlap
 * (even under concurrent fetch-and-add races), and are recognized by
 * v8m_ptr_is_bootstrap() while foreign pointers are not.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v8m_bootstrap.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_bootstrap: %s\n", msg);
	return 1;
}

enum {
	WORKER_THREADS = 8,
	ALLOCS_PER_WORKER = 32,
	TOTAL_POINTERS = WORKER_THREADS * ALLOCS_PER_WORKER,
	MIN_ALLOC_SIZE = 16,
	ALLOC_SIZE_STEPS = 4
};

struct slot {
	void *ptr;
	size_t aligned_size;
};

static struct slot g_slots[TOTAL_POINTERS];
static atomic_int g_slot_cursor = 0;

static size_t align_up(size_t n)
{
	return (n + (V8M_BOOTSTRAP_ALIGN - 1U)) &
	       ~(size_t)(V8M_BOOTSTRAP_ALIGN - 1U);
}

static void *worker(void *arg)
{
	(void)arg;
	for (int iter = 0; iter < ALLOCS_PER_WORKER; iter++) {
		size_t req =
		    MIN_ALLOC_SIZE +
		    ((size_t)(iter % ALLOC_SIZE_STEPS) * MIN_ALLOC_SIZE);
		void *ptr = v8m_bootstrap_alloc(req);
		int idx = atomic_fetch_add(&g_slot_cursor, 1);
		g_slots[idx].ptr = ptr;
		g_slots[idx].aligned_size = align_up(req);
	}
	return NULL;
}

static int compare_by_addr(const void *lhs, const void *rhs)
{
	uintptr_t left = (uintptr_t)((const struct slot *)lhs)->ptr;
	uintptr_t right = (uintptr_t)((const struct slot *)rhs)->ptr;
	if (left < right) {
		return -1;
	}
	if (left > right) {
		return 1;
	}
	return 0;
}

static int check_sequential(void)
{
	void *first = v8m_bootstrap_alloc(1);
	void *second = v8m_bootstrap_alloc(7);
	void *third = v8m_bootstrap_alloc(V8M_BOOTSTRAP_ALIGN);
	if (first == NULL || second == NULL || third == NULL) {
		return fail("bootstrap returned NULL on cold path");
	}
	if ((uintptr_t)first % V8M_BOOTSTRAP_ALIGN != 0 ||
	    (uintptr_t)second % V8M_BOOTSTRAP_ALIGN != 0 ||
	    (uintptr_t)third % V8M_BOOTSTRAP_ALIGN != 0) {
		return fail("bootstrap pointer not aligned");
	}
	if ((uintptr_t)second <= (uintptr_t)first ||
	    (uintptr_t)third <= (uintptr_t)second) {
		return fail("bootstrap allocations did not advance");
	}
	if ((uintptr_t)second - (uintptr_t)first != V8M_BOOTSTRAP_ALIGN) {
		return fail("request size 1 not rounded up to alignment");
	}
	if ((uintptr_t)third - (uintptr_t)second != V8M_BOOTSTRAP_ALIGN) {
		return fail("request size 7 not rounded up to alignment");
	}
	return 0;
}

static int check_range_predicate(void)
{
	const void *probe = v8m_bootstrap_alloc(V8M_BOOTSTRAP_ALIGN);
	if (!v8m_ptr_is_bootstrap(probe)) {
		return fail(
		    "v8m_ptr_is_bootstrap rejected a bootstrap pointer");
	}
	int stack_var = 0;
	if (v8m_ptr_is_bootstrap(&stack_var)) {
		return fail("stack pointer mistaken for bootstrap");
	}
	void *heap = malloc(V8M_BOOTSTRAP_ALIGN);
	if (heap == NULL) {
		return fail("libc malloc failed");
	}
	bool heap_match = v8m_ptr_is_bootstrap(heap);
	free(heap);
	if (heap_match) {
		return fail("libc heap pointer mistaken for bootstrap");
	}
	if (v8m_ptr_is_bootstrap(NULL)) {
		return fail("NULL mistaken for bootstrap");
	}
	return 0;
}

static int check_no_overlaps(void)
{
	for (int i = 0; i < TOTAL_POINTERS; i++) {
		if (g_slots[i].ptr == NULL) {
			return fail("concurrent: NULL pointer recorded");
		}
		if (!v8m_ptr_is_bootstrap(g_slots[i].ptr)) {
			return fail("concurrent: pointer out of range");
		}
		if ((uintptr_t)g_slots[i].ptr % V8M_BOOTSTRAP_ALIGN != 0) {
			return fail("concurrent: misaligned pointer");
		}
		if (i > 0) {
			uintptr_t prev_end = (uintptr_t)g_slots[i - 1].ptr +
					     g_slots[i - 1].aligned_size;
			if ((uintptr_t)g_slots[i].ptr < prev_end) {
				return fail(
				    "concurrent: overlapping allocations");
			}
		}
	}
	return 0;
}

static int check_concurrent(void)
{
	/* pthread.h is included at the top of the file; clang-tidy's
	 * include-cleaner does not trace pthread_t back to it on glibc. */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKER_THREADS];
	for (int i = 0; i < WORKER_THREADS; i++) {
		if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
			return fail("pthread_create failed");
		}
	}
	for (int i = 0; i < WORKER_THREADS; i++) {
		(void)pthread_join(threads[i], NULL);
	}
	if (atomic_load(&g_slot_cursor) != TOTAL_POINTERS) {
		return fail("concurrent worker count mismatch");
	}

	qsort(g_slots, (size_t)TOTAL_POINTERS, sizeof(g_slots[0]),
	      compare_by_addr);
	return check_no_overlaps();
}

int main(void)
{
	int status = check_sequential();
	if (status != 0) {
		return status;
	}
	status = check_range_predicate();
	if (status != 0) {
		return status;
	}
	return check_concurrent();
}
