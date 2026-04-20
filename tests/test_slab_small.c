/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Small slab-page tests. Verify that init stamps the expected
 * metadata, that the full capacity is reachable with distinct,
 * naturally-aligned, in-range pointers, that free reclaims slots and
 * signals the empty transition exactly once, and that the alloc-free
 * churn path does not drift. Also verify the class-31 (4 KiB)
 * alignment quirk: its data area is bumped up so returned pointers
 * are 4 KiB-aligned.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_slab_small.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_slab_small: %s\n", msg);
	return 1;
}

#define OWNER_THREAD UINT64_C(0xCAFEF00D)

enum {
	MANY_FREE_CYCLES = 3,
	STRESS_ITERATIONS = 16384,
	CLASS_LOW = 8,
	CLASS_MID = 19,
	CLASS_LARGE = 31
};

static void *alloc_page(void)
{
	void *page = aligned_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (page == NULL) {
		return NULL;
	}
	(void)memset(page, 0, V8M_PAGE_SIZE);
	return page;
}

static bool object_in_data_area(const struct v8m_page_meta *meta,
				const void *obj)
{
	uintptr_t base =
	    (uintptr_t)meta + v8m_slab_small_data_offset(meta->object_size);
	uintptr_t end = (uintptr_t)meta + V8M_PAGE_SIZE;
	uintptr_t addr = (uintptr_t)obj;
	return addr >= base && addr < end;
}

/* Natural alignment of an object size: the largest power of two that
 * divides it. For the sizes in our class table this matches the
 * alignment guarantee the design doc advertises. */
static size_t natural_alignment(size_t size)
{
	if (size == 0) {
		return 1;
	}
	return size & (size_t)(0U - size);
}

static int check_init_sets_metadata(void)
{
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (init)");
	}
	v8m_slab_small_init(page, CLASS_MID, OWNER_THREAD);
	struct v8m_page_meta *meta = page;

	if (meta->magic != V8M_MAGIC) {
		free(page);
		return fail("V8M_MAGIC not stamped");
	}
	if (meta->size_class != CLASS_MID) {
		free(page);
		return fail("size_class mis-stamped");
	}
	if (meta->object_size != 512) {
		free(page);
		return fail("object_size mis-stamped (class 19 -> 512)");
	}
	if (meta->capacity != v8m_slab_small_capacity_for(meta->object_size)) {
		free(page);
		return fail("capacity disagrees with capacity_for");
	}
	if (meta->free_list_head == NULL && meta->capacity > 0) {
		free(page);
		return fail("free_list_head NULL after init of non-empty slab");
	}
	if (!v8m_slab_small_is_empty(meta)) {
		free(page);
		return fail("slab not empty immediately after init");
	}
	if (v8m_slab_small_is_full(meta)) {
		free(page);
		return fail("slab reported full immediately after init");
	}
	if (meta->owner_thread != OWNER_THREAD) {
		free(page);
		return fail("owner_thread mis-stamped");
	}

	free(page);
	return 0;
}

static int check_full_capacity_reachable_for(uint32_t size_class)
{
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (capacity)");
	}
	v8m_slab_small_init(page, size_class, OWNER_THREAD);
	struct v8m_page_meta *meta = page;

	uint32_t capacity = meta->capacity;
	uint32_t object_size = meta->object_size;
	uintptr_t *addrs =
	    (uintptr_t *)malloc((size_t)capacity * sizeof(uintptr_t));
	if (addrs == NULL) {
		free(page);
		return fail("malloc(addrs) failed");
	}

	for (uint32_t i = 0; i < capacity; i++) {
		void *obj = v8m_slab_small_alloc(meta);
		if (obj == NULL) {
			free(addrs);
			free(page);
			return fail("alloc returned NULL before capacity");
		}
		if (!object_in_data_area(meta, obj)) {
			free(addrs);
			free(page);
			return fail("alloc returned pointer outside data area");
		}
		if ((uintptr_t)obj % natural_alignment(object_size) != 0) {
			free(addrs);
			free(page);
			return fail("alloc pointer not naturally aligned");
		}
		addrs[i] = (uintptr_t)obj;
	}

	if (v8m_slab_small_alloc(meta) != NULL) {
		free(addrs);
		free(page);
		return fail("alloc succeeded past capacity");
	}
	if (!v8m_slab_small_is_full(meta)) {
		free(addrs);
		free(page);
		return fail("is_full false when slab exhausted");
	}

	for (uint32_t i = 0; i + 1 < capacity; i++) {
		for (uint32_t j = i + 1; j < capacity; j++) {
			if (addrs[i] == addrs[j]) {
				free(addrs);
				free(page);
				return fail(
				    "alloc returned the same slot twice");
			}
		}
	}

	uint32_t empty_reports = 0;
	for (uint32_t i = 0; i < capacity; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)addrs[i];
		bool became_empty = v8m_slab_small_free(meta, obj);
		if (became_empty) {
			empty_reports++;
		}
	}
	if (empty_reports != 1) {
		free(addrs);
		free(page);
		return fail(
		    "v8m_slab_small_free did not signal empty exactly once");
	}
	if (!v8m_slab_small_is_empty(meta)) {
		free(addrs);
		free(page);
		return fail("is_empty false after freeing every slot");
	}

	free(addrs);
	free(page);
	return 0;
}

