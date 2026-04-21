/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Page heap tests. Verify alignment, input validation, read/write
 * access, distinct addresses across allocations, and that statistics
 * increment in step with mmap/munmap/madvise calls.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "v8m_internal.h"
#include "v8m_page_heap.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_page_heap: %s\n", msg);
	return 1;
}

enum {
	BIG_ALIGNMENT = 2 * 1024 * 1024, /* 2 MiB (HugePage-sized) */
	MANY_REGIONS = 16
};

static bool is_aligned(const void *ptr, size_t alignment)
{
	return ((uintptr_t)ptr & (alignment - 1U)) == 0;
}

static int check_basic_alignment(void)
{
	void *ptr = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (ptr == NULL) {
		return fail("alloc(64K, 64K) returned NULL");
	}
	if (!is_aligned(ptr, V8M_PAGE_SIZE)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("alloc(64K, 64K) not V8M_PAGE_SIZE-aligned");
	}
	/* Must be readable and writable over the full requested range. */
	(void)memset(ptr, 0xAA, V8M_PAGE_SIZE);
	if (((const unsigned char *)ptr)[0] != 0xAAU ||
	    ((const unsigned char *)ptr)[V8M_PAGE_SIZE - 1U] != 0xAAU) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("allocated region not readable/writable");
	}
	v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
	return 0;
}

static int check_big_alignment(void)
{
	void *ptr = v8m_page_heap_alloc(BIG_ALIGNMENT, BIG_ALIGNMENT);
	if (ptr == NULL) {
		return fail("alloc(2M, 2M) returned NULL");
	}
	if (!is_aligned(ptr, BIG_ALIGNMENT)) {
		v8m_page_heap_free(ptr, BIG_ALIGNMENT);
		return fail("alloc(2M, 2M) not 2 MiB-aligned");
	}
	v8m_page_heap_free(ptr, BIG_ALIGNMENT);
	return 0;
}

static int check_invalid_arguments(void)
{
	if (v8m_page_heap_alloc(0, V8M_PAGE_SIZE) != NULL) {
		return fail("alloc(0, _) did not return NULL");
	}
	if (v8m_page_heap_alloc(V8M_PAGE_SIZE, 4096) != NULL) {
		return fail("alloc with alignment < V8M_PAGE_SIZE succeeded");
	}
	/* 3 * V8M_PAGE_SIZE is not a power of two. */
	if (v8m_page_heap_alloc(V8M_PAGE_SIZE, 3U * V8M_PAGE_SIZE) != NULL) {
		return fail("alloc with non-power-of-two alignment succeeded");
	}
	if (v8m_page_heap_alloc(SIZE_MAX, V8M_PAGE_SIZE) != NULL) {
		return fail("alloc with overflowing bytes+alignment succeeded");
	}
	return 0;
}

static int check_distinct_addresses(void)
{
	void *ptrs[MANY_REGIONS] = {NULL};
	for (int i = 0; i < MANY_REGIONS; i++) {
		ptrs[i] = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
		if (ptrs[i] == NULL) {
			for (int j = 0; j < i; j++) {
				v8m_page_heap_free(ptrs[j], V8M_PAGE_SIZE);
			}
			return fail("alloc returned NULL mid-sequence");
		}
		if (!is_aligned(ptrs[i], V8M_PAGE_SIZE)) {
			for (int j = 0; j <= i; j++) {
				v8m_page_heap_free(ptrs[j], V8M_PAGE_SIZE);
			}
			return fail("mid-sequence allocation not aligned");
		}
	}
	for (int i = 0; i < MANY_REGIONS; i++) {
		for (int j = i + 1; j < MANY_REGIONS; j++) {
			if (ptrs[i] == ptrs[j]) {
				for (int k = 0; k < MANY_REGIONS; k++) {
					v8m_page_heap_free(ptrs[k],
							   V8M_PAGE_SIZE);
				}
				return fail("two allocations returned the same "
					    "address");
			}
		}
	}
	for (int i = 0; i < MANY_REGIONS; i++) {
		v8m_page_heap_free(ptrs[i], V8M_PAGE_SIZE);
	}
	return 0;
}

