/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-core L2 cache implementation. See v8m_core_cache.h for the
 * design notes.
 */

#include "v8m_core_cache.h"

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "v8m_arch.h" /* V8M_CACHE_LINE_SIZE for the static_assert */
#include "v8m_numa.h" /* V8M_NUMA_MAX_CPUS, v8m_numa_current_cpu */
#include "v8m_size_class.h" /* V8M_NUM_SIZE_CLASSES */

/*
 * False-sharing audit (TODO P1 row 137). The core-cache struct
 * carries V8M_CACHELINE_ALIGNED, but that only aligns the struct
 * BASE — the C standard does not promise the struct's `sizeof`
 * is a multiple of the alignment. Without that guarantee,
 * neighbouring entries in `g_caches[V8M_NUMA_MAX_CPUS]` could
 * share a cache line at the boundary, which would let a
 * cross-CPU push/pop on cache N+1 invalidate a line cache N
 * just touched. The static_assert below makes the constraint
 * a build-time invariant: a future field add that bumps the
 * struct past the next cache-line boundary fails the build
 * until the layout is re-padded.
 */
static_assert(sizeof(struct v8m_core_cache) % V8M_CACHE_LINE_SIZE == 0U,
	      "v8m_core_cache must be cache-line-multiple-sized so "
	      "neighbouring entries in g_caches do not share a line");

/*
 * Static BSS table. Lazy physical backing: every entry is zero
 * (NULL stack head) until first touched. No init function needed —
 * BSS is zero-initialized by the loader.
 */
static struct v8m_core_cache g_caches[V8M_NUMA_MAX_CPUS];

struct v8m_core_cache *v8m_core_cache_for_cpu(uint32_t cpu_id)
{
	if (cpu_id >= V8M_NUMA_MAX_CPUS) {
		return NULL;
	}
	return &g_caches[cpu_id];
}

struct v8m_core_cache *v8m_core_cache_for_current_cpu(void)
{
	return v8m_core_cache_for_cpu(v8m_numa_current_cpu());
}

bool v8m_core_cache_push(struct v8m_core_cache *cache, uint32_t cls, void *node)
{
	if (cache == NULL || node == NULL || cls >= V8M_NUM_SIZE_CLASSES) {
		return false;
	}
	v8m_tagged_ptr old_head =
	    atomic_load_explicit(&cache->stacks[cls], memory_order_acquire);
	for (;;) {
		void *old_ptr = v8m_tagptr_ptr(old_head);
		/* Stamp the previous head into node's first 8 bytes
		 * so the consumer can chain through. The cast keeps
		 * clang-tidy's multi-level-pointer rule happy
		 * without changing the store. */
		(void)memcpy(node, (const void *)&old_ptr, sizeof(old_ptr));
		uint16_t new_tag = (uint16_t)(v8m_tagptr_tag(old_head) + 1U);
		v8m_tagged_ptr new_head = v8m_tagptr_make(node, new_tag);
		if (atomic_compare_exchange_weak_explicit(
			&cache->stacks[cls], &old_head, new_head,
			memory_order_release, memory_order_acquire)) {
			return true;
		}
	}
}

void *v8m_core_cache_pop(struct v8m_core_cache *cache, uint32_t cls)
{
	if (cache == NULL || cls >= V8M_NUM_SIZE_CLASSES) {
		return NULL;
	}
	v8m_tagged_ptr old_head =
	    atomic_load_explicit(&cache->stacks[cls], memory_order_acquire);
	for (;;) {
		void *node = v8m_tagptr_ptr(old_head);
		if (node == NULL) {
			return NULL;
		}
		void *next = NULL;
		(void)memcpy((void *)&next, node, sizeof(next));
		uint16_t new_tag = (uint16_t)(v8m_tagptr_tag(old_head) + 1U);
		v8m_tagged_ptr new_head = v8m_tagptr_make(next, new_tag);
		if (atomic_compare_exchange_weak_explicit(
			&cache->stacks[cls], &old_head, new_head,
			memory_order_acquire, memory_order_acquire)) {
			return node;
		}
	}
}

bool v8m_core_cache_push_batch(struct v8m_core_cache *cache, uint32_t cls,
			       void *head, void *tail)
{
	if (cache == NULL || head == NULL || tail == NULL ||
	    cls >= V8M_NUM_SIZE_CLASSES) {
		return false;
	}
	v8m_tagged_ptr old_head =
	    atomic_load_explicit(&cache->stacks[cls], memory_order_acquire);
	for (;;) {
		void *old_ptr = v8m_tagptr_ptr(old_head);
		/* Link the tail of our chain to the previous stack
		 * head. Same store-into-our-own-node trick as the
		 * single push, just on the chain's tail node. */
		(void)memcpy(tail, (const void *)&old_ptr, sizeof(old_ptr));
		uint16_t new_tag = (uint16_t)(v8m_tagptr_tag(old_head) + 1U);
		v8m_tagged_ptr new_head = v8m_tagptr_make(head, new_tag);
		if (atomic_compare_exchange_weak_explicit(
			&cache->stacks[cls], &old_head, new_head,
			memory_order_release, memory_order_acquire)) {
			return true;
		}
	}
}

size_t v8m_core_cache_pop_batch(struct v8m_core_cache *cache, uint32_t cls,
				size_t max, void **out_head, void **out_tail)
{
	if (cache == NULL || cls >= V8M_NUM_SIZE_CLASSES || max == 0U ||
	    out_head == NULL || out_tail == NULL) {
		if (out_head != NULL) {
			*out_head = NULL;
		}
		if (out_tail != NULL) {
			*out_tail = NULL;
		}
		return 0;
	}
	void *first = v8m_core_cache_pop(cache, cls);
	if (first == NULL) {
		*out_head = NULL;
		*out_tail = NULL;
		return 0;
	}
	void *tail = first;
	size_t count = 1;
	while (count < max) {
		void *node = v8m_core_cache_pop(cache, cls);
		if (node == NULL) {
			break;
		}
		/* Chain `node` after `tail` so the caller receives a
		 * forward-linked list head→...→tail with NULL after
		 * tail. Each pop already cleared `node`'s next slot
		 * (no — pop reads next but does not clear it; we must
		 * write the chain link explicitly). */
		(void)memcpy(tail, (const void *)&node, sizeof(node));
		tail = node;
		count++;
	}
	void *terminator = NULL;
	(void)memcpy(tail, (const void *)&terminator, sizeof(terminator));
	*out_head = first;
	*out_tail = tail;
	return count;
}
