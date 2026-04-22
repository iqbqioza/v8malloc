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
	/* Adaptive THP advice (huge-pages.md §5). `thp_promote_calls`
	 * counts MADV_HUGEPAGE hints chosen by the density-driven
	 * decision (warm/hot path); `thp_demote_calls` counts the
	 * MADV_NOHUGEPAGE hints emitted for cold workloads where the
	 * EMA of inter-arrival ticks crossed the threshold. The two
	 * counters together partition every THP-eligible alloc that
	 * fired the decision (allocations under V8M_HUGE_PAGE_SIZE or
	 * with V8M_OPT_HUGE_PAGES off skip both branches). The
	 * `thp_ema_ticks` and `thp_cold_threshold_ticks` snapshots
	 * surface the live decision state for diagnostic comparison
	 * with the threshold. */
	uint64_t thp_promote_calls;
	uint64_t thp_demote_calls;
	uint64_t thp_ema_ticks;
	uint64_t thp_cold_threshold_ticks;
	/* Anchor reservation usage. `anchor_carve_calls` counts
	 * THP-eligible allocations the page heap satisfied from the
	 * global anchor's PROT_NONE region instead of a discrete mmap;
	 * `anchor_carve_failures` counts attempts that fell back to
	 * discrete mmap (anchor full, alignment did not fit, anchor
	 * lazy-init failed). The two together partition the
	 * THP-eligible alloc count above the kernel-page threshold. */
	uint64_t anchor_carve_calls;
	uint64_t anchor_carve_failures;
};

/*
 * Snapshot the page heap's counters into `*out`. Individual field
 * updates are atomic, but the snapshot itself is not atomic across
 * fields — concurrent allocators may leave the snapshot internally
 * inconsistent by a small amount.
 */
void v8m_page_heap_get_stats(struct v8m_page_heap_stats *out);

/*
 * Snapshot per-node bytes-mapped + computed imbalance flag (numa.md
 * §6.1). `out` must be non-NULL; pre-init / single-node hosts return
 * a zeroed struct with `node_count` set to the host's node count.
 * Defined in v8m_page_heap.c so the page-heap-internal per-node
 * counters do not need a separate accessor module. The matching
 * public API surface is `v8m_get_numa_balance` in v8m_api.c.
 */
struct v8m_numa_balance_stats;
void v8m_page_heap_get_numa_balance(struct v8m_numa_balance_stats *out);

/*
 * NUMA rebalance action (numa.md §6.2). Re-evaluates the per-node
 * balance and toggles each node's "suppressed" flag — the
 * most-loaded node is marked suppressed iff `imbalanced` is true on
 * the snapshot, every other node clears. While suppressed, new
 * allocations on a thread whose current node is the suppressed one
 * are diverted to the nearest non-suppressed neighbour via
 * `v8m_numa_fallback_node`. Returns the count of flag transitions
 * (suppressed ↔ not) — useful for the bg-purge tick to log when
 * the action fires. Called periodically by the bg purge thread;
 * tests can call it directly to drive the action.
 */
size_t v8m_page_heap_numa_rebalance(void);

/*
 * Diversion counter — total allocations the rebalance action
 * routed away from the calling thread's overloaded node. Monotonic.
 */
uint64_t v8m_page_heap_numa_rebalance_diversions(void);

/*
 * Migration counter — total move_pages() calls the rebalance
 * action issued to relocate already-resident pages off an
 * overloaded node. Monotonic. A failed syscall still counts toward
 * the call count (the migration is best-effort).
 */
uint64_t v8m_page_heap_numa_migration_calls(void);

/*
 * Read the per-node suppressed flag the rebalance action sets.
 * Out-of-range nodes return false. Read by the thread cache's GC
 * tick to shrink bin capacity for threads whose current node is
 * suppressed (numa.md §6.2 "TLC capacity shrink for threads pinned
 * to the overloaded node"). Lock-free; the value may lag a single
 * tick behind a concurrent rebalance call.
 */
bool v8m_page_heap_node_is_suppressed(uint32_t node);

/*
 * Per-region THP age sweep (huge-pages.md §5.1 cold detection).
 * Walks every registered region and demotes (MADV_NOHUGEPAGE) any
 * region whose alloc-time PROMOTE stamp has aged past
 * `g_thp_cold_threshold_ticks`. Returns the count of regions
 * demoted on this pass — useful for the bg-purge tick to log when
 * the action fires. Called periodically by the bg purge thread;
 * tests can call it directly to drive the sweep.
 */
size_t v8m_page_heap_thp_age_sweep(void);

/*
 * Total regions the age sweep has demoted since process start.
 * Diagnostic; distinct from the alloc-time demote counter
 * (`thp_demote_calls`) which counts the global EMA's decision to
 * demote at allocation time.
 */
uint64_t v8m_page_heap_thp_age_demote_calls(void);

/*
 * Test-only: tear down the global anchor reservation. Tests that
 * want a clean baseline call this between phases; production
 * callers should never touch it (the anchor lives for the process
 * lifetime). NOT exported via `v8malloc.map`.
 */
void v8m_page_heap_anchor_destroy_for_test(void);

/*
 * Test-only knob for the adaptive THP advice (huge-pages.md §5).
 * Overrides the cold-threshold ticks and the live EMA so a test can
 * deterministically exercise the promote / demote branches without
 * depending on wall-clock timing. NOT exported from the library; the
 * symbol is internal-only — `v8malloc.map` does not list it. Pass
 * any non-zero `cold_threshold_ticks` to lock the threshold (the
 * lazy initializer's early-return treats non-zero as "already
 * computed"); zero behaves as a reset that re-enables the lazy
 * derivation on the next decision.
 */
void v8m_page_heap_thp_test_inject(uint64_t cold_threshold_ticks,
				   uint64_t ema_ticks);

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
