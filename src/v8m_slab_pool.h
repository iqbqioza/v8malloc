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

struct v8m_slab_pool {
	/* Indices V8M_MEDIUM_FIRST_CLASS and above are unused; the
	 * pool only serves Tiny + Small. */
	struct v8m_slab_pool_class classes[V8M_MEDIUM_FIRST_CLASS];
	pthread_mutex_t lock;
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
 * Return one object to the pool. `meta` must be the result of
 * v8m_ptr_to_meta(obj) and must have already passed
 * v8m_page_meta_valid. Returns true iff the page became empty
 * (after which the pool has already returned it to the page heap).
 */
bool v8m_slab_pool_free(struct v8m_slab_pool *pool, struct v8m_page_meta *meta,
			void *obj);

#endif /* V8M_SLAB_POOL_H */
