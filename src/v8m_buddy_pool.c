/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Buddy pool implementation. A fixed-cap array of arenas; alloc
 * tries each in-use arena, then lazily acquires a fresh one. Free
 * scans the array to locate the owning arena, recovers the size
 * from the bitmaps via v8m_buddy_block_size, delegates to
 * v8m_buddy_free, and reclaims the arena if it drained to empty.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_buddy.h"
#include "v8m_buddy_pool.h"
#include "v8m_page_heap.h"

int v8m_buddy_pool_init(struct v8m_buddy_pool *pool)
{
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		pool->arenas[i].in_use = false;
		pool->arenas[i].drained = false;
		pool->arenas[i].idle_ticks = 0;
	}
	return pthread_mutex_init(&pool->lock, NULL);
}

void v8m_buddy_pool_destroy(struct v8m_buddy_pool *pool)
{
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		if (pool->arenas[i].in_use) {
			v8m_page_heap_free(pool->arenas[i].buddy.arena_base,
					   V8M_BUDDY_MAX_BLOCK);
			pool->arenas[i].in_use = false;
		}
	}
	(void)pthread_mutex_destroy(&pool->lock);
}

/*
 * Locate the arena whose [arena_base, arena_base + arena_size)
 * range contains `ptr`. Returns NULL if no in-use arena owns it.
 */
static struct v8m_buddy_pool_arena *
find_owning_arena(struct v8m_buddy_pool *pool, const void *ptr)
{
	uintptr_t addr = (uintptr_t)ptr;
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		struct v8m_buddy_pool_arena *slot = &pool->arenas[i];
		if (!slot->in_use) {
			continue;
		}
		uintptr_t base = (uintptr_t)slot->buddy.arena_base;
		if (addr >= base && addr < base + slot->buddy.arena_size) {
			return slot;
		}
	}
	return NULL;
}

/*
 * Try to satisfy `size` from one of the in-use arenas. Returns the
 * pointer or NULL if every arena is too full. Drained arenas
 * (post-MADV_DONTNEED) are valid alloc targets — the buddy
 * bookkeeping stayed intact, so a successful alloc just clears
 * the drained flag and the kernel re-faults zero pages on the
 * first touch. The drain-then-revive cycle is cheaper than
 * mmap/munmap when allocation patterns oscillate.
 */
static void *try_existing_arenas(struct v8m_buddy_pool *pool, size_t size)
{
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		struct v8m_buddy_pool_arena *slot = &pool->arenas[i];
		if (!slot->in_use) {
			continue;
		}
		void *obj = v8m_buddy_alloc(&slot->buddy, size);
		if (obj != NULL) {
			slot->drained = false;
			slot->idle_ticks = 0;
			return obj;
		}
	}
	return NULL;
}

/*
 * Acquire a fresh 256 KiB-aligned arena from the page heap, install
 * it in the first unused slot, and allocate `size` from it. Returns
 * NULL on page-heap exhaustion or when the arena cap is reached. If
 * the freshly-acquired arena somehow can't satisfy `size` (e.g., a
 * future caller passes size > V8M_BUDDY_MAX_BLOCK that slipped past
 * the entry check), the arena is returned to the page heap before
 * the function bails out.
 */
static void *acquire_fresh_arena(struct v8m_buddy_pool *pool, size_t size)
{
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		struct v8m_buddy_pool_arena *slot = &pool->arenas[i];
		if (slot->in_use) {
			continue;
		}
		void *arena = v8m_page_heap_alloc(V8M_BUDDY_MAX_BLOCK,
						  V8M_BUDDY_MAX_BLOCK);
		if (arena == NULL) {
			return NULL;
		}
		v8m_buddy_init(&slot->buddy, arena);
		slot->in_use = true;
		slot->drained = false;
		slot->idle_ticks = 0;
		void *obj = v8m_buddy_alloc(&slot->buddy, size);
		if (obj == NULL) {
			v8m_page_heap_free(arena, V8M_BUDDY_MAX_BLOCK);
			slot->in_use = false;
			return NULL;
		}
		return obj;
	}
	return NULL; /* arena cap reached */
}

