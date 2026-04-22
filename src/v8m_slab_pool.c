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
#include "v8m_numa.h" /* v8m_numa_current_node */
#include "v8m_numa_pool.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"
#include "v8m_slab_pool.h"

/*
 * Global per-NUMA huge-page pool. Sources slab pages for every
 * v8m_slab_pool whose `use_numa_pool` is true. Lazy-init via
 * v8m_slab_pool_global_init from the dispatcher's constructor;
 * shutdown via v8m_slab_pool_global_destroy from the destructor.
 * Test pools (created via v8m_slab_pool_init only) leave
 * `use_numa_pool` false and stay on the discrete page-heap path
 * so isolated tests don't share slab pages through the global.
 */
static struct v8m_numa_pool g_slab_numa_pool;
static atomic_bool g_slab_numa_pool_ready;

int v8m_slab_pool_global_init(void)
{
	if (atomic_load_explicit(&g_slab_numa_pool_ready,
				 memory_order_acquire)) {
		return 0;
	}
	int init_rc = v8m_numa_pool_init(&g_slab_numa_pool);
	if (init_rc != 0) {
		return init_rc;
	}
	atomic_store_explicit(&g_slab_numa_pool_ready, true,
			      memory_order_release);
	return 0;
}

void v8m_slab_pool_global_destroy(void)
{
	if (!atomic_load_explicit(&g_slab_numa_pool_ready,
				  memory_order_acquire)) {
		return;
	}
	atomic_store_explicit(&g_slab_numa_pool_ready, false,
			      memory_order_release);
	v8m_numa_pool_destroy(&g_slab_numa_pool);
}

void v8m_slab_pool_set_use_numa_pool(struct v8m_slab_pool *pool,
				     bool use_numa_pool)
{
	if (pool == NULL) {
		return;
	}
	pool->use_numa_pool = use_numa_pool;
}

/* Allocate a fresh slab page either from the global numa_pool
 * (when this pool opted in AND the global is ready) or directly
 * from the page heap (test pools or pre-init). Returns NULL on
 * failure of the chosen source. */
static void *acquire_slab_page(const struct v8m_slab_pool *pool)
{
	if (pool->use_numa_pool && atomic_load_explicit(&g_slab_numa_pool_ready,
							memory_order_acquire)) {
		uint32_t node = v8m_numa_current_node();
		if (node >= V8M_NUMA_MAX_NODES) {
			node = 0;
		}
		void *page = v8m_numa_pool_carve_slab(&g_slab_numa_pool, node);
		if (page != NULL) {
			return page;
		}
		/* Carve failed (page heap exhausted, etc.); fall
		 * through to direct alloc as a last resort. */
	}
	return v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
}

static void release_slab_page(const struct v8m_slab_pool *pool, void *page)
{
	if (pool->use_numa_pool && atomic_load_explicit(&g_slab_numa_pool_ready,
							memory_order_acquire)) {
		if (v8m_numa_pool_release_slab(&g_slab_numa_pool, page)) {
			return;
		}
		/* Not owned by the numa_pool — must have come from the
		 * direct path (allocated when the numa_pool was
		 * temporarily unavailable). Fall through to page-heap
		 * release. */
	}
	v8m_page_heap_free(page, V8M_PAGE_SIZE);
}
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
	pool->use_numa_pool = false;
	return pthread_mutex_init(&pool->lock, NULL);
}

void v8m_slab_pool_destroy(struct v8m_slab_pool *pool)
{
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		struct v8m_slab_pool_class *cls = &pool->classes[i];
		if (cls->current != NULL) {
			release_slab_page(pool, cls->current);
			cls->current = NULL;
		}
		struct v8m_page_meta *next = NULL;
		for (struct v8m_page_meta *page = cls->partials; page != NULL;
		     page = next) {
			next = page->next;
			release_slab_page(pool, page);
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
/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
static void *acquire_fresh_page(const struct v8m_slab_pool *pool,
				struct v8m_slab_pool_class *cls,
				uint32_t size_class, uint64_t owner_thread,
				uint8_t arena_id)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
	void *page = acquire_slab_page(pool);
	if (page == NULL) {
		return NULL;
	}
	slab_init_dispatch(page, size_class, owner_thread);
	((struct v8m_page_meta *)page)->arena_id = arena_id;
	cls->current = page;
	return slab_alloc_dispatch(cls->current);
}

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
void *v8m_slab_pool_alloc_arena(struct v8m_slab_pool *pool, uint32_t size_class,
				uint64_t owner_thread, uint8_t arena_id)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
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
		obj = acquire_fresh_page(pool, cls, size_class, owner_thread,
					 arena_id);
	}

	(void)pthread_mutex_unlock(&pool->lock);
	return obj;
}

void *v8m_slab_pool_alloc(struct v8m_slab_pool *pool, uint32_t size_class,
			  uint64_t owner_thread)
{
	return v8m_slab_pool_alloc_arena(pool, size_class, owner_thread, 0U);
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
		release_slab_page(pool, meta);
	} else if (was_full && cls->current != meta) {
		/* Full -> partial transition; the page wasn't in any
		 * list, so add it to partials. The `cls->current != meta`
		 * guard matters: a page can be `current` AND full (the
		 * try_current path leaves it as current until the next
		 * alloc walks it off), so a free that triggers
		 * full -> partial on the still-current page would
		 * otherwise insert a duplicate entry. The duplicate
		 * survives the eventual unmap (unlink_from_class only
		 * removes one occurrence), leaving partials holding a
		 * dangling pointer that crashes the next try_partials
		 * walk. Caught by MB-04 / MB-03 stress patterns at
		 * Small classes 30/31. */
		meta->next = cls->partials;
		cls->partials = meta;
	}
	/* Else: page was current or partial and stays where it is. */

	(void)pthread_mutex_unlock(&pool->lock);
	return became_empty;
}

static void accumulate_page(const struct v8m_page_meta *page,
			    struct v8m_slab_pool_aggregate_stats *out)
{
	out->pages_in_use++;
	out->slots_total += page->capacity;
	out->slots_used +=
	    atomic_load_explicit(&page->used_count, memory_order_relaxed);
}

void v8m_slab_pool_get_aggregate_stats(
    struct v8m_slab_pool *pool, struct v8m_slab_pool_aggregate_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->pages_in_use = 0;
	out->slots_total = 0;
	out->slots_used = 0;
	if (pool == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&pool->lock);
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		const struct v8m_slab_pool_class *cls = &pool->classes[i];
		if (cls->current != NULL) {
			accumulate_page(cls->current, out);
		}
		for (const struct v8m_page_meta *page = cls->partials;
		     page != NULL; page = page->next) {
			accumulate_page(page, out);
		}
	}
	(void)pthread_mutex_unlock(&pool->lock);
}
