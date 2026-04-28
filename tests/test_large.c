/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Large/Huge direct-mmap path tests. Verify that alloc returns a
 * writable, unique region at the expected offset inside a
 * page-heap-backed mmap, that v8m_page_meta_valid recognises the
 * page, that free unmaps it (page_heap stats reflect it), and that
 * usable_size agrees with the mmap geometry. Also exercise the
 * Huge (>2 MiB) sentinel path and the invalid-input / overflow
 * branches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_internal.h"
#include "v8m_large.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_large: %s\n", msg);
	return 1;
}

#define OWNER_THREAD UINT64_C(0xF00DBABE)

enum {
	SIZE_LARGE_SMALL =
	    256 * 1024, /* class 38 - 256 KiB (just above Small max) */
	SIZE_LARGE_MAX = 2 * 1024 * 1024, /* class 40 - 2 MiB */
	SIZE_HUGE = 4 * 1024 * 1024, /* > 2 MiB - Huge path */
	MANY_REGIONS = 8
};

/*
 * "Mid" Large size used by check_stats_reflect_free. Must stay
 * strictly below V8M_HUGE_PAGE_SIZE so the page-heap does NOT
 * attempt to satisfy the request from the global anchor reservation
 * (the anchor path skips the discrete mmap and so wouldn't advance
 * mmap_calls, breaking the assertion below). On x86_64/aarch64/etc.
 * V8M_HUGE_PAGE_SIZE is 2 MiB, so 1 MiB. On s390x the kernel huge
 * page is 1 MiB, so 512 KiB.
 */
#define SIZE_LARGE_MID (V8M_HUGE_PAGE_SIZE / 2)

static int check_basic_large(void)
{
	void *obj = v8m_large_alloc(SIZE_LARGE_SMALL, OWNER_THREAD);
	if (obj == NULL) {
		return fail("alloc(256 KiB) returned NULL");
	}

	/* User pointer must sit at offset V8M_SLAB_HEADER_SIZE into a
	 * V8M_PAGE_SIZE-aligned region. */
	uintptr_t base = (uintptr_t)obj & V8M_PAGE_MASK;
	if ((uintptr_t)obj - base != V8M_SLAB_HEADER_SIZE) {
		v8m_large_free(obj);
		return fail("user pointer not at V8M_SLAB_HEADER_SIZE offset");
	}
	if (base % V8M_PAGE_SIZE != 0) {
		v8m_large_free(obj);
		return fail("region base not V8M_PAGE_SIZE-aligned");
	}

	/* Must be readable and writable over the entire requested range. */
	(void)memset(obj, 0x5A, SIZE_LARGE_SMALL);
	if (((const unsigned char *)obj)[0] != 0x5AU ||
	    ((const unsigned char *)obj)[SIZE_LARGE_SMALL - 1U] != 0x5AU) {
		v8m_large_free(obj);
		return fail(
		    "region not readable/writable across the full request");
	}

	/* Generic reverse-lookup + magic check must accept the region. */
	const struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
	if (!v8m_page_meta_valid(meta)) {
		v8m_large_free(obj);
		return fail("page magic check rejected a Large allocation");
	}
	if (meta->capacity != 1) {
		v8m_large_free(obj);
		return fail("Large meta capacity != 1");
	}
	if (meta->object_size != 0) {
		v8m_large_free(obj);
		return fail("Large meta object_size != 0");
	}
	if (meta->owner_thread != OWNER_THREAD) {
		v8m_large_free(obj);
		return fail("Large meta owner_thread mis-stamped");
	}

	size_t usable = v8m_large_usable_size(obj);
	if (usable < SIZE_LARGE_SMALL) {
		v8m_large_free(obj);
		return fail("usable_size smaller than request");
	}

	v8m_large_free(obj);
	return 0;
}

