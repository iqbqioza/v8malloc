/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Slab pool implementation. Single-mutex for now; the lock is
 * acquired around every alloc/free operation. Dispatch between Tiny
 * and Small slabs is keyed by size_class against
 * V8M_SMALL_FIRST_CLASS (the boundary between bitmap-managed Tiny
 * and free-list-managed Small slabs).
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"
#include "v8m_slab_pool.h"
#include "v8m_slab_small.h"
#include "v8m_slab_tiny.h"

static void *slab_alloc_dispatch(struct v8m_page_meta *meta)
{
	if (meta->size_class < V8M_SMALL_FIRST_CLASS) {
		return v8m_slab_tiny_alloc(meta);
	}
	return v8m_slab_small_alloc(meta);
}

static bool slab_free_dispatch(struct v8m_page_meta *meta, void *obj)
{
	if (meta->size_class < V8M_SMALL_FIRST_CLASS) {
		return v8m_slab_tiny_free(meta, obj);
	}
	return v8m_slab_small_free(meta, obj);
}

static bool slab_is_full_dispatch(const struct v8m_page_meta *meta)
{
	if (meta->size_class < V8M_SMALL_FIRST_CLASS) {
		return v8m_slab_tiny_is_full(meta);
	}
	return v8m_slab_small_is_full(meta);
}

static void slab_init_dispatch(void *page, uint32_t size_class,
			       uint64_t owner_thread)
{
	if (size_class < V8M_SMALL_FIRST_CLASS) {
		v8m_slab_tiny_init(page, size_class, owner_thread);
	} else {
		v8m_slab_small_init(page, size_class, owner_thread);
	}
}

int v8m_slab_pool_init(struct v8m_slab_pool *pool)
{
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		pool->classes[i].current = NULL;
		pool->classes[i].partials = NULL;
	}
	return pthread_mutex_init(&pool->lock, NULL);
}

void v8m_slab_pool_destroy(struct v8m_slab_pool *pool)
{
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		struct v8m_slab_pool_class *cls = &pool->classes[i];
		if (cls->current != NULL) {
			v8m_page_heap_free(cls->current, V8M_PAGE_SIZE);
			cls->current = NULL;
		}
		struct v8m_page_meta *next = NULL;
		for (struct v8m_page_meta *page = cls->partials; page != NULL;
		     page = next) {
			next = page->next;
			v8m_page_heap_free(page, V8M_PAGE_SIZE);
		}
		cls->partials = NULL;
	}
	(void)pthread_mutex_destroy(&pool->lock);
}

/*
 * Try to draw an allocation from the current page; if it has
 * filled, drop it (full pages are off all lists) and signal the
 * caller to fall through to partials / fresh-page paths.
 */
static void *try_current(struct v8m_slab_pool_class *cls)
{
	if (cls->current == NULL) {
		return NULL;
	}
	void *obj = slab_alloc_dispatch(cls->current);
	if (obj == NULL) {
		cls->current = NULL;
	}
	return obj;
}

/*
 * Promote the most-utilized partial page into `current` and
 * allocate from it. Picking the page closest to full first
 * concentrates allocations and lets less-utilized pages drain to
 * empty (and back to the page heap) faster — fragmentation.md
 * §4.3's `v8m_select_allocation_page`. The scan is O(N partials)
 * per refill; v0 keeps it linear, the future per-class priority
 * queue / utilization-bucketed list optimizes when N grows large.
 *
 * Defensive fall-through: if the picked page is somehow already
 * full (shouldn't happen for partials, but a corrupted heap or a
 * race with a future lock-free path could expose it), drop it and
 * try the next page in walk order so the pool can still satisfy
 * the request.
 */
static void *try_partials(struct v8m_slab_pool_class *cls)
{
	while (cls->partials != NULL) {
		struct v8m_page_meta **best_link = &cls->partials;
		uint32_t best_used = 0;
		for (struct v8m_page_meta **link = &cls->partials;
		     *link != NULL; link = &(*link)->next) {
			uint32_t used = atomic_load_explicit(
			    &(*link)->used_count, memory_order_relaxed);
			if (used >= best_used) {
				best_used = used;
				best_link = link;
			}
		}

		struct v8m_page_meta *page = *best_link;
		*best_link = page->next;
		page->next = NULL;
		cls->current = page;
		void *obj = slab_alloc_dispatch(page);
		if (obj != NULL) {
			return obj;
		}
		cls->current = NULL;
	}
	return NULL;
}

/*
 * Acquire a fresh page from the page heap, format it as the slab
 * for `size_class`, install it as `current`, and allocate one
 * object from it.
 */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void *acquire_fresh_page(struct v8m_slab_pool_class *cls,
				uint32_t size_class, uint64_t owner_thread)
{
	void *page = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (page == NULL) {
		return NULL;
	}
	slab_init_dispatch(page, size_class, owner_thread);
	cls->current = page;
	return slab_alloc_dispatch(cls->current);
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_slab_pool_alloc(struct v8m_slab_pool *pool, uint32_t size_class,
			  uint64_t owner_thread)
{
	if (size_class >= V8M_MEDIUM_FIRST_CLASS) {
		return NULL;
	}

	(void)pthread_mutex_lock(&pool->lock);
	struct v8m_slab_pool_class *cls = &pool->classes[size_class];

	void *obj = try_current(cls);
	if (obj == NULL) {
		obj = try_partials(cls);
	}
	if (obj == NULL) {
		obj = acquire_fresh_page(cls, size_class, owner_thread);
	}

	(void)pthread_mutex_unlock(&pool->lock);
	return obj;
}

/* Find and unlink `meta` from `cls`'s lists. Tolerates `meta` being
 * absent (the page was full, hence off every list). */
static void unlink_from_class(struct v8m_slab_pool_class *cls,
			      struct v8m_page_meta *meta)
{
	if (cls->current == meta) {
		cls->current = NULL;
		return;
	}
	struct v8m_page_meta **link = &cls->partials;
	while (*link != NULL) {
		if (*link == meta) {
			*link = meta->next;
			meta->next = NULL;
			return;
		}
		link = &(*link)->next;
	}
}

bool v8m_slab_pool_free(struct v8m_slab_pool *pool, struct v8m_page_meta *meta,
			void *obj)
{
	(void)pthread_mutex_lock(&pool->lock);

	bool was_full = slab_is_full_dispatch(meta);
	bool became_empty = slab_free_dispatch(meta, obj);

	uint32_t size_class = meta->size_class;
	if (size_class >= V8M_MEDIUM_FIRST_CLASS) {
		(void)pthread_mutex_unlock(&pool->lock);
		return became_empty;
	}

	struct v8m_slab_pool_class *cls = &pool->classes[size_class];

	if (became_empty) {
		unlink_from_class(cls, meta);
		v8m_page_heap_free(meta, V8M_PAGE_SIZE);
	} else if (was_full) {
		/* Full -> partial transition; the page wasn't in any
		 * list, so add it to partials. */
		meta->next = cls->partials;
		cls->partials = meta;
	}
	/* Else: page was current or partial and stays where it is. */

	(void)pthread_mutex_unlock(&pool->lock);
	return became_empty;
}
