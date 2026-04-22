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
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "v8m_config.h"
#include "v8m_internal.h"
#include "v8m_numa.h" /* v8m_numa_current_node, v8m_numa_node_count */
#include "v8m_page_heap.h"
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
 * Allocations at or above this size are candidates for the
 * MADV_HUGEPAGE hint. 2 MiB matches the standard transparent-
 * huge-page size on x86_64 and aarch64 — the kernel can back the
 * entire mapping with a single 2 MiB page when memory is available.
 */
#define V8M_HUGEPAGE_HINT_MIN_BYTES ((size_t)2 * 1024 * 1024)

/*
 * MAP_HUGETLB requires the size to be a multiple of the system
 * huge-page size (2 MiB on x86_64 / aarch64) and the kernel
 * returns a 2 MiB-aligned address. The same value drives both
 * size-multiple and alignment checks.
 */
#define V8M_HUGETLB_BYTES ((size_t)2 * 1024 * 1024)

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
 * Region map. Bounded array of (start, end) tuples kept in
 * arbitrary order. Linear scan on lookup; O(N) is acceptable while
 * N stays under ~a few thousand. The cap is sized for v0; the
 * radix-tree replacement comes when production workloads start
 * crossing it.
 */
#define V8M_REGION_MAP_CAPACITY 4096

struct region_entry {
	uintptr_t start;
	uintptr_t end; /* exclusive */
};

static struct region_entry g_regions[V8M_REGION_MAP_CAPACITY];
static size_t g_region_count;
/* pthread.h is the conventional provider for pthread_mutex_t;
 * clang-tidy's IWYU rule prefers the deeper bits/pthreadtypes.h
 * which is an internal glibc header. */
static pthread_mutex_t g_region_lock = /* NOLINT(misc-include-cleaner) */
    PTHREAD_MUTEX_INITIALIZER;

static int region_register(void *ptr, size_t bytes)
{
	(void)pthread_mutex_lock(&g_region_lock);
	if (g_region_count >= V8M_REGION_MAP_CAPACITY) {
		(void)pthread_mutex_unlock(&g_region_lock);
		return -1;
	}
	uintptr_t start = (uintptr_t)ptr;
	g_regions[g_region_count].start = start;
	g_regions[g_region_count].end = start + bytes;
	g_region_count++;
	(void)pthread_mutex_unlock(&g_region_lock);
	return 0;
}

