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
	/* Number of MADV_HUGEPAGE hints emitted to the kernel for
	 * allocations large enough (>= 2 MiB) to benefit from being
	 * backed by transparent huge pages. */
	uint64_t hugepage_advise_calls;
	/* Number of mmap(MAP_HUGETLB) attempts the page heap has made.
	 * Triggered for allocations that are 2 MiB-multiple AND
	 * 2 MiB-aligned with V8M_OPT_HUGE_PAGES != 0. */
	uint64_t hugetlb_alloc_calls;
	/* Subset of hugetlb_alloc_calls that returned MAP_FAILED —
	 * typically because the system has no reserved huge pages
	 * (`echo N > /proc/sys/vm/nr_hugepages`). The page heap
	 * silently falls back to ordinary mmap + MADV_HUGEPAGE in
	 * that case, so a non-zero failure count is informational,
	 * not an error. */
	uint64_t hugetlb_alloc_failures;
	/* Number of mbind(MPOL_BIND) calls the page heap has made
	 * to pin freshly-mapped regions to the calling thread's
	 * current NUMA node. Skipped (not counted) when NUMA is
	 * disabled by env var or the host has only one node. */
	uint64_t mbind_calls;
	/* Subset of mbind_calls that returned a non-zero status —
	 * typically ENOSYS / EPERM in seccomp-restricted runtimes
	 * or kernels without CONFIG_NUMA. The allocator carries on;
	 * a non-zero failure count just means subsequent fault-ins
	 * follow the kernel's default policy instead of the
	 * intended local-bind. */
	uint64_t mbind_failures;
	/* Number of mmap(MAP_HUGETLB | MAP_HUGE_1GB) Gigantic-page
	 * attempts. Triggered for allocations that are 1 GiB-multiple
	 * AND 1 GiB-aligned with V8M_OPT_HUGE_PAGES != 0. The most
	 * common reason for the count to stay zero is that no caller
	 * routes a request that large; the most common reason for
	 * `gigantic_alloc_failures` to track `gigantic_alloc_calls`
	 * 1:1 is the kernel having no reserved 1 GiB pages. */
	uint64_t gigantic_alloc_calls;
	uint64_t gigantic_alloc_failures;
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

/*
 * Number of regions currently in the region map. Useful to glibc-
 * compat reporters (mallinfo / mallinfo2) that need a "mmapped
 * regions" count — mmap_calls - munmap_calls cannot be used for
 * that because the over-allocate-and-trim strategy emits multiple
 * munmaps per mmap, leaving the difference net-negative.
 */
size_t v8m_page_heap_live_region_count(void);

#endif /* V8M_PAGE_HEAP_H */
