/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Page heap implementation. A thin wrapper over mmap/munmap/madvise
 * that handles arbitrary power-of-two alignment by over-allocating
 * and trimming, and keeps lifetime statistics for diagnostic and
 * tuning purposes.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* fprintf for v8m_page_heap_validate diagnostics */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "v8m_anchor_reservation.h"
#include "v8m_arch.h" /* V8M_HUGE_PAGE_SIZE */
#include "v8m_config.h"
#include "v8m_internal.h"
#include "v8m_numa.h" /* v8m_numa_current_node, v8m_numa_node_count */
#include "v8m_page_heap.h"
#include "v8m_thp.h"
#include "v8malloc/v8malloc.h"

/* MPOL_BIND lives in <linux/mempolicy.h> which pulls in conflicting
 * kernel typedefs on glibc. Define just the constants we need here
 * so the syscall wrapper compiles cleanly. */
#ifndef V8M_MPOL_BIND
#define V8M_MPOL_BIND 2
#endif

static _Atomic uint64_t v8m_mmap_calls = 0;
static _Atomic uint64_t v8m_munmap_calls = 0;
static _Atomic uint64_t v8m_advise_calls = 0;
static _Atomic uint64_t v8m_bytes_mapped = 0;
static _Atomic uint64_t v8m_bytes_unmapped = 0;
static _Atomic uint64_t v8m_hugepage_advise_calls = 0;
static _Atomic uint64_t v8m_hugetlb_alloc_calls = 0;
static _Atomic uint64_t v8m_hugetlb_alloc_failures = 0;
static _Atomic uint64_t v8m_mbind_calls = 0;
static _Atomic uint64_t v8m_mbind_failures = 0;
static _Atomic uint64_t v8m_gigantic_alloc_calls = 0;
static _Atomic uint64_t v8m_gigantic_alloc_failures = 0;
/*
 * Adaptive THP advice (huge-pages.md §5). The decision logic lives
 * in v8m_thp.{h,c} now; the page heap calls into it on each THP-
 * eligible allocation and consults the threshold on the per-region
 * age sweep. The per-region age tracker (`promoted_at_tsc` on each
 * region_entry) stays here because it belongs to the region map.
 */

/*
 * Process-wide anchor reservation. Lazy-initialized on first
 * THP-eligible allocation that would otherwise mmap discretely.
 * When the carve fits inside the anchor's remaining capacity we
 * substitute the carve for the discrete mmap, saving one VMA per
 * allocation. The anchor lives inside one PROT_NONE region — the
 * kernel sees one VMA for the entire reservation regardless of how
 * many sub-carves we issue.
 */
static struct v8m_anchor_reservation g_anchor;
static atomic_bool g_anchor_initialized;
/* pthread.h supplies pthread_once_t; the IWYU rule prefers the deeper
 * bits/pthreadtypes.h which is an internal glibc header. */
static pthread_once_t g_anchor_once = /* NOLINT(misc-include-cleaner) */
    PTHREAD_ONCE_INIT;
static _Atomic uint64_t v8m_anchor_carve_calls = 0;
static _Atomic uint64_t v8m_anchor_carve_failures = 0;

static void anchor_lazy_init(void)
{
	if (v8m_anchor_reservation_init(&g_anchor, 0U) == 0) {
		atomic_store_explicit(&g_anchor_initialized, true,
				      memory_order_release);
	}
}

static struct v8m_anchor_reservation *anchor_get(void)
{
	(void)pthread_once(&g_anchor_once, anchor_lazy_init);
	if (!atomic_load_explicit(&g_anchor_initialized,
				  memory_order_acquire)) {
		return NULL;
	}
	return &g_anchor;
}

void v8m_page_heap_anchor_destroy_for_test(void)
{
	if (atomic_load_explicit(&g_anchor_initialized, memory_order_acquire)) {
		v8m_anchor_reservation_destroy(&g_anchor);
		atomic_store_explicit(&g_anchor_initialized, false,
				      memory_order_release);
	}
}

/*
 * Allocations at or above the kernel huge-page size are candidates
 * for the MADV_HUGEPAGE hint and for MAP_HUGETLB. The constant is
 * arch-defined in v8m_arch.h: 2 MiB on every Tier 1/2 arch we
 * support today, 1 MiB on s390x. The kernel can back the entire
 * mapping with a single huge page when memory is available; on
 * s390x that's a 1 MiB page, on x86_64/aarch64/ppc64le/riscv64
 * it's 2 MiB. MAP_HUGETLB additionally requires size to be a
 * multiple of, and alignment ≥, the kernel huge-page size.
 */

/*
 * 1 GiB Gigantic-page size (huge-pages.md §7). Triggered for
 * allocations that are 1 GiB-multiple AND 1 GiB-aligned with
 * V8M_OPT_HUGE_PAGES on. Requires kernel huge pages of the 1 GiB
 * size to be reserved (`echo N > /proc/sys/vm/nr_hugepages_1G`,
 * which itself usually needs `hugepagesz=1G default_hugepagesz=1G`
 * on the kernel command line). Failure is the common case in
 * containers and CI; we fall through to the 2 MiB MAP_HUGETLB
 * attempt and ultimately the regular mmap + MADV_HUGEPAGE path.
 */
#define V8M_GIGANTIC_BYTES ((size_t)1024 * 1024 * 1024)
/*
 * The Linux UAPI exposes the Gigantic-page page-shift to mmap via
 * an explicit MAP_HUGE_1GB flag (= 30 << MAP_HUGE_SHIFT). Define
 * the constants here to avoid pulling in <linux/mman.h> which
 * collides with glibc's <sys/mman.h>.
 */
#ifndef V8M_MAP_HUGE_SHIFT
#define V8M_MAP_HUGE_SHIFT 26
#endif
#ifndef V8M_MAP_HUGE_1GB
#define V8M_MAP_HUGE_1GB (30 << V8M_MAP_HUGE_SHIFT)
#endif