static int check_slot_reuse(void)
{
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (reuse)");
	}
	v8m_slab_small_init(page, CLASS_LOW, OWNER_THREAD);
	struct v8m_page_meta *meta = page;
	uint32_t capacity = meta->capacity;

	for (uint32_t cycle = 0; cycle < MANY_FREE_CYCLES; cycle++) {
		for (uint32_t i = 0; i < STRESS_ITERATIONS; i++) {
			void *obj = v8m_slab_small_alloc(meta);
			if (obj == NULL) {
				free(page);
				return fail(
				    "alloc-then-free churn produced NULL");
			}
			(void)v8m_slab_small_free(meta, obj);
		}
		if (!v8m_slab_small_is_empty(meta)) {
			free(page);
			return fail("slab not empty after alloc-free churn");
		}
	}

	for (uint32_t i = 0; i < capacity; i++) {
		if (v8m_slab_small_alloc(meta) == NULL) {
			free(page);
			return fail("alloc failed after stress cycles");
		}
	}

	free(page);
	return 0;
}

static int check_class_31_page_alignment(void)
{
	/* Class 31 serves 4 KiB objects; the data-area offset bumps up
	 * to object_size so the first slot is 4 KiB-aligned. Capacity
	 * drops by one slot (15 instead of the naive 15.5 rounded). */
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (class 31)");
	}
	v8m_slab_small_init(page, CLASS_LARGE, OWNER_THREAD);
	struct v8m_page_meta *meta = page;

	if (meta->object_size != 4096) {
		free(page);
		return fail("class 31 object_size != 4096");
	}
	if (v8m_slab_small_data_offset(4096) != 4096) {
		free(page);
		return fail("class 31 data offset not bumped to 4 KiB");
	}
	if (meta->capacity != 15) {
		free(page);
		return fail("class 31 capacity != 15 (design: 15)");
	}

	/* Every issued pointer must be 4 KiB-aligned. */
	for (uint32_t i = 0; i < meta->capacity; i++) {
		void *obj = v8m_slab_small_alloc(meta);
		if (obj == NULL) {
			free(page);
			return fail("class 31 alloc returned NULL early");
		}
		if ((uintptr_t)obj % 4096U != 0U) {
			free(page);
			return fail("class 31 pointer not 4 KiB-aligned");
		}
	}
	if (v8m_slab_small_alloc(meta) != NULL) {
		free(page);
		return fail("class 31 alloc succeeded past capacity");
	}

	free(page);
	return 0;
}

int main(void)
{
	int status = check_init_sets_metadata();
	if (status != 0) {
		return status;
	}
	status = check_full_capacity_reachable_for(CLASS_LOW);
	if (status != 0) {
		return status;
	}
	status = check_full_capacity_reachable_for(CLASS_MID);
	if (status != 0) {
		return status;
	}
	status = check_full_capacity_reachable_for(CLASS_LARGE);
	if (status != 0) {
		return status;
	}
	status = check_class_31_page_alignment();
	if (status != 0) {
		return status;
	}
	return check_slot_reuse();
}
