/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny slab-page tests. Verify that init sets the expected capacity,
 * that alloc returns pointers within the data area and respects the
 * size class's natural alignment, that the full capacity is
 * reachable, that free reclaims slots, and that is_empty / is_full
 * track used_count correctly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_slab_tiny.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_slab_tiny: %s\n", msg);
	return 1;
}

/* Sentinel thread id used throughout the test. UINT64_C keeps it out
 * of the narrower `int` domain that an enumerator is stuck with. */
#define OWNER_THREAD UINT64_C(0xDEADBEEF)

enum { MANY_FREE_CYCLES = 3, STRESS_ITERATIONS = 16384 };

/* Use the page heap directly (mmap-backed) rather than libc's
 * aligned_alloc — once the public free() hijack from v8m_api.c is
 * linked in, libc's aligned_alloc/free pair would route fake test
 * pages through v8m_dispatch_free, which would try to munmap heap
 * memory after seeing the V8M_MAGIC we stamped on the page. */
static void *alloc_page(void)
{
	void *page = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (page == NULL) {
		return NULL;
	}
	(void)memset(page, 0, V8M_PAGE_SIZE);
	return page;
}

static void release_page(void *page)
{
	v8m_page_heap_free(page, V8M_PAGE_SIZE);
}

static bool object_in_data_area(const struct v8m_page_meta *meta,
				const void *obj)
{
	uintptr_t base = (uintptr_t)meta + V8M_SLAB_HEADER_SIZE;
	uintptr_t end = (uintptr_t)meta + V8M_PAGE_SIZE;
	uintptr_t addr = (uintptr_t)obj;
	return addr >= base && addr < end;
}

/* Natural alignment of an object size: the largest power of two that
 * divides it. Matches the alignment guarantee the design advertises
 * for every Tiny class. */
static size_t natural_alignment(size_t size)
{
	if (size == 0) {
		return 1;
	}
	return size & (size_t)(0U - size);
}

static int check_init_sets_metadata(void)
{
	/* Init class 7 (64 B) and verify the fields are the ones
	 * v8m_size_class / the size-class table nail down. */
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (init)");
	}
	v8m_slab_tiny_init(page, 7, OWNER_THREAD);
	struct v8m_page_meta *meta = page;

	if (meta->magic != V8M_MAGIC) {
		release_page(page);
		return fail("V8M_MAGIC not stamped");
	}
	if (meta->size_class != 7) {
		release_page(page);
		return fail("size_class mis-stamped");
	}
	if (meta->object_size != 64) {
		release_page(page);
		return fail("object_size mis-stamped");
	}
	if (meta->capacity != v8m_slab_tiny_capacity_for(64)) {
		release_page(page);
		return fail("capacity disagrees with capacity_for");
	}
	if (!v8m_slab_tiny_is_empty(meta)) {
		release_page(page);
		return fail("slab not empty immediately after init");
	}
	if (v8m_slab_tiny_is_full(meta)) {
		release_page(page);
		return fail("slab reported full immediately after init");
	}
	if (meta->owner_thread != OWNER_THREAD) {
		release_page(page);
		return fail("owner_thread mis-stamped");
	}

	release_page(page);
	return 0;
}

