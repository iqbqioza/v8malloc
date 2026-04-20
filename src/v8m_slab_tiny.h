/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny slab pages — hold 8 B to 64 B objects (size classes 0..7).
 * Each slab is one 64 KiB page whose first V8M_TINY_HEADER_SIZE
 * bytes carry the v8m_tiny_page_meta header (magic + per-class
 * constants + bitmap + search hint); the rest is a densely packed
 * array of `object_size`-byte slots.
 *
 * Slot i is live iff bit i in meta->bitmap is set. At init time,
 * bits outside the page's capacity are pre-set so the allocation
 * fast path can scan without bounds-checking; the bitmap's trailing
 * headroom (128 × 64 = 8192 bits) is always ≥ capacity.
 *
 * The slab is logically single-threaded: the owning thread (per
 * meta->owner_thread) runs alloc/free; cross-thread frees reach the
 * slab through the MPSC queue in v8m_remote_free.h and are applied
 * later from the owner's context. Only used_count is atomic so
 * remote observers can read page occupancy without synchronizing
 * with the owner.
 *
 * See architecture.md §4 and thread-cache.md §6.
 */

#ifndef V8M_SLAB_TINY_H
#define V8M_SLAB_TINY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_page.h"

/*
 * Fixed header reservation. Sized so the allocator never has to
 * recompute where the data area starts on the hot path. Also kept
 * aligned to the maximum Tiny object size (64 B), so every slot's
 * starting address respects its natural alignment.
 */
#define V8M_TINY_HEADER_SIZE 2048

/*
 * Format a freshly-mapped 64 KiB page as a Tiny slab for
 * `size_class` (must be 0..7). `page_base` must be V8M_PAGE_SIZE-
 * aligned and writable.
 */
void v8m_slab_tiny_init(void *page_base, uint32_t size_class,
			uint64_t owner_thread);

/*
 * Allocate one object from the slab. Returns NULL when the slab is
 * full.
 */
void *v8m_slab_tiny_alloc(struct v8m_page_meta *meta);

/*
 * Return one object to the slab. `obj` must have been issued by
 * v8m_slab_tiny_alloc() from the same slab. Returns true if the
 * slab is now empty, so the caller may return the page to the
 * NUMA-pool / page-heap.
 */
bool v8m_slab_tiny_free(struct v8m_page_meta *meta, const void *obj);

bool v8m_slab_tiny_is_full(const struct v8m_page_meta *meta);
bool v8m_slab_tiny_is_empty(const struct v8m_page_meta *meta);

/*
 * Capacity that a Tiny slab of the given object size holds. Pure
 * function of the size-class byte size; used at init and exposed
 * here so tests can pin the value.
 */
uint32_t v8m_slab_tiny_capacity_for(uint16_t object_size);

#endif /* V8M_SLAB_TINY_H */
