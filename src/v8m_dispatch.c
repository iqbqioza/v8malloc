/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Dispatcher implementation. Allocation routes by size class to the
 * slab pool, buddy pool, or direct mmap path. Free uses the
 * v8m_page_meta magic check to tell slab/large pages apart from
 * buddy and foreign pointers; buddy pages have no meta header so
 * the buddy pool's range-check ownership covers them.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_buddy.h"
#include "v8m_buddy_pool.h"
#include "v8m_dispatch.h"
#include "v8m_internal.h"
#include "v8m_large.h"
#include "v8m_libc_fallback.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"
#include "v8m_slab_pool.h"
#include "v8m_thread_cache.h"

int v8m_dispatch_init(struct v8m_dispatch *dispatch)
{
	int ret = v8m_slab_pool_init(&dispatch->slab);
	if (ret != 0) {
		return ret;
	}
	ret = v8m_buddy_pool_init(&dispatch->buddy);
	if (ret != 0) {
		v8m_slab_pool_destroy(&dispatch->slab);
		return ret;
	}
	dispatch->use_tlc = false;
	return 0;
}

void v8m_dispatch_set_use_tlc(struct v8m_dispatch *dispatch, bool enabled)
{
	if (dispatch == NULL) {
		return;
	}
	dispatch->use_tlc = enabled;
}

void v8m_dispatch_destroy(struct v8m_dispatch *dispatch)
{
	v8m_buddy_pool_destroy(&dispatch->buddy);
	v8m_slab_pool_destroy(&dispatch->slab);
}

void *v8m_dispatch_alloc(struct v8m_dispatch *dispatch, size_t size)
{
	/* malloc(0) — POSIX permits NULL or a unique pointer; return a
	 * unique pointer so that downstream code that frees the result
	 * doesn't need a special-case branch. */
	if (size == 0) {
		size = 1;
	}

	uint32_t cls = v8m_size_class(size);
	if (cls < V8M_MEDIUM_FIRST_CLASS) {
		/* TLC fast path: per-thread bin pop. Only enabled on
		 * dispatchers that opted in (`use_tlc` true) — the
		 * test fixtures that create their own dispatchers
		 * stay off TLC so cached objects do not cross-link
		 * between distinct slab pools. The cache is created
		 * on first touch; failure to allocate one (rare; only
		 * under extreme memory pressure) bypasses the cache
		 * and serves directly.
		 *
		 * Slow path: drain the cross-thread remote-free queue
		 * before falling through to the slab pool, so a
		 * follow-up retry can satisfy from drained slots
		 * without a pool-mutex round trip. The drain is a
		 * no-op in v0 (slab pages are pool-owned, so nothing
		 * pushes to the queue); it lights up when the
		 * thread-owned-slab refactor wires owner-thread
		 * routing. */
		if (dispatch->use_tlc) {
			struct v8m_thread_cache *cache =
			    v8m_thread_cache_get_or_create();
			if (cache != NULL) {
				void *cached =
				    v8m_thread_cache_alloc(cache, cls);
				if (cached != NULL) {
					return cached;
				}
				if (v8m_thread_cache_drain_remote(cache) > 0U) {
					cached =
					    v8m_thread_cache_alloc(cache, cls);
					if (cached != NULL) {
						return cached;
					}
				}
			}
		}
		return v8m_slab_pool_alloc(&dispatch->slab, cls, 0);
	}
	if (size <= V8M_BUDDY_MAX_BLOCK) {
		return v8m_buddy_pool_alloc(&dispatch->buddy, size);
	}
	return v8m_large_alloc(size, 0);
}

/*
 * Natural malloc alignment is alignof(max_align_t) — 16 on
 * x86_64 / aarch64. Slab objects already meet this without effort,
 * so smaller `alignment` requests collapse to v8m_dispatch_alloc.
 */
#define V8M_MALLOC_NATURAL_ALIGN ((size_t)16)

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_dispatch_alloc_aligned(struct v8m_dispatch *dispatch, size_t size,
				 size_t alignment)
{
	if (size == 0) {
		size = 1;
	}
	if (alignment <= V8M_MALLOC_NATURAL_ALIGN) {
		return v8m_dispatch_alloc(dispatch, size);
	}
	/* Hard cap: alignments above V8M_BUDDY_MAX_BLOCK have no in-v0
	 * backend that can satisfy them — the slab data offset tops out
	 * at V8M_SLAB_HEADER_SIZE, the buddy arena at
	 * V8M_BUDDY_MAX_BLOCK, and the large-path offset trick at
	 * V8M_PAGE_SIZE - 1. */
	if (alignment > V8M_BUDDY_MAX_BLOCK) {
		return NULL;
	}

	size_t effective = (size > alignment) ? size : alignment;

	/* Slab path: pick the smallest class whose object size is >=
	 * effective AND is a multiple of alignment. The slab data area
	 * starts at V8M_SLAB_HEADER_SIZE (or `object_size` for class
	 * 31), both page-relative; for any power-of-two alignment that
	 * also divides the data offset, the chosen class's objects are
	 * naturally alignment-aligned. */
	if (alignment <= V8M_SLAB_HEADER_SIZE &&
	    effective <= V8M_SMALL_MAX_SIZE) {
		uint32_t cls = v8m_size_class(effective);
		while (cls < V8M_MEDIUM_FIRST_CLASS) {
			uint32_t obj_size = v8m_class_to_size[cls];
			if ((obj_size & (alignment - 1U)) == 0U) {
				return v8m_slab_pool_alloc(&dispatch->slab, cls,
							   0);
			}
			cls++;
		}
		/* Fall through to buddy / large. */
	}

	/* Buddy path: blocks at level L are inherently
	 * (V8M_BUDDY_MIN_BLOCK << L)-aligned, so passing
	 * effective = max(size, alignment) yields a level whose block
	 * size is a power of two >= alignment. */
	if (effective <= V8M_BUDDY_MAX_BLOCK) {
		return v8m_buddy_pool_alloc(&dispatch->buddy, effective);
	}

	/* Large/Huge path: requires alignment < V8M_PAGE_SIZE so the
	 * header still sits at the page base recovered by ptr_to_meta.
	 * Higher alignments fell out at the buddy branch above (they
	 * cannot reach here because effective > V8M_BUDDY_MAX_BLOCK
	 * implies size > V8M_BUDDY_MAX_BLOCK, which combined with
	 * alignment <= V8M_BUDDY_MAX_BLOCK means alignment <
	 * V8M_PAGE_SIZE only when alignment <= V8M_PAGE_SIZE/2 — the
	 * v8m_large_alloc_aligned guard rejects the rest by returning
	 * NULL). */
	return v8m_large_alloc_aligned(size, alignment, 0);
}