/*
 * Region map. Bounded array of (start, end) tuples kept **sorted
 * ascending by `start`**. Lookup uses binary search (O(log N));
 * insert and unregister memmove the array tail to preserve the
 * sort. Since regions never overlap (each comes from a distinct
 * mmap / carve), the "largest start ≤ addr" search yields the
 * unique candidate whose range may contain `addr`.
 *
 * The cap is sized for v0. A full radix-tree replacement would
 * further drop lookup to O(1) at the cost of a 2-level table; the
 * binary-search design hits the practical wins (hot-path ownership
 * checks no longer scan 4096 entries on every foreign pointer)
 * without the memory footprint.
 */
#define V8M_REGION_MAP_CAPACITY 4096

struct region_entry {
	uintptr_t start;
	uintptr_t end; /* exclusive */
	/* NUMA node the region was bound to via mbind(), or
	 * V8M_REGION_NODE_UNBOUND when no bind happened (NUMA off,
	 * single-node host, mbind failed, or the alloc path skipped
	 * bind_to_local_node entirely). The node field drives the
	 * per-node bytes accounting that surfaces through
	 * v8m_get_numa_balance — without it, the free path would
	 * have no way to decrement the right counter. */
	uint16_t node;
	/* True when the region was carved from the global anchor
	 * reservation rather than mmap'd discretely. Free routes
	 * through `v8m_anchor_reservation_release` instead of munmap;
	 * the virtual slot stays inside the anchor's PROT_NONE VMA
	 * and is not reclaimable (bump-only). */
	bool is_anchor;
	/* Per-region THP age tracker (huge-pages.md §5.1 "recently
	 * accessed" condition). Set to `v8m_arch_rdtsc()` when the
	 * alloc-time THP advice was PROMOTE; cleared to 0 when the
	 * bg sweep observes the region has aged past
	 * `g_thp_cold_threshold_ticks` and applies MADV_NOHUGEPAGE.
	 * 0 means either "never promoted" or "already aged out", in
	 * both cases the sweep skips the region. Per-region tracking
	 * lets a workload with one hot huge-eligible region and many
	 * cold ones get the right per-region advice — the global
	 * EMA decision could only choose one or the other for every
	 * subsequent alloc. */
	_Atomic uint64_t promoted_at_tsc;
};

#define V8M_REGION_NODE_UNBOUND UINT16_MAX

static struct region_entry g_regions[V8M_REGION_MAP_CAPACITY];
static size_t g_region_count;
/* pthread.h is the conventional provider for pthread_mutex_t;
 * clang-tidy's IWYU rule prefers the deeper bits/pthreadtypes.h
 * which is an internal glibc header. */
static pthread_mutex_t g_region_lock = /* NOLINT(misc-include-cleaner) */
    PTHREAD_MUTEX_INITIALIZER;

/* Seqlock counter for lock-free reads of the region map. Writers
 * (register / unregister) bump this twice per modification — once
 * before (odd = "writing in progress") and once after (even =
 * "stable"). Lock-free readers in `v8m_page_heap_owns_fast` do the
 * standard seqlock dance: snapshot seq, read region table, snapshot
 * seq again, retry if either snapshot was odd or they differ. The
 * mutex is still held by writers as the serialiser; the seqlock
 * just lets the malloc/free fast path skip the mutex on the
 * common-case ownership check. */
static _Atomic uint64_t g_region_seq;

/*
 * Per-node live bytes (numa.md §6.1). Bumped when a region's bind
 * lands on node N; decremented when the region is freed. Atomic to
 * support concurrent allocators without taking the region lock —
 * the lock is already held while updating the region table, but
 * the read side of v8m_get_numa_balance walks these counters
 * without the lock. The result is statistically accurate but may
 * lag a single increment behind concurrent writers.
 */
static _Atomic uint64_t v8m_per_node_bytes[V8M_NUMA_MAX_NODES];

/*
 * Per-node "suppressed" flag (numa.md §6.2 first action). Set true
 * by `v8m_page_heap_numa_rebalance` when the bg purge thread sees
 * the node holding ≥ 150 % of the average across live nodes; new
 * allocations on a suppressed node fall back to the nearest
 * neighbour via `v8m_numa_fallback_node`. The flag clears when the
 * imbalance subsides on a later tick. Atomic so the bg-thread
 * writer and the alloc-path reader race-free.
 */
static _Atomic bool v8m_per_node_suppressed[V8M_NUMA_MAX_NODES];
/* Counter of how many times the rebalance hook diverted an alloc
 * away from the calling thread's overloaded node. Diagnostic. */
static _Atomic uint64_t v8m_numa_rebalance_diversions;
/* Counter of move_pages() syscalls the rebalance action issued to
 * relocate already-resident pages off an overloaded node. */
static _Atomic uint64_t v8m_numa_migration_calls;

/* Lock-step the public ABI's per-node array width to the internal
 * NUMA cap so the snapshot helper never reads past either bound. */
_Static_assert(V8M_NUMA_MAX_NODES == V8M_PUBLIC_NUMA_MAX_NODES,
	       "public NUMA node cap must match internal cap");

/*
 * Find the lowest index `i` in g_regions[0..g_region_count) such
 * that g_regions[i].start >= start. Returns g_region_count when
 * every existing entry sorts strictly below `start`. Caller must
 * hold g_region_lock.
 */
static size_t region_lower_bound(uintptr_t start)
{
	size_t low = 0;
	size_t high = g_region_count;
	while (low < high) {
		size_t mid = low + ((high - low) >> 1U);
		if (g_regions[mid].start < start) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}
	return low;
}

/*
 * Find the index of the region with exactly `start` as its start
 * address, or SIZE_MAX if no such region exists. O(log N) via
 * binary search. Caller must hold g_region_lock.
 */
static size_t region_find_by_start(uintptr_t start)
{
	size_t idx = region_lower_bound(start);
	if (idx < g_region_count && g_regions[idx].start == start) {
		return idx;
	}
	return SIZE_MAX;
}