static int check_huge_path(void)
{
	/* > 2 MiB routes through the Huge sentinel but otherwise
	 * shares the Large machinery. */
	void *obj = v8m_large_alloc(SIZE_HUGE, OWNER_THREAD);
	if (obj == NULL) {
		return fail("Huge alloc returned NULL");
	}
	const struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
	if (!v8m_page_meta_valid(meta)) {
		v8m_large_free(obj);
		return fail("Huge allocation failed magic check");
	}
	if (meta->size_class != UINT16_MAX) {
		v8m_large_free(obj);
		return fail("Huge size_class is not the sentinel");
	}
	if (v8m_large_usable_size(obj) < SIZE_HUGE) {
		v8m_large_free(obj);
		return fail("Huge usable_size smaller than request");
	}

	/* Touch the first and last pages. */
	((unsigned char *)obj)[0] = 0xFFU;
	((unsigned char *)obj)[SIZE_HUGE - 1U] = 0xFFU;

	v8m_large_free(obj);
	return 0;
}

static int check_distinct_regions(void)
{
	void *ptrs[MANY_REGIONS];
	for (int i = 0; i < MANY_REGIONS; i++) {
		ptrs[i] = v8m_large_alloc(SIZE_LARGE_SMALL, OWNER_THREAD);
		if (ptrs[i] == NULL) {
			for (int j = 0; j < i; j++) {
				v8m_large_free(ptrs[j]);
			}
			return fail("alloc returned NULL mid-sequence");
		}
	}
	for (int i = 0; i < MANY_REGIONS; i++) {
		for (int j = i + 1; j < MANY_REGIONS; j++) {
			uintptr_t addr_i = (uintptr_t)ptrs[i];
			uintptr_t addr_j = (uintptr_t)ptrs[j];
			uintptr_t diff = (addr_i > addr_j) ? (addr_i - addr_j)
							   : (addr_j - addr_i);
			if (diff < SIZE_LARGE_SMALL) {
				for (int k = 0; k < MANY_REGIONS; k++) {
					v8m_large_free(ptrs[k]);
				}
				return fail("two Large regions overlap");
			}
		}
	}
	for (int i = 0; i < MANY_REGIONS; i++) {
		v8m_large_free(ptrs[i]);
	}
	return 0;
}

static int check_stats_reflect_free(void)
{
	struct v8m_page_heap_stats before = {0};
	struct v8m_page_heap_stats after_alloc = {0};
	struct v8m_page_heap_stats after_free = {0};

	v8m_page_heap_get_stats(&before);
	const void *obj = v8m_large_alloc(SIZE_LARGE_MID, OWNER_THREAD);
	if (obj == NULL) {
		return fail("1 MiB alloc returned NULL");
	}
	v8m_page_heap_get_stats(&after_alloc);
	if (after_alloc.mmap_calls <= before.mmap_calls) {
		v8m_large_free(obj);
		return fail("mmap_calls did not advance on Large alloc");
	}
	v8m_large_free(obj);
	v8m_page_heap_get_stats(&after_free);
	if (after_free.munmap_calls <= after_alloc.munmap_calls) {
		return fail("munmap_calls did not advance on Large free");
	}
	return 0;
}

static int check_invalid_inputs(void)
{
	if (v8m_large_alloc(0, OWNER_THREAD) != NULL) {
		return fail("alloc(0) did not return NULL");
	}
	if (v8m_large_alloc(SIZE_MAX, OWNER_THREAD) != NULL) {
		return fail("alloc(SIZE_MAX) did not return NULL (overflow)");
	}
	/* Tolerance of NULL in the free / usable_size paths. */
	v8m_large_free(NULL);
	if (v8m_large_usable_size(NULL) != 0) {
		return fail("usable_size(NULL) != 0");
	}
	return 0;
}

int main(void)
{
	int status = check_basic_large();
	if (status != 0) {
		return status;
	}
	status = check_huge_path();
	if (status != 0) {
		return status;
	}
	status = check_distinct_regions();
	if (status != 0) {
		return status;
	}
	status = check_stats_reflect_free();
	if (status != 0) {
		return status;
	}
	return check_invalid_inputs();
}
