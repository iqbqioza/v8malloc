/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-NUMA-node huge-page pool. Owns a list of `v8m_huge_slab`
 * descriptors per NUMA node; carves V8M_PAGE_SIZE-sized slab pages
 * from those huge pages on demand and returns them to the pool on
 * free. Each per-node bookkeeping block is `V8M_CACHELINE_ALIGNED`
 * so adjacent nodes' counters and lock cannot share a cache line —
 * a thread allocating from node 0 cannot bounce node 1's cache line.
 *
 * **v0 status**: this is the standalone primitive that closes the
 * cache-line padding row. The dispatcher does not yet route slab
 * allocations through this pool — that wiring lands when the slab
 * pool refactor consumes the per-NUMA shape. Tests exercise the
 * primitive in isolation against a synthetic huge-page source.
 */

#ifndef V8M_NUMA_POOL_H
#define V8M_NUMA_POOL_H

#include <pthread.h> /* IWYU pragma: keep — pthread_mutex_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_arch.h" /* V8M_CACHELINE_ALIGNED, V8M_HUGE_PAGE_SIZE */
#include "v8m_huge_slab.h"
#include "v8m_numa.h" /* V8M_NUMA_MAX_NODES */

/*
 * Per-node bookkeeping. Holds the node's mutex, the partial- and
 * full-list heads of `v8m_huge_slab` descriptors (each describing
 * one V8M_HUGE_PAGE_SIZE-sized huge page carved into
 * V8M_HUGE_SLABS_PER_HUGE slab pages), and per-node diagnostic
 * counters. The whole block is cache-line aligned so two threads
 * working on adjacent nodes don't false-share the line.
 *
 * `partials` / `fulls` are intrusive singly-linked lists via the
 * `v8m_huge_slab.next` field. Allocation pulls from `partials`
 * (first slab with a free slot); a slab that fills migrates to
 * `fulls`; a free that brings a `fulls` slab back below capacity
 * migrates it back to `partials`; a slab that empties is unlinked
 * and the underlying huge page is returned to the OS.
 */
struct V8M_CACHELINE_ALIGNED v8m_numa_pool_node {
	/* clang-tidy's IWYU rule prefers the deeper bits/pthreadtypes.h
	 * for `pthread_mutex_t`; pthread.h is the canonical provider. */
	pthread_mutex_t lock; /* NOLINT(misc-include-cleaner) */
	struct v8m_huge_slab *partials;
	struct v8m_huge_slab *fulls;
	uint32_t numa_node;
	uint64_t huge_pages_alive; /* count of huge pages currently held */
	uint64_t huge_pages_allocated; /* monotonic huge-page mmap calls */
	uint64_t huge_pages_released; /* monotonic huge-page munmap calls */
	uint64_t slab_carve_calls; /* monotonic slab carve successes */
	uint64_t slab_release_calls; /* monotonic slab release successes */
};

/*
 * One pool instance covers every NUMA node in the topology. The
 * outer struct is intentionally not cache-line padded itself — the
 * per-node array elements carry the alignment that matters.
 */
struct v8m_numa_pool {
	struct v8m_numa_pool_node nodes[V8M_NUMA_MAX_NODES];
};

/*
 * Initialize every per-node entry: zero the bookkeeping, init the
 * mutex, set `numa_node` to the slot index. Tolerates NULL.
 * Returns 0 on success or `-errno` from the first failing
 * pthread_mutex_init call (in which case all earlier-initialized
 * mutexes are destroyed before returning).
 */
int v8m_numa_pool_init(struct v8m_numa_pool *pool);

/*
 * Destroy every per-node entry: release every huge page held by
 * any node, destroy every per-node mutex, zero the descriptor.
 * Safe on a never-initialized pool. Tolerates NULL.
 */
void v8m_numa_pool_destroy(struct v8m_numa_pool *pool);

/*
 * Carve one V8M_PAGE_SIZE-aligned slab page from the pool's
 * `numa_node` partition. The pool walks the node's `partials`
 * list, picks the first slab with a free slot, and migrates the
 * slab to `fulls` if the carve filled it. If no partial slab has
 * room, mmaps a fresh huge page (via `v8m_page_heap_alloc` with
 * V8M_HUGE_PAGE_SIZE alignment), wraps it in a new
 * `v8m_huge_slab`, and serves the carve from there.
 *
 * Returns the slab pointer or NULL on:
 *   - out-of-range `numa_node`,
 *   - huge-page allocation failure (page heap exhausted).
 */
void *v8m_numa_pool_carve_slab(struct v8m_numa_pool *pool, uint32_t numa_node);

/*
 * Release `slab` back to the pool. The pool recovers the owning
 * huge page by masking `slab` to V8M_HUGE_PAGE_SIZE alignment,
 * walks every per-node list to find the matching descriptor (slow
 * — O(huge pages); a future cycle adds a hash table), clears the
 * bit, and migrates the descriptor list-state if needed (fulls →
 * partials when the carve count drops below capacity, partials →
 * unlinked + huge-page release when the descriptor empties).
 *
 * Returns true on a successful release; false if `slab` is not a
 * recognised carve from any node's pool.
 */
bool v8m_numa_pool_release_slab(struct v8m_numa_pool *pool, void *slab);

/*
 * Predicate: true iff `ptr` lies within any huge page currently
 * held by the pool. O(huge pages) — same complexity as release;
 * useful for tests + future fast-path-shortcut wiring.
 */
bool v8m_numa_pool_owns(const struct v8m_numa_pool *pool, const void *ptr);

#endif /* V8M_NUMA_POOL_H */