static int region_register_with_flags(void *ptr, size_t bytes, bool is_anchor)
{
	(void)pthread_mutex_lock(&g_region_lock);
	if (g_region_count >= V8M_REGION_MAP_CAPACITY) {
		(void)pthread_mutex_unlock(&g_region_lock);
		return -1;
	}
	/* Seqlock: bump to odd before mutating, even after. Lock-free
	 * readers retry while seq is odd. */
	atomic_fetch_add_explicit(&g_region_seq, 1U, memory_order_release);
	uintptr_t start = (uintptr_t)ptr;
	/* Insert sorted: find the position where the new entry's start
	 * fits, shift the tail right by one, place the entry. The
	 * memmove cost is O(N - idx) — bounded by the cap and amortized
	 * by the binary-search lookup that the sort enables. */
	size_t idx = region_lower_bound(start);
	if (idx < g_region_count) {
		(void)memmove(&g_regions[idx + 1], &g_regions[idx],
			      (g_region_count - idx) * sizeof(g_regions[0]));
	}
	g_regions[idx].start = start;
	g_regions[idx].end = start + bytes;
	g_regions[idx].node = V8M_REGION_NODE_UNBOUND;
	g_regions[idx].is_anchor = is_anchor;
	atomic_store_explicit(&g_regions[idx].promoted_at_tsc, 0U,
			      memory_order_relaxed);
	g_region_count++;
	atomic_fetch_add_explicit(&g_region_seq, 1U, memory_order_release);
	(void)pthread_mutex_unlock(&g_region_lock);
	return 0;
}

/*
 * Stamp the region rooted at `ptr` as PROMOTED at `tsc`. Called
 * from the alloc path right after the THP-advice decision returns
 * V8M_THP_PROMOTE; the bg sweep later compares this against the
 * cold threshold to decide whether the region has aged out and
 * should be demoted to MADV_NOHUGEPAGE.
 */
static void region_record_promote(const void *ptr, uint64_t tsc)
{
	uintptr_t start = (uintptr_t)ptr;
	(void)pthread_mutex_lock(&g_region_lock);
	size_t idx = region_find_by_start(start);
	if (idx != SIZE_MAX) {
		atomic_store_explicit(&g_regions[idx].promoted_at_tsc, tsc,
				      memory_order_relaxed);
	}
	(void)pthread_mutex_unlock(&g_region_lock);
}

static int region_register(void *ptr, size_t bytes)
{
	return region_register_with_flags(ptr, bytes, false);
}

/*
 * Record `node` against the region rooted at `ptr`. Called from the
 * alloc path right after a successful mbind so the per-node counter
 * decrement at free time can find the right bucket. Silent no-op
 * when the region is no longer registered (race with a concurrent
 * free — extremely rare but defensive).
 */
static void region_record_node(const void *ptr, uint16_t node)
{
	uintptr_t start = (uintptr_t)ptr;
	(void)pthread_mutex_lock(&g_region_lock);
	size_t idx = region_find_by_start(start);
	if (idx != SIZE_MAX) {
		g_regions[idx].node = node;
	}
	(void)pthread_mutex_unlock(&g_region_lock);
}

/*
 * Returns the node previously recorded for the unregistered region,
 * or V8M_REGION_NODE_UNBOUND if the region was never bound or is
 * not present.
 */
static uint16_t region_unregister_with_flags(const void *ptr,
					     bool *out_is_anchor)
{
	uintptr_t start = (uintptr_t)ptr;
	uint16_t node = V8M_REGION_NODE_UNBOUND;
	*out_is_anchor = false;
	(void)pthread_mutex_lock(&g_region_lock);
	size_t idx = region_find_by_start(start);
	if (idx != SIZE_MAX) {
		atomic_fetch_add_explicit(&g_region_seq, 1U,
					  memory_order_release);
		node = g_regions[idx].node;
		*out_is_anchor = g_regions[idx].is_anchor;
		/* Memmove-compact to preserve the sort. The cost is
		 * O(g_region_count - idx) bytes, bounded by the cap
		 * and amortized by the binary-search ownership check
		 * the sort enables. */
		size_t tail = g_region_count - idx - 1U;
		if (tail > 0) {
			(void)memmove(&g_regions[idx], &g_regions[idx + 1],
				      tail * sizeof(g_regions[0]));
		}
		g_region_count--;
		atomic_fetch_add_explicit(&g_region_seq, 1U,
					  memory_order_release);
	}
	(void)pthread_mutex_unlock(&g_region_lock);
	return node;
}

/* Lock-free owns: seqlock-based read of the region map. Returns
 * true / false on a stable snapshot, or `unknown` (-1) when the
 * snapshot raced with a writer (caller should fall back to the
 * mutex-protected slow path). The seqlock pattern (snapshot seq,
 * read, snapshot seq again, retry-or-bail) keeps the malloc/free
 * fast path off the region mutex on the typical case where reads
 * vastly outnumber writes. */
/* Per-thread cache for v8m_page_heap_owns_fast. The malloc/free hot
 * path tends to hit the same slab page repeatedly (especially in
 * oscillating alloc/free patterns); caching the page-base + the
 * last-result + the seq number it was valid at lets the second-and-
 * later queries on the same page skip the binary search entirely.
 * The seq match guarantees the cached verdict is still authoritative
 * — if any writer has run since, the cached entry is invalidated and
 * we fall through to the full check. */
static __thread uintptr_t v8m_owns_cache_page_base;
static __thread uint64_t v8m_owns_cache_seq;
static __thread int v8m_owns_cache_result;