static int check_full_capacity_reachable_for(uint32_t size_class)
{
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (capacity)");
	}
	v8m_slab_tiny_init(page, size_class, OWNER_THREAD);
	struct v8m_page_meta *meta = page;

	uint32_t capacity = meta->capacity;
	uint32_t object_size = meta->object_size;
	/* Track every allocated pointer as an integer; this avoids the
	 * multi-level pointer conversions that flag clang-tidy and still
	 * serves every check we need (equality, passing back to free). */
	uintptr_t *addrs =
	    (uintptr_t *)malloc((size_t)capacity * sizeof(uintptr_t));
	if (addrs == NULL) {
		release_page(page);
		return fail("malloc(addrs) failed");
	}

	for (uint32_t i = 0; i < capacity; i++) {
		void *obj = v8m_slab_tiny_alloc(meta);
		if (obj == NULL) {
			free(addrs);
			release_page(page);
			return fail("alloc returned NULL before capacity");
		}
		if (!object_in_data_area(meta, obj)) {
			free(addrs);
			release_page(page);
			return fail("alloc returned pointer outside data area");
		}
		if ((uintptr_t)obj % natural_alignment(object_size) != 0) {
			free(addrs);
			release_page(page);
			return fail("alloc pointer not naturally aligned");
		}
		addrs[i] = (uintptr_t)obj;
	}

	/* Exhausted — one more alloc must fail. */
	if (v8m_slab_tiny_alloc(meta) != NULL) {
		free(addrs);
		release_page(page);
		return fail("alloc succeeded past capacity");
	}
	if (!v8m_slab_tiny_is_full(meta)) {
		free(addrs);
		release_page(page);
		return fail("is_full false when slab exhausted");
	}

	/* All pointers must be pairwise distinct. */
	for (uint32_t i = 0; i + 1 < capacity; i++) {
		for (uint32_t j = i + 1; j < capacity; j++) {
			if (addrs[i] == addrs[j]) {
				free(addrs);
				release_page(page);
				return fail(
				    "alloc returned the same slot twice");
			}
		}
	}

	/* Free everything — only the final call may report empty. The
	 * int-to-ptr cast is intentional; we stored the issued pointers
	 * as uintptr_t above. */
	uint32_t empty_reports = 0;
	for (uint32_t i = 0; i < capacity; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		const void *obj = (const void *)addrs[i];
		bool became_empty = v8m_slab_tiny_free(meta, obj);
		if (became_empty) {
			empty_reports++;
		}
	}
	if (empty_reports != 1) {
		free(addrs);
		release_page(page);
		return fail(
		    "v8m_slab_tiny_free did not signal empty exactly once");
	}
	if (!v8m_slab_tiny_is_empty(meta)) {
		free(addrs);
		release_page(page);
		return fail("is_empty false after freeing every slot");
	}
	if (v8m_slab_tiny_is_full(meta)) {
		free(addrs);
		release_page(page);
		return fail("is_full true after freeing every slot");
	}

	free(addrs);
	release_page(page);
	return 0;
}

static int check_slot_reuse(void)
{
	/* Stress the search_hint rewind path: alloc-then-free many
	 * times in succession (each pair leaves the slab empty, so the
	 * next alloc must succeed), then prove we can still fill the
	 * slab completely afterwards. */
	void *page = alloc_page();
	if (page == NULL) {
		return fail("aligned_alloc failed (reuse)");
	}
	v8m_slab_tiny_init(page, 0, OWNER_THREAD);
	struct v8m_page_meta *meta = page;
	uint32_t capacity = meta->capacity;

	for (uint32_t cycle = 0; cycle < MANY_FREE_CYCLES; cycle++) {
		for (uint32_t i = 0; i < STRESS_ITERATIONS; i++) {
			const void *obj = v8m_slab_tiny_alloc(meta);
			if (obj == NULL) {
				release_page(page);
				return fail(
				    "alloc-then-free churn produced NULL");
			}
			(void)v8m_slab_tiny_free(meta, obj);
		}
		if (!v8m_slab_tiny_is_empty(meta)) {
			release_page(page);
			return fail("slab not empty after alloc-free churn");
		}
	}

	/* After the churn, fully fill once more to prove no state rot. */
	for (uint32_t i = 0; i < capacity; i++) {
		if (v8m_slab_tiny_alloc(meta) == NULL) {
			release_page(page);
			return fail("alloc failed after stress cycles");
		}
	}

	release_page(page);
	return 0;
}

int main(void)
{
	int status = check_init_sets_metadata();
	if (status != 0) {
		return status;
	}
	/* Spot-check three classes: smallest, a mid class, largest. */
	status = check_full_capacity_reachable_for(0);
	if (status != 0) {
		return status;
	}
	status = check_full_capacity_reachable_for(3);
	if (status != 0) {
		return status;
	}
	status = check_full_capacity_reachable_for(7);
	if (status != 0) {
		return status;
	}
	return check_slot_reuse();
}