void *v8m_buddy_pool_alloc(struct v8m_buddy_pool *pool, size_t size)
{
	if (size == 0 || size > V8M_BUDDY_MAX_BLOCK) {
		return NULL;
	}

	(void)pthread_mutex_lock(&pool->lock);

	void *obj = try_existing_arenas(pool, size);
	if (obj == NULL) {
		obj = acquire_fresh_arena(pool, size);
	}

	(void)pthread_mutex_unlock(&pool->lock);
	return obj;
}

bool v8m_buddy_pool_free(struct v8m_buddy_pool *pool, void *ptr)
{
	if (ptr == NULL) {
		return false;
	}

	(void)pthread_mutex_lock(&pool->lock);

	struct v8m_buddy_pool_arena *slot = find_owning_arena(pool, ptr);
	if (slot == NULL) {
		(void)pthread_mutex_unlock(&pool->lock);
		return false;
	}

	size_t size = v8m_buddy_block_size(&slot->buddy, ptr);
	if (size == 0) {
		(void)pthread_mutex_unlock(&pool->lock);
		return false;
	}

	v8m_buddy_free(&slot->buddy, ptr, size);

	if (v8m_buddy_is_empty(&slot->buddy)) {
		/* Defer the munmap: keep the VMA + buddy bookkeeping
		 * intact, but ask the kernel to reclaim the physical
		 * frames immediately. A revival alloc within the next
		 * few bg-purge ticks reuses the slot without an
		 * mmap/munmap round trip; otherwise the bg purge sweep
		 * fully releases it (see v8m_buddy_pool_sweep_idle). */
		slot->drained = true;
		slot->idle_ticks = 0;
		v8m_page_heap_advise_dont_need(slot->buddy.arena_base,
					       V8M_BUDDY_MAX_BLOCK);
	}

	(void)pthread_mutex_unlock(&pool->lock);
	return true;
}

size_t v8m_buddy_pool_sweep_idle(struct v8m_buddy_pool *pool,
				 uint32_t max_idle_ticks)
{
	if (pool == NULL) {
		return 0;
	}
	size_t released = 0;
	(void)pthread_mutex_lock(&pool->lock);
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		struct v8m_buddy_pool_arena *slot = &pool->arenas[i];
		if (!slot->in_use || !slot->drained) {
			continue;
		}
		if (slot->idle_ticks < UINT32_MAX) {
			slot->idle_ticks++;
		}
		if (slot->idle_ticks >= max_idle_ticks) {
			v8m_page_heap_free(slot->buddy.arena_base,
					   V8M_BUDDY_MAX_BLOCK);
			slot->in_use = false;
			slot->drained = false;
			slot->idle_ticks = 0;
			released++;
		}
	}
	(void)pthread_mutex_unlock(&pool->lock);
	return released;
}

void v8m_buddy_pool_get_arena_stats(struct v8m_buddy_pool *pool,
				    struct v8m_buddy_pool_arena_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->live = 0;
	out->drained = 0;
	out->total_in_use = 0;
	if (pool == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&pool->lock);
	for (uint32_t i = 0; i < V8M_BUDDY_POOL_MAX_ARENAS; i++) {
		const struct v8m_buddy_pool_arena *slot = &pool->arenas[i];
		if (!slot->in_use) {
			continue;
		}
		out->total_in_use++;
		if (slot->drained) {
			out->drained++;
		} else {
			out->live++;
		}
	}
	(void)pthread_mutex_unlock(&pool->lock);
}

size_t v8m_buddy_pool_block_size(struct v8m_buddy_pool *pool, const void *ptr)
{
	if (ptr == NULL) {
		return 0;
	}
	(void)pthread_mutex_lock(&pool->lock);
	struct v8m_buddy_pool_arena *slot = find_owning_arena(pool, ptr);
	size_t size =
	    (slot != NULL) ? v8m_buddy_block_size(&slot->buddy, ptr) : 0;
	(void)pthread_mutex_unlock(&pool->lock);
	return size;
}