int v8m_page_heap_owns_fast(const void *ptr)
{
	if (ptr == NULL) {
		return 0;
	}
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t page_base = addr & V8M_PAGE_MASK;
	/* Per-thread fast-fast-path cache: same page as last query AND
	 * no writer since. Common in oscillating alloc/free workloads
	 * (the bin keeps cycling slots through the same slab page). */
	if (page_base == v8m_owns_cache_page_base) {
		uint64_t seq_now =
		    atomic_load_explicit(&g_region_seq, memory_order_acquire);
		if (seq_now == v8m_owns_cache_seq && (seq_now & 1U) == 0U) {
			return v8m_owns_cache_result;
		}
	}
	for (int retry = 0; retry < 4; retry++) {
		/* seq1 acquire pairs with the writer's seq release on
		 * its second bump (transition odd -> even). */
		uint64_t seq1 =
		    atomic_load_explicit(&g_region_seq, memory_order_acquire);
		if ((seq1 & 1U) != 0U) {
			continue; /* writer in progress */
		}
		/* The array reads below must happen between the two seq
		 * snapshots. The acquire fence pairs with the writer's
		 * release-store on the seq counter so the compiler /
		 * CPU cannot reorder the array reads past either
		 * snapshot. */
		size_t count = g_region_count;
		bool owned = false;
		size_t low = 0;
		size_t high = count;
		while (low < high) {
			size_t mid = low + ((high - low) >> 1U);
			if (g_regions[mid].start <= addr) {
				low = mid + 1;
			} else {
				high = mid;
			}
		}
		if (low > 0 && low <= count) {
			const struct region_entry *region = &g_regions[low - 1];
			owned = addr >= region->start && addr < region->end;
		}
		atomic_thread_fence(memory_order_acquire);
		uint64_t seq2 =
		    atomic_load_explicit(&g_region_seq, memory_order_relaxed);
		if (seq1 == seq2) {
			int result = owned ? 1 : 0;
			/* Cache the result for the next query on the same
			 * page. seq carries the validity stamp. */
			v8m_owns_cache_page_base = page_base;
			v8m_owns_cache_seq = seq2;
			v8m_owns_cache_result = result;
			return result;
		}
	}
	return -1; /* race-bound — caller falls back to slow owns */
}

bool v8m_page_heap_owns(const void *ptr)
{
	if (ptr == NULL) {
		return false;
	}
	uintptr_t addr = (uintptr_t)ptr;
	(void)pthread_mutex_lock(&g_region_lock);
	/* Binary search for the largest region whose start ≤ addr.
	 * Regions don't overlap (each is a distinct mmap / carve), so
	 * the unique candidate whose range may contain `addr` is that
	 * one; a single end-check closes the verdict. O(log N) vs the
	 * prior O(N) linear scan. */
	bool owned = false;
	size_t low = 0;
	size_t high = g_region_count;
	while (low < high) {
		size_t mid = low + ((high - low) >> 1U);
		if (g_regions[mid].start <= addr) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}
	if (low > 0) {
		const struct region_entry *region = &g_regions[low - 1];
		owned = addr < region->end;
	}
	(void)pthread_mutex_unlock(&g_region_lock);
	return owned;
}

size_t v8m_page_heap_live_region_count(void)
{
	(void)pthread_mutex_lock(&g_region_lock);
	size_t count = g_region_count;
	(void)pthread_mutex_unlock(&g_region_lock);
	return count;
}

int v8m_page_heap_validate(void)
{
	int issues = 0;
	(void)pthread_mutex_lock(&g_region_lock);
	for (size_t i = 1; i < g_region_count; i++) {
		const struct region_entry *prev = &g_regions[i - 1U];
		const struct region_entry *cur = &g_regions[i];
		if (cur->start < prev->start) {
			(void)fprintf(
			    stderr,
			    "v8m_validate: region map unsorted at "
			    "index %zu (prev.start=%lx cur.start=%lx)\n",
			    i, (unsigned long)prev->start,
			    (unsigned long)cur->start);
			issues++;
		}
		if (cur->start < prev->end) {
			(void)fprintf(
			    stderr,
			    "v8m_validate: regions overlap at "
			    "index %zu (prev.end=%lx cur.start=%lx)\n",
			    i, (unsigned long)prev->end,
			    (unsigned long)cur->start);
			issues++;
		}
		if (cur->start >= cur->end) {
			(void)fprintf(stderr,
				      "v8m_validate: region %zu has empty "
				      "range (start=%lx end=%lx)\n",
				      i, (unsigned long)cur->start,
				      (unsigned long)cur->end);
			issues++;
		}
	}
	(void)pthread_mutex_unlock(&g_region_lock);
	return issues;
}

/* pthread_atfork plumbing for the page heap. Locks every page-heap-
 * owned mutex (the region map + the lazy anchor reservation when
 * initialised) before fork so the child does not inherit a
 * locked-by-dead-thread mutex. The dispatcher's prefork calls this
 * after locking its own pools so the global acquire order is
 * dispatch-pools → page-heap → anchor. */
void v8m_page_heap_prefork(void)
{
	(void)pthread_mutex_lock(&g_region_lock);
	if (atomic_load_explicit(&g_anchor_initialized, memory_order_acquire)) {
		(void)pthread_mutex_lock(&g_anchor.lock);
	}
}

void v8m_page_heap_postfork_parent(void)
{
	if (atomic_load_explicit(&g_anchor_initialized, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&g_anchor.lock);
	}
	(void)pthread_mutex_unlock(&g_region_lock);
}

void v8m_page_heap_postfork_child(void)
{
	if (atomic_load_explicit(&g_anchor_initialized, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&g_anchor.lock);
	}
	(void)pthread_mutex_unlock(&g_region_lock);
}

