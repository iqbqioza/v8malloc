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

/* Release the page directly to the underlying source — numa_pool
 * (when opted in AND ready) or the page heap. Drained-cache callers
 * fall through to this on overflow / sweep. */
static void release_slab_page_immediate(const struct v8m_slab_pool *pool,
					void *page)
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

/* Pop the most-recently-parked page from the drained cache and
 * return it as a "fresh" page. The kernel zero-fills on the next
 * fault (we MADV_DONTNEED'd the physical pages on push), so the
 * meta header the caller writes via slab_init_dispatch lands on
 * clean memory. Returns NULL when the cache is empty. */
static void *drained_pop(struct v8m_slab_pool *pool)
{
	if (pool->drained_count == 0U) {
		return NULL;
	}
	pool->drained_count--;
	void *page = pool->drained[pool->drained_count];
	pool->drained[pool->drained_count] = NULL;
	pool->drained_idle[pool->drained_count] = 0U;
	return page;
}

/* Push a page onto the drained cache, applying MADV_DONTNEED so the
 * kernel reclaims the physical frames immediately while the VMA
 * stays intact. Returns true on success; false (cache full) means
 * the caller must release the page through the underlying source.
 *
 * The slab metadata co-locates with the page's data area, so the
 * MADV_DONTNEED hits the meta header too — that's why we don't try
 * to "recover" a drained page's prior class. The acquire path
 * always re-runs slab init and stamps a fresh meta. */
static bool drained_push(struct v8m_slab_pool *pool, void *page)
{
	if (pool->drained_count >= V8M_SLAB_DRAINED_CAP) {
		return false;
	}
	v8m_page_heap_advise_dont_need(page, V8M_PAGE_SIZE);
	pool->drained[pool->drained_count] = page;
	pool->drained_idle[pool->drained_count] = 0U;
	pool->drained_count++;
	return true;
}

/* Allocate a fresh slab page. Tries the per-pool drained cache
 * first (LIFO so the most-recently-parked page reuses while still
 * potentially warm in the kernel's reclaim queue), falling through
 * to the global numa_pool (when this pool opted in AND the global
 * is ready) or directly to the page heap. Returns NULL on failure
 * of every source. */
static void *acquire_slab_page(struct v8m_slab_pool *pool)
{
	void *page = drained_pop(pool);
	if (page != NULL) {
		return page;
	}
	if (pool->use_numa_pool && atomic_load_explicit(&g_slab_numa_pool_ready,
							memory_order_acquire)) {
		uint32_t node = v8m_numa_current_node();
		if (node >= V8M_NUMA_MAX_NODES) {
			node = 0;
		}
		page = v8m_numa_pool_carve_slab(&g_slab_numa_pool, node);
		if (page != NULL) {
			return page;
		}
		/* Carve failed (page heap exhausted, etc.); fall
		 * through to direct alloc as a last resort. */
	}
	return v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
}

/* Park the page in the drained cache (MADV_DONTNEED + push). On
 * cache overflow, fall through to the immediate-release path. */
static void release_slab_page(struct v8m_slab_pool *pool, void *page)
{
	if (drained_push(pool, page)) {
		return;
	}
	release_slab_page_immediate(pool, page);
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
	for (uint32_t i = 0; i < V8M_SLAB_DRAINED_CAP; i++) {
		pool->drained[i] = NULL;
		pool->drained_idle[i] = 0U;
	}
	pool->drained_count = 0;
	return pthread_mutex_init(&pool->lock, NULL);
}