static int check_stats_advance(void)
{
	struct v8m_page_heap_stats before = {0};
	struct v8m_page_heap_stats after = {0};

	v8m_page_heap_get_stats(&before);
	void *ptr = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (ptr == NULL) {
		return fail("alloc returned NULL during stats check");
	}
	v8m_page_heap_get_stats(&after);
	if (after.mmap_calls <= before.mmap_calls) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("mmap_calls did not increase");
	}
	if (after.bytes_mapped <= before.bytes_mapped) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("bytes_mapped did not increase");
	}

	v8m_page_heap_advise_dont_need(ptr, V8M_PAGE_SIZE);
	struct v8m_page_heap_stats after_advise = {0};
	v8m_page_heap_get_stats(&after_advise);
	if (after_advise.advise_calls <= after.advise_calls) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("advise_calls did not increase");
	}

	v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
	struct v8m_page_heap_stats after_free = {0};
	v8m_page_heap_get_stats(&after_free);
	if (after_free.munmap_calls <= after_advise.munmap_calls) {
		return fail("munmap_calls did not increase on free");
	}
	if (after_free.bytes_unmapped <= after_advise.bytes_unmapped) {
		return fail("bytes_unmapped did not increase on free");
	}
	return 0;
}

static int check_advise_and_free_tolerate_null(void)
{
	/* Tolerant of NULL / zero bytes — must not crash. */
	v8m_page_heap_free(NULL, V8M_PAGE_SIZE);
	v8m_page_heap_free(NULL, 0);
	v8m_page_heap_advise_dont_need(NULL, V8M_PAGE_SIZE);
	v8m_page_heap_advise_dont_need(NULL, 0);
	v8m_page_heap_get_stats(NULL);
	return 0;
}

static int check_owns_predicate(void)
{
	if (v8m_page_heap_owns(NULL)) {
		return fail("owns(NULL) returned true");
	}

	void *ptr = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (ptr == NULL) {
		return fail("alloc for owns check returned NULL");
	}
	if (!v8m_page_heap_owns(ptr)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(start of region) returned false");
	}
	/* Mid-region and last byte must both register as owned. */
	const unsigned char *mid =
	    (const unsigned char *)ptr + (V8M_PAGE_SIZE / 2U);
	if (!v8m_page_heap_owns(mid)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(mid) returned false");
	}
	const unsigned char *last =
	    (const unsigned char *)ptr + (V8M_PAGE_SIZE - 1U);
	if (!v8m_page_heap_owns(last)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(last byte) returned false");
	}
	/* The first byte just past the end is unowned. */
	const unsigned char *past = (const unsigned char *)ptr + V8M_PAGE_SIZE;
	if (v8m_page_heap_owns(past)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(end-exclusive) returned true");
	}

	/* A stack address — definitely never v8malloc-owned. */
	int stack_local = 0;
	if (v8m_page_heap_owns(&stack_local)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(stack address) returned true");
	}

	v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
	if (v8m_page_heap_owns(ptr)) {
		return fail("owns(freed pointer) returned true");
	}
	return 0;
}

int main(void)
{
	int status = check_basic_alignment();
	if (status != 0) {
		return status;
	}
	status = check_big_alignment();
	if (status != 0) {
		return status;
	}
	status = check_invalid_arguments();
	if (status != 0) {
		return status;
	}
	status = check_distinct_addresses();
	if (status != 0) {
		return status;
	}
	status = check_stats_advance();
	if (status != 0) {
		return status;
	}
	/* check_advise_and_free_tolerate_null only ever returns 0;
	 * cppcheck flags the post-call status check as dead code. */
	(void)check_advise_and_free_tolerate_null();
	return check_owns_predicate();
}