static void region_unregister(const void *ptr)
{
	uintptr_t start = (uintptr_t)ptr;
	(void)pthread_mutex_lock(&g_region_lock);
	for (size_t i = 0; i < g_region_count; i++) {
		if (g_regions[i].start == start) {
			/* Swap-remove to keep the lookup scan
			 * compact. Order in the array does not matter. */
			g_regions[i] = g_regions[--g_region_count];
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_region_lock);
}

bool v8m_page_heap_owns(const void *ptr)
{
	if (ptr == NULL) {
		return false;
	}
	uintptr_t addr = (uintptr_t)ptr;
	(void)pthread_mutex_lock(&g_region_lock);
	bool owned = false;
	for (size_t i = 0; i < g_region_count; i++) {
		if (addr >= g_regions[i].start && addr < g_regions[i].end) {
			owned = true;
			break;
		}
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

static void bind_to_local_node(void *addr, size_t bytes)
{
	if (v8m_config_get(V8M_OPT_NUMA_AWARE) == 0) {
		return;
	}
	uint32_t node_count = v8m_numa_node_count();
	if (node_count <= 1U) {
		return;
	}
	uint32_t node = v8m_numa_current_node();
	if (node >= node_count) {
		return;
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
	}
}

void *v8m_page_heap_alloc(size_t bytes, size_t alignment)
{
	if (bytes == 0 || alignment < V8M_PAGE_SIZE ||
	    !is_power_of_two(alignment)) {
		return NULL;
	}

	/* Try MAP_HUGETLB | MAP_HUGE_1GB first for Gigantic-class
	 * requests (size + alignment ≥ 1 GiB, both 1 GiB-multiple,
	 * V8M_OPT_HUGE_PAGES on). On a kernel with reserved 1 GiB
	 * huge pages this is the densest possible mapping; on every
	 * other host (the common case — CI, dev containers, most
	 * production systems without a `hugepagesz=1G` boot
	 * argument) the syscall fails and we fall through to the
	 * 2 MiB MAP_HUGETLB attempt below, then ultimately to the
	 * regular mmap + MADV_HUGEPAGE path (huge-pages.md §7). */
	if (bytes >= V8M_GIGANTIC_BYTES &&
	    (bytes & (V8M_GIGANTIC_BYTES - 1U)) == 0U &&
	    alignment >= V8M_GIGANTIC_BYTES &&
	    v8m_config_get(V8M_OPT_HUGE_PAGES) != 0) {
		atomic_fetch_add_explicit(&v8m_gigantic_alloc_calls, 1U,
					  memory_order_relaxed);
		void *gigantic = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS |
					  MAP_HUGETLB | V8M_MAP_HUGE_1GB,
				      -1, 0);
		if (gigantic != MAP_FAILED) {
			record_mmap(bytes);
			if (region_register(gigantic, bytes) != 0) {
				(void)munmap(gigantic, bytes);
				record_munmap(bytes);
				return NULL;
			}
			bind_to_local_node(gigantic, bytes);
			return gigantic;
		}
		atomic_fetch_add_explicit(&v8m_gigantic_alloc_failures, 1U,
					  memory_order_relaxed);
		/* Fall through to the 2 MiB MAP_HUGETLB attempt. */
	}

	/* Try MAP_HUGETLB first when the request is shaped for it:
	 * size is a 2 MiB multiple, alignment is at least 2 MiB, and
	 * V8M_OPT_HUGE_PAGES allows it. The kernel returns a 2 MiB-
	 * aligned address so no over-allocate-and-trim is needed; on
	 * failure (no reserved huge pages — the typical case in
	 * containers and CI) we fall through to the regular mmap +
	 * MADV_HUGEPAGE path, which is documented as the supported
	 * fallback for this code path (huge-pages.md §4.1). */
	if (bytes >= V8M_HUGETLB_BYTES &&
	    (bytes & (V8M_HUGETLB_BYTES - 1U)) == 0U &&
	    alignment >= V8M_HUGETLB_BYTES &&
	    v8m_config_get(V8M_OPT_HUGE_PAGES) != 0) {
		atomic_fetch_add_explicit(&v8m_hugetlb_alloc_calls, 1U,
					  memory_order_relaxed);
		void *huge =
		    mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (huge != MAP_FAILED) {
			record_mmap(bytes);
			if (region_register(huge, bytes) != 0) {
				(void)munmap(huge, bytes);
				record_munmap(bytes);
				return NULL;
			}
			bind_to_local_node(huge, bytes);
			return huge;
		}
		atomic_fetch_add_explicit(&v8m_hugetlb_alloc_failures, 1U,
					  memory_order_relaxed);
		/* Fall through to the regular path. */
	}

	/* Regular path. reserve_aligned picks between a single direct
	 * mmap (when alignment fits within one OS page — common on
	 * ppc64le 64 KiB kernels and aarch64 16/64 KiB kernels) and the
	 * over-allocate-and-trim fallback (the only option on x86_64
	 * where the 4 KiB OS page is smaller than every
	 * V8M_PAGE_SIZE-shaped request). */
	void *result = reserve_aligned(bytes, alignment);
	if (result == NULL) {
		return NULL;
	}
	if (region_register(result, bytes) != 0) {
		/* Region table is full — undo the mmap so the caller
		 * never sees a pointer the foreign-detection path
		 * can't classify. The cap is generous (4096 live
		 * regions) and crossing it points at either a leak or a
		 * workload that needs the radix-tree replacement. */
		(void)munmap(result, bytes);
		record_munmap(bytes);
		return NULL;
	}
	/* Hint the kernel toward 2 MiB transparent huge pages for
	 * Large/Huge-class regions. Honours V8M_OPT_HUGE_PAGES — set
	 * to 0 to suppress the hint (resolves TODO open question #7
	 * in favour of opt-out via the env-var contract). The advise
	 * is best-effort: failure here doesn't change the allocator's
	 * behaviour, so we don't even check the return value. */
	if (bytes >= V8M_HUGEPAGE_HINT_MIN_BYTES &&
	    v8m_config_get(V8M_OPT_HUGE_PAGES) != 0) {
		(void)madvise(result, bytes, MADV_HUGEPAGE);
		atomic_fetch_add_explicit(&v8m_hugepage_advise_calls, 1U,
					  memory_order_relaxed);
	}
	bind_to_local_node(result, bytes);
	return result;
}

void v8m_page_heap_free(void *ptr, size_t bytes)
{
	if (ptr == NULL || bytes == 0) {
		return;
	}
	region_unregister(ptr);
	(void)munmap(ptr, bytes);
	record_munmap(bytes);
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
}
