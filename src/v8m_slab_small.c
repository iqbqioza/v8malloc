/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Small slab page implementation. Free slots form an intrusive
 * singly-linked list whose head lives in meta->free_list_head; the
 * first sizeof(void *) bytes of every free slot carry the next link.
 * Alloc pops from the head, free pushes to the head — both O(1)
 * with no scan, since a slab of Small class objects is always far
 * denser than a bitmap would warrant.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_debug.h"
#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_size_class.h"
#include "v8m_slab_small.h"

static_assert(V8M_SLAB_HEADER_SIZE >= sizeof(struct v8m_page_meta),
	      "V8M_SLAB_HEADER_SIZE too small for v8m_page_meta");

size_t v8m_slab_small_data_offset(uint16_t object_size)
{
	/* Bump the header reservation up to the object size when that
	 * is larger than V8M_SLAB_HEADER_SIZE, so every slot is
	 * naturally aligned. Today only class 31 (4 KiB) exercises
	 * this path. */
	return (size_t)object_size > V8M_SLAB_HEADER_SIZE
		   ? (size_t)object_size
		   : (size_t)V8M_SLAB_HEADER_SIZE;
}

uint32_t v8m_slab_small_capacity_for(uint16_t object_size)
{
	if (object_size < sizeof(void *)) {
		return 0;
	}
	size_t offset = v8m_slab_small_data_offset(object_size);
	if (offset >= V8M_PAGE_SIZE) {
		return 0;
	}
	size_t data_area = V8M_PAGE_SIZE - offset;
	return (uint32_t)(data_area / object_size);
}

static unsigned char *slab_data(struct v8m_page_meta *meta)
{
	return (unsigned char *)meta +
	       v8m_slab_small_data_offset(meta->object_size);
}

/* size_class (0..31) and owner_thread (pthread_self() cast) share an
 * integer type but have wildly different domains; a mix-up is loud
 * rather than subtle, so the bugprone-easily-swappable-parameters
 * warning is more noise than signal here. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void v8m_slab_small_init(void *page_base, uint32_t size_class,
			 uint64_t owner_thread)
{
	struct v8m_page_meta *meta = page_base;
	uint16_t object_size = (uint16_t)v8m_size_class_size(size_class);
	uint32_t capacity = v8m_slab_small_capacity_for(object_size);

	meta->magic = V8M_MAGIC;
	meta->size_class = (uint16_t)size_class;
	meta->object_size = object_size;
	meta->capacity = capacity;
	atomic_store_explicit(&meta->used_count, 0, memory_order_relaxed);
	meta->owner_thread = owner_thread;
	meta->next = NULL;

	/* Build the free list from the tail backwards so the head
	 * points at slot 0 — allocations then hand out memory in
	 * natural order, which is friendlier to the prefetcher. */
	unsigned char *data = slab_data(meta);
	void *head = NULL;
	for (uint32_t i = capacity; i > 0U; i--) {
		uint32_t slot = i - 1U;
		void *obj = data + ((size_t)slot * object_size);
		void **link = (void **)obj;
		*link = head;
		head = obj;
	}
	meta->free_list_head = head;

	/* DEBUG-mode UAF detector: poison every slot's body bytes
	 * (everything past the sizeof(void *) free-list link the loop
	 * above just wrote). On first alloc the verify step sees this
	 * poison and passes; subsequent free re-stamps the poison
	 * itself. The helper is a no-op when V8M_OPT_DEBUG is 0. */
	v8m_debug_uaf_poison_data_area(data, object_size, capacity,
				       sizeof(void *));
}

void *v8m_slab_small_alloc(struct v8m_page_meta *meta)
{
	void *head = meta->free_list_head;
	if (head == NULL) {
		return NULL;
	}
	void **link = (void **)head;
	meta->free_list_head = *link;
	atomic_fetch_add_explicit(&meta->used_count, 1U, memory_order_relaxed);
	/* Verify the body poison left by the prior free (or by the
	 * init-time pre-poison for the first alloc). The first
	 * sizeof(void *) bytes hold the next-link we just consumed
	 * and are not part of the verify window. No-op in release
	 * builds. */
	if (meta->object_size > sizeof(void *)) {
		v8m_debug_uaf_verify((unsigned char *)head + sizeof(void *),
				     (size_t)meta->object_size - sizeof(void *),
				     "small slab");
	}
	return head;
}

bool v8m_slab_small_free(struct v8m_page_meta *meta, void *obj)
{
	void **link = (void **)obj;
	*link = meta->free_list_head;
	meta->free_list_head = obj;
	/* Stamp the slot's body bytes with the UAF-poison pattern;
	 * the first sizeof(void *) bytes hold the next-link we just
	 * wrote and are excluded from the poison. No-op in release
	 * builds. */
	if (meta->object_size > sizeof(void *)) {
		v8m_debug_uaf_poison((unsigned char *)obj + sizeof(void *),
				     (size_t)meta->object_size -
					 sizeof(void *));
	}
	uint32_t previous = atomic_fetch_sub_explicit(&meta->used_count, 1U,
						      memory_order_relaxed);
	return previous == 1U;
}

bool v8m_slab_small_is_full(const struct v8m_page_meta *meta)
{
	uint32_t used =
	    atomic_load_explicit(&meta->used_count, memory_order_relaxed);
	return used == meta->capacity;
}

bool v8m_slab_small_is_empty(const struct v8m_page_meta *meta)
{
	return atomic_load_explicit(&meta->used_count, memory_order_relaxed) ==
	       0U;
}
