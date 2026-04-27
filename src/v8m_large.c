/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Large/Huge direct-mmap allocation path. One v8m_page_heap_alloc()
 * per request; the region's V8M_SLAB_HEADER_SIZE-aligned header
 * carries the page-metadata plus the mmap size so free() can unmap
 * the exact bytes allocated.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* fprintf for the red-zone abort diagnostic */
#include <stdlib.h> /* abort */
#include <string.h> /* memset, memchr */
#include <sys/mman.h> /* mprotect for V8M_OPT_DEBUG guard pages */

#include "v8m_arch.h" /* V8M_HUGE_PAGE_SIZE */
#include "v8m_config.h"
#include "v8m_internal.h"
#include "v8m_large.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"
#include "v8malloc/v8malloc.h"

/*
 * Red zone canary byte. Filled into the tail padding between the
 * caller's requested size and the usable_size boundary at alloc
 * time, verified at free time. A mismatched byte is the signature
 * of a small overflow that did not reach the trailing guard page.
 * 0xCD matches the convention from MSVC's debug heap so anyone
 * familiar with Windows debug allocators recognises the pattern.
 */
#define V8M_LARGE_REDZONE_BYTE 0xCDU

/*
 * Maximum red-zone span, in bytes. Capping the canary work bounds
 * the cost of DEBUG mode for the largest allocations — a Huge
 * request whose mmap_size rounds up to 2 MiB-multiples would
 * otherwise trigger ~2 MiB of memset + memcmp on every free. 64
 * bytes (one cache line on every supported arch) catches the
 * overwhelming majority of small overflows; anything larger
 * either falls inside the cap or eventually crosses into the
 * trailing guard page, which traps synchronously regardless.
 */
#define V8M_LARGE_REDZONE_MAX_BYTES 64U

/* Common-prefix offsets must match v8m_page_meta exactly, so generic
 * reverse-lookup code (v8m_ptr_to_meta + magic check) works against
 * Large/Huge pages without a tag check. */
static_assert(offsetof(struct v8m_large_page_meta, magic) ==
		  offsetof(struct v8m_page_meta, magic),
	      "Large meta: magic offset diverges from common layout");
static_assert(offsetof(struct v8m_large_page_meta, size_class) ==
		  offsetof(struct v8m_page_meta, size_class),
	      "Large meta: size_class offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, object_size) ==
		  offsetof(struct v8m_page_meta, object_size),
	      "Large meta: object_size offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, capacity) ==
		  offsetof(struct v8m_page_meta, capacity),
	      "Large meta: capacity offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, used_count) ==
		  offsetof(struct v8m_page_meta, used_count),
	      "Large meta: used_count offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, owner_thread) ==
		  offsetof(struct v8m_page_meta, owner_thread),
	      "Large meta: owner_thread offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, free_list_head) ==
		  offsetof(struct v8m_page_meta, free_list_head),
	      "Large meta: free_list_head offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, next) ==
		  offsetof(struct v8m_page_meta, next),
	      "Large meta: next offset diverges");

static_assert(sizeof(struct v8m_large_page_meta) <= V8M_SLAB_HEADER_SIZE,
	      "Large meta exceeds V8M_SLAB_HEADER_SIZE");

/*
 * Sentinel stored in size_class for Huge allocations (> 2 MiB), so a
 * walker that wants to distinguish Huge from Large can check against
 * V8M_LARGE_HUGE_TAG. Large allocations (classes 38..40) keep their
 * actual class value in the field.
 */
#define V8M_LARGE_HUGE_TAG UINT16_MAX

/*
 * Per-class lifetime counters. Updated on every alloc / free; read
 * by v8m_large_get_stats and the public v8m_huge_stats reporter.
 * All relaxed because they're diagnostic — the surrounding
 * page-heap counters carry the load-bearing happens-before edges.
 */
static _Atomic uint64_t v8m_large_alloc_count;
static _Atomic uint64_t v8m_large_free_count;
static _Atomic uint64_t v8m_large_bytes_in_use;
static _Atomic uint64_t v8m_huge_alloc_count;
static _Atomic uint64_t v8m_huge_free_count;
static _Atomic uint64_t v8m_huge_bytes_in_use;

