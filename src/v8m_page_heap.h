/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Page heap — L4 of the 4-tier hierarchy. Owns the mmap-backed
 * virtual address regions that every higher layer draws from. See
 * architecture.md §2.4.
 *
 * This module provides the raw allocation / free / advise primitives
 * plus lifetime statistics. Region tracking for foreign-pointer
 * detection (the v8m_region_map referenced in architecture.md) is
 * open question #1 in TODO.md and is deferred to a dedicated cycle;
 * until then, foreign-pointer detection relies on bootstrap
 * range-check + page-metadata magic check.
 */

#ifndef V8M_PAGE_HEAP_H
#define V8M_PAGE_HEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Reserve a virtual address range of `bytes` whose start is aligned
 * to `alignment`. Returns NULL on failure (ENOMEM, invalid arguments)
 * and a valid pointer otherwise.
 *
 * Constraints:
 *   - `bytes` > 0
 *   - `alignment` is a power of two, >= V8M_PAGE_SIZE (64 KiB)
 *   - `bytes + alignment` must not overflow size_t
 */
void *v8m_page_heap_alloc(size_t bytes, size_t alignment);

/*
 * Release a region previously returned by v8m_page_heap_alloc(). The
 * `bytes` argument must match the allocation-time request; the page
 * heap does not track sizes itself (callers at higher layers track
 * them in per-page / per-extent metadata).
 */
void v8m_page_heap_free(void *ptr, size_t bytes);

/*
 * Advise the kernel that the pages in [ptr, ptr + bytes) are no
 * longer needed. On Linux this is madvise(MADV_DONTNEED) — physical
 * pages are returned to the OS, but the virtual mapping is
 * preserved and subsequent reads zero-fill.
 */
void v8m_page_heap_advise_dont_need(void *ptr, size_t bytes);

/*
 * Lifetime statistics for the page heap. Counters are monotonically
 * increasing and never reset.
 */
struct v8m_page_heap_stats {
	uint64_t mmap_calls;
	uint64_t munmap_calls;
	uint64_t advise_calls;
	uint64_t bytes_mapped;
	uint64_t bytes_unmapped;
};

/*
 * Snapshot the page heap's counters into `*out`. Individual field
 * updates are atomic, but the snapshot itself is not atomic across
 * fields — concurrent allocators may leave the snapshot internally
 * inconsistent by a small amount.
 */
void v8m_page_heap_get_stats(struct v8m_page_heap_stats *out);

#endif /* V8M_PAGE_HEAP_H */