/* `ptr` cannot be const-qualified: v8m_slab_small_free overwrites
 * the first sizeof(void *) bytes of the freed object with the
 * free-list link, so the type must remain non-const all the way
 * down. */
/* cppcheck-suppress constParameterPointer */
void v8m_dispatch_free(struct v8m_dispatch *dispatch, void *ptr)
{
	if (ptr == NULL) {
		return;
	}

	/* Safe foreign-vs-ours decision via the page-heap region
	 * map. Without this, the magic-check read below could fault
	 * on a foreign pointer whose page-aligned base sits in an
	 * unmapped page. */
	if (!v8m_page_heap_owns(ptr)) {
		v8m_libc_free(ptr);
		return;
	}

	struct v8m_page_meta *meta = v8m_ptr_to_meta(ptr);
	if (v8m_page_meta_valid(meta)) {
		if (meta->size_class < V8M_MEDIUM_FIRST_CLASS) {
			/* TLC fast path (gated on dispatch->use_tlc;
			 * see v8m_dispatch_alloc for the rationale).
			 * Overflow triggers a half-bin batch flush back
			 * to the slab pool so the cache cannot grow
			 * unboundedly. */
			struct v8m_thread_cache *cache =
			    dispatch->use_tlc ? v8m_thread_cache_get_or_create()
					      : NULL;
			if (cache != NULL) {
				bool overflowed = v8m_thread_cache_free(
				    cache, meta->size_class, ptr);
				if (overflowed) {
					(void)v8m_thread_cache_flush_half(
					    cache, &dispatch->slab,
					    meta->size_class);
				}
			} else {
				(void)v8m_slab_pool_free(&dispatch->slab, meta,
							 ptr);
			}
		} else {
			/* size_class is a Large class (38..40) or the
			 * UINT16_MAX Huge sentinel; v8m_large_free
			 * handles both via the same mmap_size header. */
			v8m_large_free(ptr);
		}
		return;
	}

	/* No magic at the page base — must be a buddy allocation
	 * (buddy arenas don't stamp v8m_page_meta). The page-heap
	 * ownership check above already excluded foreign pointers,
	 * so this is the only remaining backend. */
	(void)v8m_buddy_pool_free(&dispatch->buddy, ptr);
}

size_t v8m_dispatch_usable_size(struct v8m_dispatch *dispatch, const void *ptr)
{
	if (ptr == NULL) {
		return 0;
	}
	const struct v8m_page_meta *meta = v8m_ptr_to_meta(ptr);
	if (v8m_page_meta_valid(meta)) {
		if (meta->size_class < V8M_MEDIUM_FIRST_CLASS) {
			return meta->object_size;
		}
		return v8m_large_usable_size(ptr);
	}
	return v8m_buddy_pool_block_size(&dispatch->buddy, ptr);
}

/* --- pthread_atfork plumbing -------------------------------------- */

void v8m_dispatch_prefork(struct v8m_dispatch *dispatch)
{
	(void)pthread_mutex_lock(&dispatch->slab.lock);
	(void)pthread_mutex_lock(&dispatch->buddy.lock);
}

void v8m_dispatch_postfork_parent(struct v8m_dispatch *dispatch)
{
	(void)pthread_mutex_unlock(&dispatch->buddy.lock);
	(void)pthread_mutex_unlock(&dispatch->slab.lock);
}

void v8m_dispatch_postfork_child(struct v8m_dispatch *dispatch)
{
	(void)pthread_mutex_unlock(&dispatch->buddy.lock);
	(void)pthread_mutex_unlock(&dispatch->slab.lock);
}

/*
 * Idle-buddy-arena hold duration in bg-purge ticks. At the default
 * V8M_OPT_PURGE_INTERVAL of 1 second, 4 ticks ≈ 4 seconds of grace
 * before a drained arena is fully released — enough to absorb the
 * typical alloc/free burst cadence without retaining VMA pressure
 * on a long idle workload.
 */
#define V8M_DISPATCH_BUDDY_IDLE_TICKS 4U

size_t v8m_dispatch_bg_tick(struct v8m_dispatch *dispatch)
{
	if (dispatch == NULL) {
		return 0;
	}
	return v8m_buddy_pool_sweep_idle(&dispatch->buddy,
					 V8M_DISPATCH_BUDDY_IDLE_TICKS);
}

size_t v8m_dispatch_purge_drained(struct v8m_dispatch *dispatch)
{
	if (dispatch == NULL) {
		return 0;
	}
	/* max_idle_ticks = 0 → every drained arena passes the
	 * release test on the first walk. */
	return v8m_buddy_pool_sweep_idle(&dispatch->buddy, 0U);
}