/*
 * Round `value` up to the next multiple of `multiple`. `multiple`
 * must be a power of two; the caller checks that.
 */
static size_t round_up_pow2(size_t value, size_t multiple)
{
	return (value + multiple - 1U) & ~(multiple - 1U);
}

/*
 * Bytes of red zone available for `requested_size` past the user
 * pointer, capped at V8M_LARGE_REDZONE_MAX_BYTES. The window is the
 * difference between the rounded-up usable_size and the caller's
 * requested size; if the request happened to align exactly to the
 * page boundary the window is zero and the helper returns zero
 * (trailing guard page still catches overflows past the page).
 */
static size_t large_redzone_span(const struct v8m_large_page_meta *meta,
				 uintptr_t header_offset)
{
	size_t accessible = meta->mmap_size - header_offset - meta->guard_bytes;
	if (meta->requested_size >= accessible) {
		return 0U;
	}
	size_t tail = accessible - meta->requested_size;
	if (tail > V8M_LARGE_REDZONE_MAX_BYTES) {
		tail = V8M_LARGE_REDZONE_MAX_BYTES;
	}
	return tail;
}

static void large_fill_redzone(unsigned char *user_ptr,
			       const struct v8m_large_page_meta *meta,
			       uintptr_t header_offset)
{
	size_t span = large_redzone_span(meta, header_offset);
	if (span == 0U) {
		return;
	}
	(void)memset(user_ptr + meta->requested_size, V8M_LARGE_REDZONE_BYTE,
		     span);
}

static void large_check_redzone(const unsigned char *user_ptr,
				const struct v8m_large_page_meta *meta,
				uintptr_t header_offset)
{
	size_t span = large_redzone_span(meta, header_offset);
	if (span == 0U) {
		return;
	}
	const unsigned char *zone = user_ptr + meta->requested_size;
	for (size_t i = 0; i < span; i++) {
		if (zone[i] != V8M_LARGE_REDZONE_BYTE) {
			(void)fprintf(
			    stderr,
			    "v8malloc DEBUG: red-zone corrupted at "
			    "offset %zu past requested size (%zu) of "
			    "allocation %p — heap overflow detected\n",
			    i, meta->requested_size, (const void *)user_ptr);
			abort();
		}
	}
}

/*
 * Bumping mmap_size and the page-heap alignment to one kernel
 * huge page lets the page heap try MAP_HUGETLB for Huge
 * allocations (size > V8M_LARGE_MAX_SIZE) when V8M_OPT_HUGE_PAGES
 * is enabled. The constant comes from v8m_arch.h: 2 MiB on every
 * Tier 1/2 arch we ship today, 1 MiB on s390x where the kernel
 * default huge page is 1 MiB. An s390x Huge allocation of e.g.
 * 4 MiB will land as four 1 MiB huge pages instead of two
 * 2 MiB pages on x86_64.
 */
#define V8M_LARGE_HUGE_ALIGN V8M_HUGE_PAGE_SIZE

/*
 * Gigantic allocations (>= 1 GiB) bump the page-heap alignment to
 * 1 GiB so the page heap can attempt MAP_HUGETLB | MAP_HUGE_1GB.
 * The over-alignment cost is bounded (≤ 1 GiB of virtual address
 * space, negligible against a multi-GiB allocation); the win when
 * 1 GiB huge pages are reserved is one TLB entry per gigabyte
 * versus 512 TLB entries with 2 MiB pages.
 */
#define V8M_LARGE_GIGANTIC_ALIGN ((size_t)1024 * 1024 * 1024)