static bool is_power_of_two(size_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

/*
 * OS page size, cached on first use. POSIX guarantees
 * sysconf(_SC_PAGESIZE) is non-negative and constant for the
 * process lifetime, so a one-shot atomic cache is sufficient.
 *
 * This matters for the over-allocate-and-trim path below: mmap
 * always returns an OS-page-aligned address, so when the caller's
 * requested alignment fits within one OS page the trim is pure
 * overhead. On x86_64 the OS page is 4 KiB and our typical request
 * is 64 KiB-aligned, so the optimization stays dormant. On
 * ppc64le with the 64 KiB kernel page (Debian / Ubuntu / RHEL
 * default — platform-abstraction.md §5.4) and on aarch64 with
 * 16 KiB or 64 KiB kernel pages (Asahi / certain server kernels)
 * the optimization fires for V8M_PAGE_SIZE-aligned requests,
 * eliminating one mmap-region's worth of VMA churn and one or two
 * trim munmap calls per allocation.
 */
static size_t v8m_os_page_size(void)
{
	static _Atomic size_t cached = 0;
	size_t value = atomic_load_explicit(&cached, memory_order_relaxed);
	if (value != 0) {
		return value;
	}
	long ret = sysconf(_SC_PAGESIZE);
	value = (ret > 0) ? (size_t)ret : 4096U;
	atomic_store_explicit(&cached, value, memory_order_relaxed);
	return value;
}

static void record_mmap(size_t bytes)
{
	atomic_fetch_add_explicit(&v8m_mmap_calls, 1U, memory_order_relaxed);
	atomic_fetch_add_explicit(&v8m_bytes_mapped, bytes,
				  memory_order_relaxed);
}

static void record_munmap(size_t bytes)
{
	atomic_fetch_add_explicit(&v8m_munmap_calls, 1U, memory_order_relaxed);
	atomic_fetch_add_explicit(&v8m_bytes_unmapped, bytes,
				  memory_order_relaxed);
}

/*
 * Pin a freshly-mapped region to the calling thread's current
 * NUMA node via mbind(MPOL_BIND). Best-effort: failures are
 * counted but do not change the allocator's behaviour — the
 * region remains usable, just with the kernel's default
 * (typically MPOL_DEFAULT first-touch) policy. Skipped when
 * NUMA is disabled by env var, the host is single-node (no
 * benefit), or the kernel filters / does not support the
 * syscall (every WSL2 / no-NUMA / seccomp-restricted runtime).
 *
 * The bind is applied immediately after mmap, before any page
 * fault has materialised a physical frame, so the policy
 * controls the *new* faults (no MPOL_MF_MOVE needed). Subsequent
 * fault-ins land on the local node, which is the entire point
 * of the pin.
 */
/*
 * Reserve `bytes` bytes aligned to `alignment`. When the requested
 * alignment fits within one OS page mmap's natural alignment is
 * sufficient and we issue a single direct mmap; otherwise we
 * over-allocate by `alignment` and trim the unaligned head and tail.
 * Returns NULL on overflow or mmap failure.
 */
static void *reserve_aligned(size_t bytes, size_t alignment)
{
	if (alignment <= v8m_os_page_size()) {
		void *direct = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (direct == MAP_FAILED) {
			return NULL;
		}
		record_mmap(bytes);
		return direct;
	}

	if (bytes > SIZE_MAX - alignment) {
		return NULL;
	}
	size_t request = bytes + alignment;
	void *raw = mmap(NULL, request, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) {
		return NULL;
	}
	record_mmap(request);

	uintptr_t raw_addr = (uintptr_t)raw;
	uintptr_t aligned =
	    (raw_addr + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
	size_t pre = (size_t)(aligned - raw_addr);
	size_t post = request - pre - bytes;

	if (pre > 0) {
		(void)munmap(raw, pre);
		record_munmap(pre);
	}
	if (post > 0) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		(void)munmap((void *)(aligned + bytes), post);
		record_munmap(post);
	}
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	return (void *)aligned;
}

static int bind_to_local_node(void *addr, size_t bytes)
{
	if (v8m_config_get(V8M_OPT_NUMA_AWARE) == 0) {
		return -1;
	}
	uint32_t node_count = v8m_numa_node_count();
	if (node_count <= 1U) {
		return -1;
	}
	uint32_t node = v8m_numa_current_node();
	if (node >= node_count) {
		return -1;
	}
	/* NUMA imbalance rebalance (numa.md §6.2 first action). When
	 * the bg purge thread has flagged this node as suppressed —
	 * because it holds ≥ 150 % of the average across live nodes —
	 * route this allocation to the nearest non-suppressed
	 * neighbour instead. The diversion counter surfaces in the
	 * rebalance stats so a maintainer can confirm the path
	 * fired. */
	if (atomic_load_explicit(&v8m_per_node_suppressed[node],
				 memory_order_relaxed)) {
		for (uint32_t rank = 0; rank < node_count; rank++) {
			uint32_t alt = v8m_numa_fallback_node(node, rank);
			if (alt >= node_count || alt == node) {
				continue;
			}
			if (!atomic_load_explicit(&v8m_per_node_suppressed[alt],
						  memory_order_relaxed)) {
				node = alt;
				atomic_fetch_add_explicit(
				    &v8m_numa_rebalance_diversions, 1U,
				    memory_order_relaxed);
				break;
			}
		}
	}

	/* Build a unsigned-long bitmap with just our node's bit set.
	 * The kernel reads `maxnode + 1` bits of the mask; setting
	 * `maxnode = node + 1` keeps the bitmap a single word for
	 * any reasonable NUMA topology (current cap is 64 nodes). */
	enum { BITS_PER_LONG = 64 };
	unsigned long mask[2] = {0, 0};
	if (node >= BITS_PER_LONG) {
		mask[1] = 1UL << (node - BITS_PER_LONG);
	} else {
		mask[0] = 1UL << node;
	}
	unsigned long maxnode = (unsigned long)node + 1UL;

	atomic_fetch_add_explicit(&v8m_mbind_calls, 1U, memory_order_relaxed);
	long ret = syscall(SYS_mbind, addr, (unsigned long)bytes,
			   (long)V8M_MPOL_BIND, mask, maxnode, 0U);
	if (ret != 0) {
		atomic_fetch_add_explicit(&v8m_mbind_failures, 1U,
					  memory_order_relaxed);
		return -1;
	}
	return (int)node;
}

/*
 * Combine the bind syscall with the per-node accounting and the
 * region-table node update. Centralizes the "after a successful
 * bind, the per-node bytes counter and the region's node field
 * must agree" invariant in one place so the four call sites in
 * v8m_page_heap_alloc don't have to repeat the pattern.
 */
static void bind_and_account(void *addr, size_t bytes)
{
	int chosen = bind_to_local_node(addr, bytes);
	if (chosen < 0) {
		return;
	}
	region_record_node(addr, (uint16_t)chosen);
	atomic_fetch_add_explicit(&v8m_per_node_bytes[chosen], (uint64_t)bytes,
				  memory_order_relaxed);
}

/*
 * Anchor-or-mmap: VMA-minimization fallback for THP-eligible
 * allocations. Tries the global anchor first when the request is at
 * or above V8M_HUGE_PAGE_SIZE — every successful carve saves one
 * VMA over the per-allocation discrete mmap path. Falls back to
 * `reserve_aligned` (over-allocate-and-trim mmap) when the anchor
 * is too small, the carve doesn't fit, or the lazy init failed.
 * `*used_anchor` is set true iff the result came from the carve
 * path so the caller can route the matching free correctly.
 */
static void *anchor_or_mmap(size_t bytes, size_t alignment, bool *used_anchor)
{
	*used_anchor = false;
	if (bytes >= V8M_HUGE_PAGE_SIZE) {
		struct v8m_anchor_reservation *anchor = anchor_get();
		if (anchor != NULL) {
			void *carved = v8m_anchor_reservation_carve(
			    anchor, bytes, alignment);
			if (carved != NULL) {
				*used_anchor = true;
				atomic_fetch_add_explicit(
				    &v8m_anchor_carve_calls, 1U,
				    memory_order_relaxed);
				return carved;
			}
			atomic_fetch_add_explicit(&v8m_anchor_carve_failures,
						  1U, memory_order_relaxed);
		}
	}
	return reserve_aligned(bytes, alignment);
}

/*
 * Try MAP_HUGETLB | MAP_HUGE_1GB for Gigantic-class requests.
 * Returns the mapped pointer on success or NULL when the request
 * isn't shaped for the path or the syscall failed (caller falls
 * through to the next-larger huge-page attempt).
 */
static void *try_gigantic(size_t bytes, size_t alignment)
{
	if (bytes < V8M_GIGANTIC_BYTES ||
	    (bytes & (V8M_GIGANTIC_BYTES - 1U)) != 0U ||
	    alignment < V8M_GIGANTIC_BYTES ||
	    v8m_config_get(V8M_OPT_HUGE_PAGES) == 0) {
		return NULL;
	}
	atomic_fetch_add_explicit(&v8m_gigantic_alloc_calls, 1U,
				  memory_order_relaxed);
	void *gigantic =
	    mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | V8M_MAP_HUGE_1GB,
		 -1, 0);
	if (gigantic != MAP_FAILED) {
		return gigantic;
	}
	atomic_fetch_add_explicit(&v8m_gigantic_alloc_failures, 1U,
				  memory_order_relaxed);
	return NULL;
}

