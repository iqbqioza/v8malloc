/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Small slab pages — hold 80 B to 4 KiB objects (size classes 8..31).
 * Each slab is one 64 KiB page; metadata lives in the common
 * v8m_page_meta header at offset 0, and the data area begins at
 * V8M_SLAB_HEADER_SIZE (or higher, rounded up to the class's object
 * size so every returned pointer honours the class's alignment — the
 * 4 KiB class is the only one that needs this rounding today).
 *
 * Free slots are linked through the objects themselves: the first
 * `sizeof(void *)` bytes of every free slot hold the next-pointer of
 * an intrusive singly-linked list whose head is
 * meta->free_list_head. There is no bitmap — alloc pops from the
 * head, free pushes to the head; both are O(1) with no scan.
 *
 * Like the Tiny slab, the layout is logically single-threaded: the
 * owner thread (per meta->owner_thread) performs alloc and free;
 * cross-thread frees reach the slab via the MPSC queue in
 * v8m_remote_free.h and are applied from the owner's context. Only
 * used_count is atomic so remote observers can read page occupancy.
 *
 * See architecture.md §4 and size-classes.md §4.
 */

#ifndef V8M_SLAB_SMALL_H
#define V8M_SLAB_SMALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_page.h"

/*
 * Format a freshly-mapped 64 KiB page as a Small slab for
 * `size_class` (must be 8..31). `page_base` must be V8M_PAGE_SIZE-
 * aligned and writable. The data area begins at
 * v8m_slab_small_data_offset(size_class) — typically
 * V8M_SLAB_HEADER_SIZE, bumped up to the class's object size when
 * that is larger.
 */
void v8m_slab_small_init(void *page_base, uint32_t size_class,
			 uint64_t owner_thread);

/*
 * Allocate one object from the slab. Returns NULL when the slab is
 * full (free list empty).
 */
void *v8m_slab_small_alloc(struct v8m_page_meta *meta);

/*
 * Return one object to the slab it came from. `obj` must have been
 * issued by v8m_slab_small_alloc() from the same slab; the call
 * overwrites the first sizeof(void *) bytes of the object with the
 * free-list link (the caller has already relinquished the memory).
 * Returns true iff the slab has become empty, signalling that the
 * page may be returned to the NUMA pool / page heap.
 */
bool v8m_slab_small_free(struct v8m_page_meta *meta, void *obj);

bool v8m_slab_small_is_full(const struct v8m_page_meta *meta);
bool v8m_slab_small_is_empty(const struct v8m_page_meta *meta);

/*
 * Pure function of the class's byte size: how many objects fit in a
 * 64 KiB Small slab. Exposed so tests can pin the value against the
 * design doc's size-classes.md §4 table.
 */
uint32_t v8m_slab_small_capacity_for(uint16_t object_size);

/*
 * Offset at which the data area begins for a Small slab of the given
 * class. Equal to V8M_SLAB_HEADER_SIZE for every class except those
 * whose object_size exceeds it; those round up to object_size so the
 * first slot is naturally aligned.
 */
size_t v8m_slab_small_data_offset(uint16_t object_size);

#endif /* V8M_SLAB_SMALL_H */
