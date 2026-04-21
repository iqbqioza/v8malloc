/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Page heap — L4 of the 4-tier hierarchy. Owns the mmap-backed
 * virtual address regions that every higher layer draws from. See
 * architecture.md §2.4.
 *
 * This module provides the raw allocation / free / advise primitives
 * plus lifetime statistics, and a region map that records every live
 * (start, length) range so foreign-pointer detection on the free
 * path can decide ownership without reading at the pointer's
 * page-aligned base — that read faults for truly-foreign pointers
 * whose page base sits in an unmapped page.
 */

#ifndef V8M_PAGE_HEAP_H
#define V8M_PAGE_HEAP_H

#include <stdbool.h>
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

/*
 * Region map — every successful v8m_page_heap_alloc records the
 * returned (start, length) range in an internal table; v8m_page_heap_free
 * removes the matching entry. v8m_page_heap_owns is a safe predicate
 * that callers consult before reading at a pointer's page-aligned
 * base on the free path — without ownership confirmation, the
 * read can fault on truly-foreign pointers whose base sits in an
 * unmapped page.
 *
 * The lookup is a linear scan under a per-process mutex (≤ a few
 * thousand live regions in v0); a radix-tree replacement is the
 * follow-on optimization once region count grows.
 */
bool v8m_page_heap_owns(const void *ptr);

#endif /* V8M_PAGE_HEAP_H */
