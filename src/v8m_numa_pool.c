/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-NUMA-node huge-page pool — implementation. See the matching
 * header for the design notes and v0 status.
 */

#include "v8m_numa_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "v8m_arch.h" /* V8M_HUGE_PAGE_SIZE */
#include "v8m_huge_slab.h"
#include "v8m_numa.h" /* V8M_NUMA_MAX_NODES — surfaces through the header */
#include "v8m_page_heap.h" /* v8m_page_heap_alloc / _free for huge pages */

/*
 * Allocate / release a `v8m_huge_slab` descriptor via mmap. We avoid
 * calling malloc here because the slab pool path (the pool's
 * primary consumer) holds its own mutex while requesting a fresh
 * huge page; routing through libc / v8m malloc would re-enter the
 * same slab pool and deadlock. mmap returns kernel-page-aligned
 * pages and the descriptor is small (~40 bytes), so each
 * descriptor wastes about one OS page — acceptable given the rate
 * is one descriptor per huge page (roughly one per V8M_HUGE_PAGE_SIZE
 * bytes of slab capacity).
 */
static struct v8m_huge_slab *desc_alloc(void)
{
	void *raw =
	    mmap(NULL, sizeof(struct v8m_huge_slab), PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) {
		return NULL;
	}
	return raw;
}

static void desc_free(struct v8m_huge_slab *desc)
{
	if (desc == NULL) {
		return;
	}
	(void)munmap(desc, sizeof(*desc));
}

/*
 * Allocate a fresh huge page + its descriptor for `node`. Caller
 * must hold `node->lock`. On success the descriptor is linked into
 * `node->partials` and the huge-page counters are bumped.
 */
static struct v8m_huge_slab *add_huge_page(struct v8m_numa_pool_node *node)
{
	void *base =
	    v8m_page_heap_alloc(V8M_HUGE_PAGE_SIZE, V8M_HUGE_PAGE_SIZE);
	if (base == NULL) {
		return NULL;
	}
	struct v8m_huge_slab *slab = desc_alloc();
	if (slab == NULL) {
		v8m_page_heap_free(base, V8M_HUGE_PAGE_SIZE);
		return NULL;
	}
	v8m_huge_slab_init(slab, base, node->numa_node,
			   V8M_HUGE_SLABS_PER_HUGE);
	slab->next = node->partials;
	node->partials = slab;
	node->huge_pages_alive++;
	node->huge_pages_allocated++;
	return slab;
}

/* Unlink `slab` from `head` if present. Returns true on a hit. */
static bool list_unlink(struct v8m_huge_slab **head,
			const struct v8m_huge_slab *slab)
{
	while (*head != NULL) {
		if (*head == slab) {
			*head = (*head)->next;
			return true;
		}
		head = &(*head)->next;
	}
	return false;
}

int v8m_numa_pool_init(struct v8m_numa_pool *pool)
{
	if (pool == NULL) {
		return -EINVAL;
	}
	(void)memset(pool, 0, sizeof(*pool));
	for (uint32_t i = 0; i < V8M_NUMA_MAX_NODES; i++) {
		int mutex_rc = pthread_mutex_init(&pool->nodes[i].lock, NULL);
		if (mutex_rc != 0) {
			for (uint32_t j = 0; j < i; j++) {
				(void)pthread_mutex_destroy(
				    &pool->nodes[j].lock);
			}
			return -mutex_rc;
		}
		pool->nodes[i].numa_node = i;
	}
	return 0;
}

static void destroy_list(struct v8m_huge_slab *head)
{
	while (head != NULL) {
		struct v8m_huge_slab *next = head->next;
		if (head->base != NULL) {
			v8m_page_heap_free(head->base, V8M_HUGE_PAGE_SIZE);
		}
		desc_free(head);
		head = next;
	}
}

void v8m_numa_pool_destroy(struct v8m_numa_pool *pool)
{
	if (pool == NULL) {
		return;
	}
	for (uint32_t i = 0; i < V8M_NUMA_MAX_NODES; i++) {
		struct v8m_numa_pool_node *node = &pool->nodes[i];
		(void)pthread_mutex_lock(&node->lock);
		destroy_list(node->partials);
		destroy_list(node->fulls);
		node->partials = NULL;
		node->fulls = NULL;
		(void)pthread_mutex_unlock(&node->lock);
		(void)pthread_mutex_destroy(&node->lock);
	}
	(void)memset(pool, 0, sizeof(*pool));
}