/*
 * Huge/Large recycle cache. Recently-freed regions stay mapped
 * (region map still owns them, meta cleared) so a follow-on
 * allocation of the matching mmap_size can pop from here instead
 * of paying mmap + munmap + page-fault on every iteration. This is
 * the v0 deferred-decommit cousin of tcmalloc's huge-page pool.
 *
 * Why it matters: mb_01's 2 MiB column was 60 % slower than
 * tcmalloc because every iteration was a fresh mmap (with
 * MAP_HUGETLB attempt) plus a fresh munmap (with TLB shootdown).
 * tcmalloc keeps the region around. The cache below keeps it the
 * same way — each cached entry holds the original mmap_size + the
 * region pointer; the region map still considers the region owned
 * (we never call v8m_page_heap_free on cached entries), so
 * v8m_page_heap_owns continues to return true.
 *
 * Capacity bounds the worst-case retained virtual address space at
 * V8M_LARGE_CACHE_CAP × max_cached_mmap_size. With the cap below
 * (8 entries) and a typical 2 MiB Huge alloc that's 16 MiB; a
 * pathological caller that frees a single 256 MiB Huge fills one
 * slot and bounds at 256 MiB. Eviction is FIFO: the oldest entry
 * is munmap'd when a free wants to push into a full cache.
 *
 * Concurrency: single mutex serialises both alloc-pop and free-push.
 * The cache is the cold path's penultimate step (alloc) / first
 * step (free), so the lock cost is amortised against the syscall
 * cost it replaces.
 */
#define V8M_LARGE_CACHE_CAP 8

struct large_cache_entry {
	void *region;
	size_t mmap_size;
};

static struct large_cache_entry g_large_cache[V8M_LARGE_CACHE_CAP];
static size_t g_large_cache_count;
/* NOLINTNEXTLINE(misc-include-cleaner) — pthread.h is included above */
static pthread_mutex_t g_large_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pop a cached region matching `mmap_size`. Returns NULL on miss
 * (no entry of that size in the cache) so the caller falls through
 * to v8m_page_heap_alloc. Linear scan over the bounded array is
 * O(CAP). */
