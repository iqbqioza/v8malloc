/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Slab pool — per-class management of Tiny and Small slab pages.
 * For every size class 0..31 the pool holds a current page (the one
 * being allocated from) and a list of partial pages (pages that have
 * at least one free slot but aren't current). Full pages are simply
 * dropped from the lists; a free that brings a full page back below
 * capacity re-inserts it into the partials chain. Empty pages are
 * returned to the page heap.
 *
 * Concurrency: a single per-pool mutex serializes all operations.
 * This is the single-threaded baseline; future cycles add the
 * thread-local cache (lock-free fast path) and the per-NUMA pool
 * structure on top of this. Until then, the slab pool is the
 * canonical allocation path for Tiny and Small classes.
 *
 * The pool dispatches between Tiny (bitmap) and Small (free-list)
 * slabs internally based on `meta->size_class`; callers see one
 * uniform API.
 *
 * See architecture.md §2.3 (NUMA pool, of which this is the
 * non-NUMA-specialized core) and thread-cache.md §6.
 */

#ifndef V8M_SLAB_POOL_H
#define V8M_SLAB_POOL_H

#include <pthread.h> /* IWYU pragma: keep */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_arch.h" /* V8M_CACHELINE_ALIGNED */
#include "v8m_page.h"
#include "v8m_size_class.h"

/*
 * Per-class state. `current` is the page allocations come from; when
 * it fills it is dropped (full pages off all lists). Partial pages
 * (used_count in (0, capacity)) live in `partials` linked through
 * v8m_page_meta::next.
 */
struct v8m_slab_pool_class {
	struct v8m_page_meta *current;
	struct v8m_page_meta *partials;
};

/*
 * Drained-slab-page cache (mirror of the buddy pool's drained-arena
 * pattern). When a slab page becomes empty in `v8m_slab_pool_free`,
 * release_slab_page first tries to park the page in this cache:
 * applies MADV_DONTNEED (kernel reclaims physical pages, virtual
 * mapping stays intact) and pushes the raw page pointer onto the
 * stack. A subsequent acquire_slab_page pops the cache first so a
 * revival within the bg-purge grace window reuses the slot without
 * an mmap/munmap round trip. Caller of acquire treats the returned
 * page as fresh and re-runs slab init — that overwrites the meta
 * header, which the kernel had zeroed on the next fault, so the
 * "metadata co-located with data area" constraint is satisfied
 * without a separate handshake. The cache is per-pool so isolated
 * test pools do not share parked pages. Cap is small (8) so the
 * worst-case retained mapping is bounded; pages that age past the
 * sweep threshold are released to the underlying source
 * (numa_pool or page_heap) via the original release path.
 */
#define V8M_SLAB_DRAINED_CAP 8U

struct v8m_slab_pool {
	/* Indices V8M_MEDIUM_FIRST_CLASS and above are unused; the
	 * pool only serves Tiny + Small. */
	struct v8m_slab_pool_class classes[V8M_MEDIUM_FIRST_CLASS];
	/* Lock sits on its own cache line so a core spinning on the
	 * futex doesn't bounce the line every neighbouring field
	 * touches. The wrapping struct (v8m_dispatch) places buddy
	 * state immediately after the slab pool, so without padding
	 * the slab.lock and the head of buddy.arenas[0] would share
	 * a line on x86_64 (where V8M_CACHE_LINE_SIZE == 64). */
	V8M_CACHELINE_ALIGNED pthread_mutex_t lock;
	/* When true, fresh slab pages are sourced from the global
	 * per-NUMA huge-page pool (`g_slab_numa_pool`) instead of
	 * direct page-heap mmap. The dispatcher singleton sets this
	 * in the constructor; isolated test pools leave it false so
	 * their alloc/free path stays on the discrete page-heap path
	 * (avoids cross-pool slab-page sharing through the global
	 * numa_pool). */
	bool use_numa_pool;
	/* Drained-page cache. Parallel arrays so the hot scan over
	 * `drained_count` entries reads contiguous memory; idle_ticks
	 * is touched only on sweep so it stays separate from the
	 * page-pointer array the acquire path scans. */
	void *drained[V8M_SLAB_DRAINED_CAP];
	uint32_t drained_idle[V8M_SLAB_DRAINED_CAP];
	uint32_t drained_count;
};

/*
 * Initialize a freshly-allocated pool. Returns 0 on success, an
 * errno-style code on pthread_mutex_init failure.
 */
int v8m_slab_pool_init(struct v8m_slab_pool *pool);

/*
 * Tear down the pool. Releases every page held in current/partials
 * back to the page heap, then destroys the mutex. Callers must
 * ensure no allocations remain live (the pool does not track
 * outstanding allocations and cannot reclaim them).
 */
void v8m_slab_pool_destroy(struct v8m_slab_pool *pool);

/*
 * Allocate one object of size class `size_class` (must be 0..31, ie
 * a Tiny or Small class). Returns NULL on out-of-range class or on
 * page-heap exhaustion. `owner_thread` is recorded in the meta of
 * any newly-acquired page; passes through to v8m_slab_*_init.
 */
void *v8m_slab_pool_alloc(struct v8m_slab_pool *pool, uint32_t size_class,
			  uint64_t owner_thread);

/*
 * Same contract as v8m_slab_pool_alloc but stamps the page's
 * arena_id field so the free path can route the page back to the
 * matching pool. The default-arena variant above is implemented
 * as `v8m_slab_pool_alloc_arena(pool, cls, owner, V8M_ARENA_DEFAULT)`.
 */
void *v8m_slab_pool_alloc_arena(struct v8m_slab_pool *pool, uint32_t size_class,
				uint64_t owner_thread, uint8_t arena_id);

/*
 * Return one object to the pool. `meta` must be the result of
 * v8m_ptr_to_meta(obj) and must have already passed
 * v8m_page_meta_valid. Returns true iff the page became empty
 * (after which the pool has already returned it to the page heap).
 */
bool v8m_slab_pool_free(struct v8m_slab_pool *pool, struct v8m_page_meta *meta,
			void *obj);

/*
 * Aggregate utilization snapshot across every Tiny/Small class held
 * by the pool. Walks current + partials per class under the pool
 * lock, summing page count, slot capacity, and slot occupancy.
 * Empty pages have already been returned to the page heap by the
 * free path, so the walk does not see them; full pages are off all
 * lists by design and are likewise invisible. The snapshot
 * therefore reports "actively partitioned" slab pages — the
 * population that drives operational utilization decisions
 * (fragmentation.md §4.1). Reporters can derive
 * `utilization = slots_used / slots_total` from the two counters.
 */
struct v8m_slab_pool_aggregate_stats {
	uint64_t pages_in_use;
	uint64_t slots_total;
	uint64_t slots_used;
};

void v8m_slab_pool_get_aggregate_stats(
    struct v8m_slab_pool *pool, struct v8m_slab_pool_aggregate_stats *out);

/*
 * Age every entry in the drained-page cache by one tick and release
 * those that have reached `max_idle_ticks` to the underlying source
 * (numa_pool or page_heap). Returns the number of pages released.
 * Mirror of `v8m_buddy_pool_sweep_idle`. Called by the dispatcher's
 * bg_tick callback. Safe to call on a NULL pool or one that never
 * cached anything.
 */
size_t v8m_slab_pool_sweep_idle(struct v8m_slab_pool *pool,
				uint32_t max_idle_ticks);

/*
 * Force-release every page in the drained cache to the underlying
 * source. Equivalent to `v8m_slab_pool_sweep_idle(pool, 0)`. Used
 * by `v8m_purge` for immediate VMA / RSS relief.
 */
size_t v8m_slab_pool_purge_drained(struct v8m_slab_pool *pool);

/*
 * Snapshot of the drained-page cache's occupancy, exposed for
 * tests and diagnostics. Counts the number of currently parked
 * pages (0..V8M_SLAB_DRAINED_CAP).
 */
uint32_t v8m_slab_pool_drained_count(struct v8m_slab_pool *pool);

/*
 * Initialize the global per-NUMA huge-page pool that sources slab
 * pages for any v8m_slab_pool instance with `use_numa_pool` set.
 * Returns 0 on success or a negative errno on init failure.
 * Idempotent: a second call returns 0 without re-initializing.
 */
int v8m_slab_pool_global_init(void);

/*
 * Tear down the global per-NUMA huge-page pool. Releases every
 * huge page held by every node back to the page heap. Safe on a
 * never-initialized global.
 */
void v8m_slab_pool_global_destroy(void);

/*
 * Opt the pool into routing fresh slab pages through the global
 * per-NUMA huge-page pool (true) or keeping the discrete page-heap
 * path (false). The dispatcher singleton's constructor sets this
 * to true after `v8m_slab_pool_global_init` succeeds; isolated
 * test pools stay on false.
 */
void v8m_slab_pool_set_use_numa_pool(struct v8m_slab_pool *pool,
				     bool use_numa_pool);

#endif /* V8M_SLAB_POOL_H */
