/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Large/Huge direct-mmap allocation path. One v8m_page_heap_alloc()
 * per request; the region's V8M_SLAB_HEADER_SIZE-aligned header
 * carries the page-metadata plus the mmap size so free() can unmap
 * the exact bytes allocated.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_internal.h"
#include "v8m_large.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"

/* Common-prefix offsets must match v8m_page_meta exactly, so generic
 * reverse-lookup code (v8m_ptr_to_meta + magic check) works against
 * Large/Huge pages without a tag check. */
static_assert(offsetof(struct v8m_large_page_meta, magic) ==
		  offsetof(struct v8m_page_meta, magic),
	      "Large meta: magic offset diverges from common layout");
static_assert(offsetof(struct v8m_large_page_meta, size_class) ==
		  offsetof(struct v8m_page_meta, size_class),
	      "Large meta: size_class offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, object_size) ==
		  offsetof(struct v8m_page_meta, object_size),
	      "Large meta: object_size offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, capacity) ==
		  offsetof(struct v8m_page_meta, capacity),
	      "Large meta: capacity offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, used_count) ==
		  offsetof(struct v8m_page_meta, used_count),
	      "Large meta: used_count offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, owner_thread) ==
		  offsetof(struct v8m_page_meta, owner_thread),
	      "Large meta: owner_thread offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, free_list_head) ==
		  offsetof(struct v8m_page_meta, free_list_head),
	      "Large meta: free_list_head offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, next) ==
		  offsetof(struct v8m_page_meta, next),
	      "Large meta: next offset diverges");

static_assert(sizeof(struct v8m_large_page_meta) <= V8M_SLAB_HEADER_SIZE,
	      "Large meta exceeds V8M_SLAB_HEADER_SIZE");

/*
 * Sentinel stored in size_class for Huge allocations (> 2 MiB), so a
 * walker that wants to distinguish Huge from Large can check against
 * V8M_LARGE_HUGE_TAG. Large allocations (classes 38..40) keep their
 * actual class value in the field.
 */
#define V8M_LARGE_HUGE_TAG UINT16_MAX

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_large_alloc(size_t size, uint64_t owner_thread)
{
	if (size == 0) {
		return NULL;
	}
	/* Overflow guard: need room for the header + size, then round
	 * up to a page multiple. */
	if (size > SIZE_MAX - V8M_SLAB_HEADER_SIZE - V8M_PAGE_SIZE) {
		return NULL;
	}

	size_t total = V8M_SLAB_HEADER_SIZE + size;
	size_t mmap_size =
	    (total + V8M_PAGE_SIZE - 1U) & ~(size_t)(V8M_PAGE_SIZE - 1U);

	void *region = v8m_page_heap_alloc(mmap_size, V8M_PAGE_SIZE);
	if (region == NULL) {
		return NULL;
	}

	struct v8m_large_page_meta *meta = region;
	uint32_t cls = v8m_size_class(size);
	uint16_t size_class_tag = (cls == V8M_CLASS_HUGE)
				      ? (uint16_t)V8M_LARGE_HUGE_TAG
				      : (uint16_t)cls;

	meta->magic = V8M_MAGIC;
	meta->size_class = size_class_tag;
	meta->object_size = 0;
	meta->capacity = 1;
	atomic_store_explicit(&meta->used_count, 1U, memory_order_relaxed);
	meta->owner_thread = owner_thread;
	meta->free_list_head = NULL;
	meta->next = NULL;
	meta->mmap_size = mmap_size;

	return (unsigned char *)region + V8M_SLAB_HEADER_SIZE;
}

void v8m_large_free(const void *obj)
{
	if (obj == NULL) {
		return;
	}
	/* Recover the page base via the shared ptr-to-meta helper so the
	 * int-to-ptr cast stays behind v8m_ptr_to_meta and this module
	 * doesn't need its own NOLINT. ptr_to_meta returns a non-const
	 * pointer, so writing to the header fields below is well-formed
	 * even though the caller's object pointer is const-qualified. */
	struct v8m_page_meta *common = v8m_ptr_to_meta(obj);
	struct v8m_large_page_meta *meta = (struct v8m_large_page_meta *)common;
	size_t mmap_size = meta->mmap_size;

	atomic_store_explicit(&meta->used_count, 0U, memory_order_relaxed);
	meta->magic = 0U;

	v8m_page_heap_free(common, mmap_size);
}

size_t v8m_large_usable_size(const void *obj)
{
	if (obj == NULL) {
		return 0;
	}
	const struct v8m_page_meta *common = v8m_ptr_to_meta(obj);
	const struct v8m_large_page_meta *meta =
	    (const struct v8m_large_page_meta *)common;
	return meta->mmap_size - V8M_SLAB_HEADER_SIZE;
}
