/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Buddy pool — manages a fixed-cap array of v8m_buddy arenas to back
 * Medium-class allocations (8 KiB – 256 KiB; size classes 32..37).
 * Each arena is a 256 KiB-aligned region from the page heap.
 *
 * Allocation tries existing arenas first; if none has room, it
 * lazily acquires a fresh arena. Free locates the owning arena by
 * scanning the in-use slots (each arena's range is bounded), then
 * delegates to v8m_buddy_free; arenas that drain to empty are
 * returned to the page heap.
 *
 * The size-less v8m_buddy_pool_free recovers the allocation's size
 * from the buddy's bitmaps via v8m_buddy_block_size, so the public
 * free() surface doesn't need to track per-allocation sizes.
 *
 * Concurrency: a single per-pool mutex serializes all operations.
 * The pool is the single-threaded baseline that the future TLC + L2
 * core cache will sit in front of, mirroring the slab pool's role.
 */

#ifndef V8M_BUDDY_POOL_H
#define V8M_BUDDY_POOL_H

#include <pthread.h> /* IWYU pragma: keep */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_arch.h" /* V8M_CACHELINE_ALIGNED */
#include "v8m_buddy.h"

/*
 * Maximum number of concurrently-live buddy arenas the pool will
 * keep. 64 arenas × 256 KiB = 16 MiB of Medium-class capacity —
 * comfortable for a v0; the future per-NUMA-node pool will lift the
 * cap by sharding.
 */
#define V8M_BUDDY_POOL_MAX_ARENAS 64

struct v8m_buddy_pool_arena {
	struct v8m_buddy buddy;
	bool in_use;
	/*
	 * Drain-and-keep state. When a free path empties an arena, we
	 * apply MADV_DONTNEED to release physical frames but keep the
	 * VMA + buddy bookkeeping intact, so a follow-up alloc can
	 * reuse the slot without an mmap/munmap round trip. The arena
	 * stays `in_use=true` while drained; `idle_ticks` advances by
	 * one each bg-purge sweep that did not find an alloc reviving
	 * it. After enough idle ticks the sweep releases the arena
	 * fully — see v8m_buddy_pool_sweep_idle.
	 */
	bool drained;
	uint32_t idle_ticks;
};

struct v8m_buddy_pool {
	struct v8m_buddy_pool_arena arenas[V8M_BUDDY_POOL_MAX_ARENAS];
	/* Cache-line padded — see the matching note on v8m_slab_pool::lock. */
	V8M_CACHELINE_ALIGNED pthread_mutex_t lock;
};

/*
 * Initialize a freshly-allocated pool. Returns 0 on success, the
 * pthread_mutex_init errno on lock-init failure.
 */
int v8m_buddy_pool_init(struct v8m_buddy_pool *pool);

/*
 * Tear down the pool. Releases every in-use arena back to the page
 * heap and destroys the mutex. Callers must ensure no allocations
 * remain live.
 */
void v8m_buddy_pool_destroy(struct v8m_buddy_pool *pool);

/*
 * Allocate `size` bytes from the pool. Returns NULL on size 0,
 * size > V8M_BUDDY_MAX_BLOCK, page-heap exhaustion, or arena-cap
 * exhaustion (V8M_BUDDY_POOL_MAX_ARENAS reached and all full).
 */
void *v8m_buddy_pool_alloc(struct v8m_buddy_pool *pool, size_t size);

/*
 * Return a previously-issued allocation. Returns true iff the pool
 * owned `ptr` (and therefore freed it). Returns false for NULL,
 * foreign pointers, and pointers that fall inside an in-use arena
 * but don't sit at the start of any allocated buddy block. The
 * boolean lets the dispatch layer fall through to other handlers
 * for foreign pointers; whether the arena itself was reclaimed on
 * this free can be inferred from `v8m_page_heap_get_stats`.
 */
bool v8m_buddy_pool_free(struct v8m_buddy_pool *pool, void *ptr);

/*
 * Byte size of the buddy-pool allocation containing `ptr`. Returns
 * 0 if `ptr` is NULL, lies outside every in-use arena, or is not
 * the start of an allocated buddy block. Used by the dispatch
 * layer's malloc_usable_size for buddy allocations.
 */
size_t v8m_buddy_pool_block_size(struct v8m_buddy_pool *pool, const void *ptr);

/*
 * Periodic idle-arena sweep. Walks every in-use slot; for arenas
 * that are currently drained (no live blocks, MADV_DONTNEED already
 * applied) increments `idle_ticks` by one and, if the new count
 * meets or exceeds `max_idle_ticks`, releases the arena to the page
 * heap. Returns the number of arenas released this pass.
 *
 * Intended caller is the background purge thread; one tick per bg
 * purge interval. `max_idle_ticks` tunes how long a drained arena
 * is held for potential revival before the VMA is given back. A
 * value of 0 releases every drained arena immediately (defeats the
 * deferred-munmap optimization but useful for tests).
 *
 * Concurrency: takes the pool lock for the duration of the scan.
 */
size_t v8m_buddy_pool_sweep_idle(struct v8m_buddy_pool *pool,
				 uint32_t max_idle_ticks);

/*
 * Snapshot of arena occupancy. Reports the counts the bg purge and
 * frag-metrics reporters need without exposing the array directly.
 * Live = in_use && !drained; drained = in_use && drained;
 * total_in_use = live + drained.
 */
struct v8m_buddy_pool_arena_stats {
	uint32_t live;
	uint32_t drained;
	uint32_t total_in_use;
};

void v8m_buddy_pool_get_arena_stats(struct v8m_buddy_pool *pool,
				    struct v8m_buddy_pool_arena_stats *out);

#endif /* V8M_BUDDY_POOL_H */
