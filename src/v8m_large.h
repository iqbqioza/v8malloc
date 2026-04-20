/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Direct-mmap path for Large (256 KiB – 2 MiB, size classes 38..40)
 * and Huge (> 2 MiB) allocations. Every allocation gets its own
 * page-heap-backed region; the common v8m_page_meta header sits at
 * offset 0 and the user's pointer is returned at offset
 * V8M_SLAB_HEADER_SIZE so free()'s ptr-to-meta mask recovers the
 * header correctly. The region's full mmap size is stored in an
 * extended page-meta variant so free() can pass it back to
 * v8m_page_heap_free().
 *
 * Two optimizations the design doc calls for are intentionally
 * deferred:
 *  - Red-Black tree extent management for the Large range
 *    (size-classes.md §6) — gives best-fit reuse and coalescing of
 *    freed extents.
 *  - MAP_HUGETLB for the Huge range (huge-pages.md §4.1) — lets
 *    the kernel back the allocation with one or more 2 MiB
 *    HugePages, slashing TLB pressure.
 * Both land in dedicated cycles; this module establishes the
 * always-correct baseline they will optimize.
 */

#ifndef V8M_LARGE_H
#define V8M_LARGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_internal.h"
#include "v8m_page.h"

/*
 * Page-metadata extension for Large/Huge allocations. The leading
 * fields match v8m_page_meta exactly (static_asserted in v8m_large.c)
 * so generic reverse-lookup code can access magic / size_class /
 * used_count without a tag check.
 */
struct v8m_large_page_meta {
	uint64_t magic;
	uint16_t size_class;
	uint16_t object_size; /* unused for Large/Huge; always 0 */
	uint32_t capacity;    /* always 1 */
	atomic_uint used_count;
	uint64_t owner_thread;
	void *free_list_head;	    /* unused; kept NULL for layout parity */
	struct v8m_page_meta *next; /* unused; kept NULL */
	/* Large/Huge-specific. */
	size_t mmap_size;
};

/*
 * Allocate a region for a single object of `size` bytes (> 0).
 * Returns a pointer to the user-visible area whose first byte is at
 * offset V8M_SLAB_HEADER_SIZE inside the mmap region, and which has
 * at least `size` bytes of accessible storage.
 *
 * Returns NULL on allocation failure or on arithmetic overflow of
 * the region size.
 */
void *v8m_large_alloc(size_t size, uint64_t owner_thread);

/*
 * Free a region previously returned by v8m_large_alloc(). Tolerates
 * NULL. Caller has already verified `obj` is a v8malloc pointer via
 * v8m_page_meta_valid(). `obj` itself is only read through by the
 * implementation (the writes are to the separately-recovered page
 * header), so the signature takes a pointer to const.
 */
void v8m_large_free(const void *obj);

/*
 * Number of bytes accessible through the returned pointer. Equal to
 * mmap_size - V8M_SLAB_HEADER_SIZE; always >= the original request.
 */
size_t v8m_large_usable_size(const void *obj);

#endif /* V8M_LARGE_H */