static void *large_cache_take(size_t mmap_size)
{
	void *result = NULL;
	(void)pthread_mutex_lock(&g_large_cache_lock);
	for (size_t i = 0; i < g_large_cache_count; i++) {
		if (g_large_cache[i].mmap_size == mmap_size) {
			result = g_large_cache[i].region;
			/* Compact: replace this slot with the last
			 * entry, drop the count by one. Order doesn't
			 * matter for FIFO-with-LIFO-by-size semantics
			 * — the eviction policy only fires when full,
			 * and the test is "any matching size", so the
			 * compaction is a safe O(1) drop. */
			g_large_cache_count--;
			if (i < g_large_cache_count) {
				g_large_cache[i] =
				    g_large_cache[g_large_cache_count];
			}
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_large_cache_lock);
	return result;
}

/* Drain every cached region back to the page heap. Called from
 * v8m_purge so callers asking for VMA / RSS relief actually get it
 * (the cache otherwise hoards regions across an indefinite tail).
 * Returns the number of regions released. */
size_t v8m_large_cache_drain(void)
{
	struct large_cache_entry snapshot[V8M_LARGE_CACHE_CAP];
	size_t snapshot_count = 0;
	(void)pthread_mutex_lock(&g_large_cache_lock);
	for (size_t i = 0; i < g_large_cache_count; i++) {
		snapshot[i] = g_large_cache[i];
	}
	snapshot_count = g_large_cache_count;
	g_large_cache_count = 0;
	(void)pthread_mutex_unlock(&g_large_cache_lock);
	for (size_t i = 0; i < snapshot_count; i++) {
		v8m_page_heap_free(snapshot[i].region, snapshot[i].mmap_size);
	}
	return snapshot_count;
}

/* Push a region into the cache. Returns NULL on success (caller
 * must NOT munmap the region), or the original region pointer
 * (with `out_evicted_size` set to the matching mmap_size) when the
 * cache was full and the oldest entry was evicted to make room —
 * the caller then munmaps the evicted entry to release VMAs.
 *
 * The "evicted entry returned via out parameters" pattern keeps
 * the syscall outside the lock. */
static void *large_cache_put(void *region, size_t mmap_size,
			     size_t *out_evicted_size)
{
	void *evicted = NULL;
	*out_evicted_size = 0;
	(void)pthread_mutex_lock(&g_large_cache_lock);
	if (g_large_cache_count == V8M_LARGE_CACHE_CAP) {
		/* Evict the oldest (slot 0). Shift the rest down by one.
		 * O(CAP) is fine — CAP is 8. */
		evicted = g_large_cache[0].region;
		*out_evicted_size = g_large_cache[0].mmap_size;
		for (size_t i = 1; i < g_large_cache_count; i++) {
			g_large_cache[i - 1] = g_large_cache[i];
		}
		g_large_cache_count--;
	}
	g_large_cache[g_large_cache_count].region = region;
	g_large_cache[g_large_cache_count].mmap_size = mmap_size;
	g_large_cache_count++;
	(void)pthread_mutex_unlock(&g_large_cache_lock);
	return evicted;
}

/*
 * Shared backend used by v8m_large_alloc / v8m_large_alloc_aligned.
 * `header_offset` places the user pointer; `pheap_alignment` is
 * the alignment requested from the page heap (always >=
 * V8M_PAGE_SIZE) and also drives the mmap_size rounding so the
 * resulting allocation is shaped for the page heap's MAP_HUGETLB
 * attempt when applicable.
 *
 * Under V8M_OPT_DEBUG the function appends one V8M_PAGE_SIZE
 * guard at the end of the region and mprotect()s it PROT_NONE
 * so any out-of-bounds write past the user-data window
 * traps instead of silently corrupting an adjacent mapping
 * (api.md §6.2). The guard size is recorded on the meta so
 * usable_size can report the accessible window correctly; free
 * unmaps the whole region (mmap_size already includes the
 * guard), so no DEBUG/non-DEBUG branching is needed there.
 */
/* The four parameters share size-or-uint types; the linter flags
 * the adjacent size_t pair as swappable. The naming pins each
 * role (`header_offset`, `pheap_alignment`, `owner_thread`) and
 * the function is internal to this file — wrapping in a struct
 * just for the linter's benefit would obscure the intent. */
/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
static void *large_alloc_with_offset(size_t size, size_t header_offset,
				     size_t pheap_alignment,
				     uint64_t owner_thread)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
	/* Overflow guard: need room for the header + size, then round
	 * up to the page-heap alignment. */
	if (size > SIZE_MAX - header_offset - pheap_alignment) {
		return NULL;
	}

	size_t total = header_offset + size;
	size_t mmap_size = round_up_pow2(total, pheap_alignment);

	bool guard_on = v8m_config_get(V8M_OPT_DEBUG) != 0;
	size_t guard_bytes = 0;
	if (guard_on) {
		if (mmap_size > SIZE_MAX - V8M_PAGE_SIZE) {
			return NULL;
		}
		guard_bytes = V8M_PAGE_SIZE;
		mmap_size += guard_bytes;
	}

	/* Recycle a cached region of matching mmap_size before paying
	 * for a fresh page-heap mmap. Skipped under DEBUG: a cached
	 * region's PROT_NONE guard page would still be in place from
	 * the prior allocation, which is benign — but the redzone
	 * canary check at free time was based on the prior alloc's
	 * `requested_size`, so reusing under DEBUG would mis-bound the
	 * window. Easier to just skip the cache when DEBUG is on. */
	void *region = NULL;
	if (__builtin_expect(!guard_on, 1)) {
		region = large_cache_take(mmap_size);
	}
	if (region == NULL) {
		region = v8m_page_heap_alloc(mmap_size, pheap_alignment);
		if (__builtin_expect(region == NULL, 0)) {
			return NULL;
		}
	}

	if (__builtin_expect(guard_on, 0)) {
		void *guard = (unsigned char *)region + mmap_size - guard_bytes;
		if (mprotect(guard, guard_bytes, PROT_NONE) != 0) {
			/* mprotect failure is rare (kernel out of VMAs is
			 * the realistic cause). Release the region and
			 * fail the alloc — silently downgrading to a
			 * no-guard allocation under DEBUG would defeat
			 * the diagnostic. */
			v8m_page_heap_free(region, mmap_size);
			return NULL;
		}
	}

	struct v8m_large_page_meta *meta = region;
	uint32_t cls = v8m_size_class(size);
	uint16_t size_class_tag = (cls == V8M_CLASS_HUGE)
				      ? (uint16_t)V8M_LARGE_HUGE_TAG
				      : (uint16_t)cls;

	meta->magic = V8M_MAGIC;
	meta->size_class = size_class_tag;
	meta->object_size = 0;
	meta->capacity = 1;
	atomic_store_explicit(&meta->used_count, 1U, memory_order_relaxed);
	meta->owner_thread = owner_thread;
	meta->free_list_head = NULL;
	meta->next = NULL;
	meta->mmap_size = mmap_size;
	meta->guard_bytes = guard_bytes;
	meta->requested_size = guard_on ? size : 0U;

	if (__builtin_expect(guard_on, 0)) {
		unsigned char *user = (unsigned char *)region + header_offset;
		large_fill_redzone(user, meta, header_offset);
	}

	if (cls == V8M_CLASS_HUGE) {
		atomic_fetch_add_explicit(&v8m_huge_alloc_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&v8m_huge_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&v8m_large_alloc_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&v8m_large_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	}

	return (unsigned char *)region + header_offset;
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_large_alloc(size_t size, uint64_t owner_thread)
{
	if (__builtin_expect(size == 0, 0)) {
		return NULL;
	}
	/* Huge allocations bump the page-heap alignment to 2 MiB so
	 * the page heap's MAP_HUGETLB attempt is in play. The mmap_size
	 * is rounded to the same alignment, wasting at most 2 MiB of
	 * virtual address space per Huge alloc — small relative to the
	 * request. Gigantic allocations (>= 1 GiB) bump further to
	 * 1 GiB so the page heap can attempt MAP_HUGE_1GB. Large
	 * allocations (256 KiB - 2 MiB) keep the V8M_PAGE_SIZE
	 * alignment the v0 baseline used. */
	size_t pheap_alignment = V8M_PAGE_SIZE;
	if (v8m_config_get(V8M_OPT_HUGE_PAGES) != 0) {
		if (size >= V8M_LARGE_GIGANTIC_ALIGN) {
			pheap_alignment = V8M_LARGE_GIGANTIC_ALIGN;
		} else if (size > V8M_LARGE_MAX_SIZE) {
			pheap_alignment = V8M_LARGE_HUGE_ALIGN;
		}
	}
	return large_alloc_with_offset(size, V8M_SLAB_HEADER_SIZE,
				       pheap_alignment, owner_thread);
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_large_alloc_aligned(size_t size, size_t alignment,
			      uint64_t owner_thread)
{
	if (__builtin_expect(size == 0, 0)) {
		return NULL;
	}
	if (__builtin_expect(
		alignment == 0 || alignment <= V8M_SLAB_HEADER_SIZE, 1)) {
		/* Default header offset already satisfies alignment <=
		 * V8M_SLAB_HEADER_SIZE (which is a power of two). The
		 * aligned variant always uses V8M_PAGE_SIZE for the
		 * page-heap alignment — callers asked for specific user
		 * alignment, so don't over-align them into MAP_HUGETLB
		 * eligibility. */
		return large_alloc_with_offset(size, V8M_SLAB_HEADER_SIZE,
					       V8M_PAGE_SIZE, owner_thread);
	}
	/* The header sits at region offset 0; the user pointer is at
	 * offset header_offset. v8m_ptr_to_meta masks away the low
	 * V8M_PAGE_SHIFT bits, so the meta is recoverable iff
	 * header_offset < V8M_PAGE_SIZE. */
	if (alignment >= V8M_PAGE_SIZE) {
		return NULL;
	}
	/* alignment is a power of two > V8M_SLAB_HEADER_SIZE, so the
	 * smallest multiple of alignment that admits the header is
	 * `alignment` itself. */
	return large_alloc_with_offset(size, alignment, V8M_PAGE_SIZE,
				       owner_thread);
}

void v8m_large_free(const void *obj)
{
	if (__builtin_expect(obj == NULL, 0)) {
		return;
	}
	/* Recover the page base via the shared ptr-to-meta helper so the
	 * int-to-ptr cast stays behind v8m_ptr_to_meta and this module
	 * doesn't need its own NOLINT. ptr_to_meta returns a non-const
	 * pointer, so writing to the header fields below is well-formed
	 * even though the caller's object pointer is const-qualified. */
	struct v8m_page_meta *common = v8m_ptr_to_meta(obj);
	struct v8m_large_page_meta *meta = (struct v8m_large_page_meta *)common;
	size_t mmap_size = meta->mmap_size;
	bool was_huge = meta->size_class == (uint16_t)V8M_LARGE_HUGE_TAG;

	/* Verify the red zone before tearing down the meta. `guard_bytes`
	 * is the marker for "DEBUG was on at alloc time" — if it's zero
	 * the allocation predates DEBUG and there is no canary to check. */
	if (__builtin_expect(meta->guard_bytes != 0U, 0)) {
		uintptr_t header_offset = (uintptr_t)obj & (V8M_PAGE_SIZE - 1U);
		large_check_redzone((const unsigned char *)obj, meta,
				    header_offset);
	}

	atomic_store_explicit(&meta->used_count, 0U, memory_order_relaxed);
	meta->magic = 0U;

	if (__builtin_expect(was_huge, 0)) {
		atomic_fetch_add_explicit(&v8m_huge_free_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_sub_explicit(&v8m_huge_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&v8m_large_free_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_sub_explicit(&v8m_large_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	}

	/* Stash the region in the recycle cache so a follow-on alloc of
	 * the same mmap_size can skip the page-heap roundtrip. Skipped
	 * under DEBUG (the cached region carries a PROT_NONE guard +
	 * stale redzone-baseline that would mismatch the next alloc's
	 * `requested_size`). On cache-full, the oldest entry is evicted
	 * here and we munmap that one instead — keeps the steady-state
	 * VMA count bounded. */
	if (__builtin_expect(meta->guard_bytes == 0U, 1)) {
		size_t evicted_size = 0;
		void *evicted =
		    large_cache_put(common, mmap_size, &evicted_size);
		if (evicted != NULL) {
			v8m_page_heap_free(evicted, evicted_size);
		}
		return;
	}
	v8m_page_heap_free(common, mmap_size);
}

size_t v8m_large_usable_size(const void *obj)
{
	if (__builtin_expect(obj == NULL, 0)) {
		return 0;
	}
	const struct v8m_page_meta *common = v8m_ptr_to_meta(obj);
	const struct v8m_large_page_meta *meta =
	    (const struct v8m_large_page_meta *)common;
	/* The user pointer sits at offset header_offset from the page
	 * base; recover that offset from the pointer's low bits. For the
	 * default path header_offset == V8M_SLAB_HEADER_SIZE, for the
	 * aligned variant it equals the requested alignment. Both fit in
	 * the page (< V8M_PAGE_SIZE), so the low-bits trick is exact.
	 * Subtract the trailing guard (0 outside V8M_OPT_DEBUG) so the
	 * reported window stops at the first inaccessible byte. */
	uintptr_t header_offset = (uintptr_t)obj & (V8M_PAGE_SIZE - 1U);
	return meta->mmap_size - header_offset - meta->guard_bytes;
}

void v8m_large_get_stats(struct v8m_large_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->large_alloc_count =
	    atomic_load_explicit(&v8m_large_alloc_count, memory_order_relaxed);
	out->large_free_count =
	    atomic_load_explicit(&v8m_large_free_count, memory_order_relaxed);
	out->large_bytes_in_use =
	    atomic_load_explicit(&v8m_large_bytes_in_use, memory_order_relaxed);
	out->huge_alloc_count =
	    atomic_load_explicit(&v8m_huge_alloc_count, memory_order_relaxed);
	out->huge_free_count =
	    atomic_load_explicit(&v8m_huge_free_count, memory_order_relaxed);
	out->huge_bytes_in_use =
	    atomic_load_explicit(&v8m_huge_bytes_in_use, memory_order_relaxed);
}
