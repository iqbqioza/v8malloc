/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-core L2 cache implementation. See v8m_core_cache.h for the
 * design notes.
 */

#include "v8m_core_cache.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "v8m_numa.h" /* V8M_NUMA_MAX_CPUS, v8m_numa_current_cpu */
#include "v8m_size_class.h" /* V8M_NUM_SIZE_CLASSES */

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