/*
 * Try MAP_HUGETLB for 2 MiB-class requests. Returns the mapped
 * pointer on success or NULL on failure / unsupported shape.
 */
static void *try_hugetlb(size_t bytes, size_t alignment)
{
	if (bytes < V8M_HUGE_PAGE_SIZE ||
	    (bytes & (V8M_HUGE_PAGE_SIZE - 1U)) != 0U ||
	    alignment < V8M_HUGE_PAGE_SIZE ||
	    v8m_config_get(V8M_OPT_HUGE_PAGES) == 0) {
		return NULL;
	}
	atomic_fetch_add_explicit(&v8m_hugetlb_alloc_calls, 1U,
				  memory_order_relaxed);
	void *huge = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (huge != MAP_FAILED) {
		return huge;
	}
	atomic_fetch_add_explicit(&v8m_hugetlb_alloc_failures, 1U,
				  memory_order_relaxed);
	return NULL;
}

/* Common register-and-bind tail for the kernel-direct mmap paths
 * (Gigantic + HUGETLB). On region-table-full undoes the mmap so
 * the caller never sees an unclassifiable pointer. */
static void *register_and_bind_mmap(void *ptr, size_t bytes)
{
	record_mmap(bytes);
	if (region_register(ptr, bytes) != 0) {
		(void)munmap(ptr, bytes);
		record_munmap(bytes);
		return NULL;
	}
	bind_and_account(ptr, bytes);
	return ptr;
}

void *v8m_page_heap_alloc(size_t bytes, size_t alignment)
{
	if (bytes == 0 || alignment < V8M_PAGE_SIZE ||
	    !is_power_of_two(alignment)) {
		return NULL;
	}

	void *gigantic = try_gigantic(bytes, alignment);
	if (gigantic != NULL) {
		return register_and_bind_mmap(gigantic, bytes);
	}
	void *huge = try_hugetlb(bytes, alignment);
	if (huge != NULL) {
		return register_and_bind_mmap(huge, bytes);
	}

	bool used_anchor = false;
	void *result = anchor_or_mmap(bytes, alignment, &used_anchor);
	if (result == NULL) {
		return NULL;
	}
	if (region_register_with_flags(result, bytes, used_anchor) != 0) {
		/* Region table is full — undo the alloc so the caller
		 * never sees a pointer the foreign-detection path
		 * can't classify. The cap is generous (4096 live
		 * regions) and crossing it points at either a leak or a
		 * workload that needs the radix-tree replacement. */
		if (used_anchor) {
			(void)v8m_anchor_reservation_release(anchor_get(),
							     result, bytes);
		} else {
			(void)munmap(result, bytes);
			record_munmap(bytes);
		}
		return NULL;
	}
	/* Hint the kernel toward transparent huge pages for
	 * Large/Huge-class regions. Honours V8M_OPT_HUGE_PAGES — set
	 * to 0 to suppress the hint (resolves TODO open question #7
	 * in favour of opt-out via the env-var contract). The advise
	 * is best-effort: failure here doesn't change the allocator's
	 * behaviour, so we don't even check the return value. The
	 * threshold is the kernel huge-page size (1 MiB on s390x,
	 * 2 MiB elsewhere) — anything below that has no THP path. */
	if (bytes >= V8M_HUGE_PAGE_SIZE &&
	    v8m_config_get(V8M_OPT_HUGE_PAGES) != 0) {
		/* Adaptive THP advice (huge-pages.md §5): when the
		 * inter-allocation EMA is hot/warm, retain the existing
		 * MADV_HUGEPAGE behaviour; when it crosses the cold
		 * threshold, demote to MADV_NOHUGEPAGE so the kernel
		 * does not waste effort promoting a region the program
		 * is unlikely to actively touch. */
		enum v8m_thp_advice advice = v8m_thp_decide_and_record();
		if (advice == V8M_THP_DEMOTE) {
			(void)madvise(result, bytes, MADV_NOHUGEPAGE);
			v8m_thp_record_demote();
		} else {
			(void)madvise(result, bytes, MADV_HUGEPAGE);
			atomic_fetch_add_explicit(&v8m_hugepage_advise_calls,
						  1U, memory_order_relaxed);
			v8m_thp_record_promote();
			/* Stamp the region's promote timestamp so the bg
			 * sweep can age it out and demote when it goes
			 * cold. Anchor-carved regions can't be re-advised
			 * later (they share one VMA), so skip the stamp
			 * for those. */
			if (!used_anchor) {
				region_record_promote(result, v8m_arch_rdtsc());
			}
		}
	}
	bind_and_account(result, bytes);
	return result;
}