void *v8m_numa_pool_carve_slab(struct v8m_numa_pool *pool, uint32_t numa_node)
{
	if (pool == NULL || numa_node >= V8M_NUMA_MAX_NODES) {
		return NULL;
	}
	struct v8m_numa_pool_node *node = &pool->nodes[numa_node];

	(void)pthread_mutex_lock(&node->lock);
	struct v8m_huge_slab *slab = node->partials;
	if (__builtin_expect(slab == NULL, 0)) {
		slab = add_huge_page(node);
		if (__builtin_expect(slab == NULL, 0)) {
			(void)pthread_mutex_unlock(&node->lock);
			return NULL;
		}
	}
	void *carved = v8m_huge_slab_alloc(slab);
	/* Picking from the head guarantees a non-empty descriptor (we
	 * just verified the partials head exists or freshly added one).
	 * The carve cannot fail unless the descriptor was inconsistent. */
	if (__builtin_expect(carved == NULL, 0)) {
		(void)pthread_mutex_unlock(&node->lock);
		return NULL;
	}
	node->slab_carve_calls++;
	if (v8m_huge_slab_is_full(slab)) {
		(void)list_unlink(&node->partials, slab);
		slab->next = node->fulls;
		node->fulls = slab;
	}
	(void)pthread_mutex_unlock(&node->lock);
	return carved;
}

/*
 * Find the descriptor whose huge page contains `slab` within
 * `node`'s lists. Caller must hold `node->lock`. Returns
 * (descriptor, list-head pointer) on hit; NULL on miss. The
 * out-param `out_list` lets the caller migrate / unlink without a
 * second walk.
 */
static struct v8m_huge_slab *find_owning(struct v8m_numa_pool_node *node,
					 const void *slab,
					 struct v8m_huge_slab ***out_list)
{
	uintptr_t huge_base =
	    (uintptr_t)slab & ~((uintptr_t)V8M_HUGE_PAGE_SIZE - 1U);
	struct v8m_huge_slab **lists[2] = {&node->partials, &node->fulls};
	for (size_t i = 0; i < 2; i++) {
		struct v8m_huge_slab *cur = *lists[i];
		while (cur != NULL) {
			if ((uintptr_t)cur->base == huge_base) {
				*out_list = lists[i];
				return cur;
			}
			cur = cur->next;
		}
	}
	return NULL;
}

/* `slab` is non-const at the API boundary because release semantics
 * imply ownership transfer; the function body itself never writes
 * through the pointer. */
/* cppcheck-suppress constParameterPointer */
bool v8m_numa_pool_release_slab(struct v8m_numa_pool *pool, void *slab)
{
	if (pool == NULL || slab == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < V8M_NUMA_MAX_NODES; i++) {
		struct v8m_numa_pool_node *node = &pool->nodes[i];
		(void)pthread_mutex_lock(&node->lock);
		struct v8m_huge_slab **list_head = NULL;
		struct v8m_huge_slab *desc =
		    find_owning(node, slab, &list_head);
		if (desc == NULL) {
			(void)pthread_mutex_unlock(&node->lock);
			continue;
		}
		bool was_full = v8m_huge_slab_is_full(desc);
		bool released = v8m_huge_slab_free(desc, slab);
		if (!released) {
			(void)pthread_mutex_unlock(&node->lock);
			return false;
		}
		node->slab_release_calls++;
		/* Migration: full → partials when room opened up;
		 * partials → unlinked when the descriptor emptied. */
		if (was_full) {
			(void)list_unlink(&node->fulls, desc);
			desc->next = node->partials;
			node->partials = desc;
		}
		if (v8m_huge_slab_is_empty(desc)) {
			(void)list_unlink(&node->partials, desc);
			void *base = desc->base;
			desc_free(desc);
			node->huge_pages_alive--;
			node->huge_pages_released++;
			(void)pthread_mutex_unlock(&node->lock);
			v8m_page_heap_free(base, V8M_HUGE_PAGE_SIZE);
			return true;
		}
		(void)pthread_mutex_unlock(&node->lock);
		return true;
	}
	return false;
}

bool v8m_numa_pool_owns(const struct v8m_numa_pool *pool, const void *ptr)
{
	if (pool == NULL || ptr == NULL) {
		return false;
	}
	uintptr_t addr = (uintptr_t)ptr;
	for (uint32_t i = 0; i < V8M_NUMA_MAX_NODES; i++) {
		const struct v8m_numa_pool_node *node = &pool->nodes[i];
		/* Lock-free walk: the read race vs. concurrent carve /
		 * release leaves the predicate momentarily inaccurate;
		 * tests that need a strict snapshot quiesce other
		 * threads first. */
		const struct v8m_huge_slab *cur = node->partials;
		while (cur != NULL) {
			uintptr_t base = (uintptr_t)cur->base;
			if (addr >= base && addr < base + V8M_HUGE_PAGE_SIZE) {
				return true;
			}
			cur = cur->next;
		}
		cur = node->fulls;
		while (cur != NULL) {
			uintptr_t base = (uintptr_t)cur->base;
			if (addr >= base && addr < base + V8M_HUGE_PAGE_SIZE) {
				return true;
			}
			cur = cur->next;
		}
	}
	return false;
}