void v8m_slab_pool_destroy(struct v8m_slab_pool *pool)
{
	/* Release every live page directly to the underlying source —
	 * destroy is the terminal call so deferring any release would
	 * leak the underlying VMA. */
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		struct v8m_slab_pool_class *cls = &pool->classes[i];
		if (cls->current != NULL) {
			release_slab_page_immediate(pool, cls->current);
			cls->current = NULL;
		}
		struct v8m_page_meta *next = NULL;
		for (struct v8m_page_meta *page = cls->partials; page != NULL;
		     page = next) {
			next = page->next;
			release_slab_page_immediate(pool, page);
		}
		cls->partials = NULL;
	}
	/* Flush the drained cache so the parked pages do not survive
	 * the pool. */
	for (uint32_t i = 0; i < pool->drained_count; i++) {
		release_slab_page_immediate(pool, pool->drained[i]);
		pool->drained[i] = NULL;
	}
	pool->drained_count = 0;
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
 * Bounded-scan budget for the partials max-pick. A pure head-pop
 * is wrong because partial pages can accumulate frees after
 * insertion — a newer (head) entry can have lower `used_count`
 * than an older entry that took fewer frees. A pure linear scan
 * is correct but O(N partials per class). Compromise: walk the
 * first `V8M_PARTIALS_SCAN_BUDGET` entries and pick the max
 * `used_count` among them. Constant cost per refill, and since
 * the insert path always pushes at head with `used_count =
 * capacity - 1`, the recently-inserted entries clustered at the
 * head are where the max overwhelmingly lives — older entries
 * deeper in the list have had more time to lose `used_count` and
 * are by construction less utilized on average. The budget caps
 * the worst-case constant; tail entries that are somehow more
 * utilized (rare) are missed but the pool still serves the
 * request from a well-utilized page.
 */
#define V8M_PARTIALS_SCAN_BUDGET 4U

/*
 * Promote the most-utilized partial page (within the bounded scan
 * budget) into `current` and allocate from it. Picking the page
 * closest to full first concentrates allocations and lets less-
 * utilized pages drain to empty (and back to the page heap) faster
 * — fragmentation.md §4.3's `v8m_select_allocation_page`.
 *
 * Defensive fall-through: if the chosen page is somehow already
 * full (shouldn't happen for partials, but a corrupted heap or a
 * race with a future lock-free path could expose it), drop it and
 * scan again over the remaining list so the pool can still satisfy
 * the request.
 */
static void *try_partials(struct v8m_slab_pool_class *cls)
{
	while (cls->partials != NULL) {
		struct v8m_page_meta **best_link = &cls->partials;
		uint32_t best_used = 0;
		uint32_t scanned = 0;
		for (struct v8m_page_meta **link = &cls->partials;
		     *link != NULL && scanned < V8M_PARTIALS_SCAN_BUDGET;
		     link = &(*link)->next, scanned++) {
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

size_t v8m_slab_pool_sweep_idle(struct v8m_slab_pool *pool,
				uint32_t max_idle_ticks)
{
	if (pool == NULL) {
		return 0;
	}
	size_t released = 0;
	(void)pthread_mutex_lock(&pool->lock);
	/* Compact-on-release: walk the array, advance idle, evict
	 * matured entries and shift the tail down so the cache stays
	 * dense (drained_pop pulls from the top end). */
	uint32_t write = 0;
	for (uint32_t read = 0; read < pool->drained_count; read++) {
		uint32_t ticks = pool->drained_idle[read];
		if (ticks < UINT32_MAX) {
			ticks++;
		}
		if (ticks >= max_idle_ticks) {
			release_slab_page_immediate(pool, pool->drained[read]);
			released++;
			continue;
		}
		pool->drained[write] = pool->drained[read];
		pool->drained_idle[write] = ticks;
		write++;
	}
	for (uint32_t i = write; i < pool->drained_count; i++) {
		pool->drained[i] = NULL;
		pool->drained_idle[i] = 0U;
	}
	pool->drained_count = write;
	(void)pthread_mutex_unlock(&pool->lock);
	return released;
}

size_t v8m_slab_pool_purge_drained(struct v8m_slab_pool *pool)
{
	return v8m_slab_pool_sweep_idle(pool, 0U);
}

uint32_t v8m_slab_pool_drained_count(struct v8m_slab_pool *pool)
{
	if (pool == NULL) {
		return 0;
	}
	(void)pthread_mutex_lock(&pool->lock);
	uint32_t count = pool->drained_count;
	(void)pthread_mutex_unlock(&pool->lock);
	return count;
}