void v8m_page_heap_free(void *ptr, size_t bytes)
{
	if (ptr == NULL || bytes == 0) {
		return;
	}
	bool is_anchor = false;
	uint16_t node = region_unregister_with_flags(ptr, &is_anchor);
	if (node != V8M_REGION_NODE_UNBOUND && node < V8M_NUMA_MAX_NODES) {
		atomic_fetch_sub_explicit(&v8m_per_node_bytes[node],
					  (uint64_t)bytes,
					  memory_order_relaxed);
	}
	if (is_anchor) {
		/* Anchor carves stay inside the anchor's PROT_NONE VMA;
		 * release drops the physical pages and re-protects the
		 * slot so use-after-free traps. The slot is not
		 * reclaimable (bump-only by design). */
		(void)v8m_anchor_reservation_release(anchor_get(), ptr, bytes);
	} else {
		(void)munmap(ptr, bytes);
		record_munmap(bytes);
	}
}

void v8m_page_heap_advise_dont_need(void *ptr, size_t bytes)
{
	if (ptr == NULL || bytes == 0) {
		return;
	}
	(void)madvise(ptr, bytes, MADV_DONTNEED);
	atomic_fetch_add_explicit(&v8m_advise_calls, 1U, memory_order_relaxed);
}

void v8m_page_heap_get_stats(struct v8m_page_heap_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->mmap_calls =
	    atomic_load_explicit(&v8m_mmap_calls, memory_order_relaxed);
	out->munmap_calls =
	    atomic_load_explicit(&v8m_munmap_calls, memory_order_relaxed);
	out->advise_calls =
	    atomic_load_explicit(&v8m_advise_calls, memory_order_relaxed);
	out->bytes_mapped =
	    atomic_load_explicit(&v8m_bytes_mapped, memory_order_relaxed);
	out->bytes_unmapped =
	    atomic_load_explicit(&v8m_bytes_unmapped, memory_order_relaxed);
	out->hugepage_advise_calls = atomic_load_explicit(
	    &v8m_hugepage_advise_calls, memory_order_relaxed);
	out->hugetlb_alloc_calls = atomic_load_explicit(
	    &v8m_hugetlb_alloc_calls, memory_order_relaxed);
	out->hugetlb_alloc_failures = atomic_load_explicit(
	    &v8m_hugetlb_alloc_failures, memory_order_relaxed);
	out->mbind_calls =
	    atomic_load_explicit(&v8m_mbind_calls, memory_order_relaxed);
	out->mbind_failures =
	    atomic_load_explicit(&v8m_mbind_failures, memory_order_relaxed);
	out->gigantic_alloc_calls = atomic_load_explicit(
	    &v8m_gigantic_alloc_calls, memory_order_relaxed);
	out->gigantic_alloc_failures = atomic_load_explicit(
	    &v8m_gigantic_alloc_failures, memory_order_relaxed);
	out->thp_promote_calls = v8m_thp_promote_calls();
	out->thp_demote_calls = v8m_thp_demote_calls();
	out->thp_ema_ticks = v8m_thp_ema_ticks();
	/* Use the snapshot variant so a stats read does NOT silently
	 * lazy-init the threshold — preserves the original behaviour
	 * where 0 means "no THP decision has been made yet". */
	out->thp_cold_threshold_ticks = v8m_thp_cold_threshold_ticks_snapshot();
	out->anchor_carve_calls =
	    atomic_load_explicit(&v8m_anchor_carve_calls, memory_order_relaxed);
	out->anchor_carve_failures = atomic_load_explicit(
	    &v8m_anchor_carve_failures, memory_order_relaxed);
}

size_t v8m_page_heap_thp_age_sweep(void)
{
	uint64_t threshold = v8m_thp_cold_threshold_ticks();
	uint64_t now = v8m_arch_rdtsc();
	size_t demoted = 0;
	(void)pthread_mutex_lock(&g_region_lock);
	for (size_t i = 0; i < g_region_count; i++) {
		struct region_entry *region = &g_regions[i];
		uint64_t promoted_at = atomic_load_explicit(
		    &region->promoted_at_tsc, memory_order_relaxed);
		if (promoted_at == 0U || now <= promoted_at ||
		    now - promoted_at < threshold) {
			continue;
		}
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *ptr = (void *)region->start;
		size_t bytes = region->end - region->start;
		atomic_store_explicit(&region->promoted_at_tsc, 0U,
				      memory_order_relaxed);
		(void)pthread_mutex_unlock(&g_region_lock);
		(void)madvise(ptr, bytes, MADV_NOHUGEPAGE);
		v8m_thp_record_age_demote();
		demoted++;
		(void)pthread_mutex_lock(&g_region_lock);
		/* Region table may have shrunk under us during the
		 * mprotect/madvise window; recheck the index. The
		 * worst case is we revisit the same slot if a free
		 * happened to swap-remove into it — harmless because
		 * the new tenant's promoted_at_tsc is either 0 or
		 * very recent. */
	}
	(void)pthread_mutex_unlock(&g_region_lock);
	return demoted;
}

uint64_t v8m_page_heap_thp_age_demote_calls(void)
{
	return v8m_thp_age_demote_calls();
}

/*
 * Migrate one region's already-resident pages off `from_node` to
 * `to_node` via the move_pages() syscall. Bounded scope per call
 * (V8M_MIGRATE_MAX_PAGES = 256 OS pages → at most 1 MiB on x86_64);
 * larger regions get partial migration this tick and the rest next
 * tick. Best-effort: the syscall return is ignored — failure means
 * the kernel could not migrate (target node out of memory, page
 * pinned, etc.) and the action retries next tick.
 *
 * Returns true iff a region was found and a syscall issued, so the
 * caller can bound work per tick.
 */
#define V8M_MIGRATE_MAX_PAGES 256U

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static bool migrate_one_region(uint16_t from_node, int to_node)
{
	long os_page_signed = sysconf(_SC_PAGESIZE);
	size_t os_page = (os_page_signed > 0) ? (size_t)os_page_signed : 4096U;

	uintptr_t region_base = 0;
	size_t region_bytes = 0;
	(void)pthread_mutex_lock(&g_region_lock);
	for (size_t i = 0; i < g_region_count; i++) {
		if (g_regions[i].node == from_node) {
			region_base = g_regions[i].start;
			region_bytes = g_regions[i].end - g_regions[i].start;
			/* Re-stamp node so the next tick picks a
			 * different region. The actual residency
			 * after move_pages may not match, but the
			 * accounting follows the intent — a future
			 * snapshot with the kernel's mempolicy will
			 * correct any drift. */
			g_regions[i].node = (uint16_t)to_node;
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_region_lock);

	if (region_base == 0U || region_bytes == 0U) {
		return false;
	}

	size_t total_pages = region_bytes / os_page;
	if (total_pages > V8M_MIGRATE_MAX_PAGES) {
		total_pages = V8M_MIGRATE_MAX_PAGES;
	}

	void *pages[V8M_MIGRATE_MAX_PAGES];
	int nodes[V8M_MIGRATE_MAX_PAGES];
	for (size_t i = 0; i < total_pages; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		pages[i] = (void *)(region_base + (i * os_page));
		nodes[i] = to_node;
	}

	(void)syscall(SYS_move_pages, 0, total_pages, pages, nodes, NULL, 0);
	atomic_fetch_add_explicit(&v8m_numa_migration_calls, 1U,
				  memory_order_relaxed);
	return true;
}

size_t v8m_page_heap_numa_rebalance(void)
{
	struct v8m_numa_balance_stats snap = {0};
	v8m_page_heap_get_numa_balance(&snap);
	if (snap.node_count <= 1U) {
		return 0;
	}
	size_t flipped = 0;
	uint32_t cap = snap.node_count > V8M_NUMA_MAX_NODES ? V8M_NUMA_MAX_NODES
							    : snap.node_count;
	for (uint32_t node = 0; node < cap; node++) {
		bool over = snap.imbalanced && node == snap.most_loaded_node;
		bool was = atomic_exchange_explicit(
		    &v8m_per_node_suppressed[node], over, memory_order_relaxed);
		if (was != over) {
			flipped++;
		}
	}
	/* Active migration: when the imbalance trigger fired and a
	 * fallback target exists, push one region per tick from the
	 * most-loaded node toward the nearest neighbour via
	 * move_pages(). Bounded to one region per tick so a deeply
	 * imbalanced run does not stall the bg purge thread on a
	 * single tick's worth of migration syscalls. The diversion
	 * action (already wired in `bind_to_local_node`) prevents new
	 * allocs from landing on the suppressed node, so the
	 * combination of (a) no new allocs on the node and (b) one
	 * migration per tick drains the imbalance over a few ticks. */
	if (snap.imbalanced) {
		uint32_t fallback =
		    v8m_numa_fallback_node(snap.most_loaded_node, 1);
		if (fallback < V8M_NUMA_MAX_NODES &&
		    fallback != snap.most_loaded_node) {
			(void)migrate_one_region(
			    (uint16_t)snap.most_loaded_node, (int)fallback);
		}
	}
	return flipped;
}

uint64_t v8m_page_heap_numa_rebalance_diversions(void)
{
	return atomic_load_explicit(&v8m_numa_rebalance_diversions,
				    memory_order_relaxed);
}

uint64_t v8m_page_heap_numa_migration_calls(void)
{
	return atomic_load_explicit(&v8m_numa_migration_calls,
				    memory_order_relaxed);
}

bool v8m_page_heap_node_is_suppressed(uint32_t node)
{
	if (node >= V8M_NUMA_MAX_NODES) {
		return false;
	}
	return atomic_load_explicit(&v8m_per_node_suppressed[node],
				    memory_order_relaxed);
}

void v8m_page_heap_get_numa_balance(struct v8m_numa_balance_stats *out)
{
	if (out == NULL) {
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	uint32_t node_count = v8m_numa_node_count();
	if (node_count > V8M_PUBLIC_NUMA_MAX_NODES) {
		node_count = V8M_PUBLIC_NUMA_MAX_NODES;
	}
	out->node_count = node_count;
	uint64_t total = 0;
	uint64_t most = 0;
	uint32_t most_node = 0;
	for (uint32_t node = 0; node < node_count; node++) {
		uint64_t bytes = atomic_load_explicit(&v8m_per_node_bytes[node],
						      memory_order_relaxed);
		out->per_node_bytes[node] = bytes;
		total += bytes;
		if (bytes > most) {
			most = bytes;
			most_node = node;
		}
	}
	out->total_bytes = total;
	out->most_loaded_node = most_node;
	out->most_loaded_bytes = most;
	if (node_count > 0U) {
		out->average_bytes_per_node = total / node_count;
	}
	/* Imbalance threshold (numa.md §6.1): a node holding ≥ 150 % of
	 * the average is overloaded. The 1.5× factor is encoded as
	 * `most * 2 > average * 3` to avoid floating point on the
	 * snapshot path. The "needs at least 2 nodes" guard keeps a
	 * single-node host from ever flipping the flag (every byte
	 * lands on node 0, so most == average always). */
	if (node_count >= 2U &&
	    out->most_loaded_bytes * 2U > out->average_bytes_per_node * 3U) {
		out->imbalanced = true;
	}
}
