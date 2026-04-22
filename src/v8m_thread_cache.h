/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Thread cache (L1) — per-thread allocator state. Foundation cycle:
 * this header / TU lands the `__thread` cache pointer, the lazy
 * first-touch initializer, and the `pthread_key_create` destructor
 * that reclaims the cache when a thread exits (architecture.md §2.1
 * + thread-cache.md §2.1). The fast-path routing (TLS load → bin
 * pop, page mask → magic check → bin push, bin overflow → batch
 * flush) lands in subsequent cycles; today the cache is allocated
 * and torn down correctly but is otherwise inert — v8m_dispatch
 * still serves every alloc/free directly from the slab / buddy /
 * Large backends.
 *
 * Concurrency model: each thread sees a private `t_cache` pointer.
 * No locks are taken on the get-or-create path; the lazy
 * initializer reads the TLS slot, allocates if NULL, registers the
 * pointer with the pthread_key so the runtime calls our destructor
 * on thread exit, and stores the pointer back in TLS. The remote
 * MPSC queue (already implemented in v8m_remote_free.h) is the
 * exception — peer threads push freed objects into our remote
 * queue, the owner thread drains it on its next slow path.
 */

#ifndef V8M_THREAD_CACHE_H
#define V8M_THREAD_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_remote_free.h" /* v8m_mpsc_queue */
#include "v8m_size_class.h" /* V8M_MEDIUM_FIRST_CLASS */

/*
 * Per-class bin-capacity bounds (thread-cache.md §5.2). Adaptive
 * tuning of the per-class capacity is a separate cycle; today every
 * class starts at V8M_BIN_CAPACITY_DEFAULT and stays there. The
 * MIN / MAX bounds are the clamp the future EMA controller will
 * enforce — defining them now pins the contract so the controller
 * cycle does not have to plumb them through after the fact.
 */
#define V8M_BIN_CAPACITY_MIN 16U
#define V8M_BIN_CAPACITY_DEFAULT 64U
#define V8M_BIN_CAPACITY_MAX 256U

/*
 * Forward decl — the slab-class fast path needs to flush back to
 * the slab pool on overflow / module shutdown, but the cache header
 * does not pull v8m_slab_pool.h in. The .c file's include set
 * carries it.
 */
struct v8m_slab_pool;

/*
 * Per-thread allocator state. Foundation-cycle layout: only the
 * fields the destructor + remote-free queue need today. Future
 * cycles grow this struct with bin_heads[] / bin_count[] /
 * bin_capacity[] / gc counters; consumers should treat the layout
 * as internal and only access via the helpers below.
 */
struct v8m_thread_cache {
	/*
	 * Cross-thread free queue (thread-cache.md §2.3). Peers push
	 * freed objects here when they observe `meta->owner_thread`
	 * pointing at this cache; the owner drains on its next slow
	 * path. Lives at the head of the struct so future hot-path
	 * reads land in the first cache line.
	 *
	 * v0 leaves the queue dormant — slab pages are pool-owned
	 * (not thread-owned), so a free always lands in the freeing
	 * thread's local TLC bin regardless of which thread allocated
	 * the object. The queue + drain plumbing land with the
	 * thread-owned-slab refactor.
	 */
	struct v8m_mpsc_queue remote;
	/*
	 * 1 once the cache has been wired up (allocated, registered
	 * with the pthread_key, and the TLS slot stored).
	 */
	uint32_t initialized;
	/*
	 * Per-slab-class free-list head. Each entry points at the head
	 * of an intrusive singly-linked list whose nodes are the
	 * cached free slots — the first 8 bytes of each cached object
	 * carry the `next` pointer. Indexed by size class id; only
	 * Tiny + Small classes (< V8M_MEDIUM_FIRST_CLASS) are cached
	 * because Medium / Large / Huge requests are rare enough that
	 * the slab/buddy/large slow paths dominate their cost anyway.
	 */
	void *bin_heads[V8M_MEDIUM_FIRST_CLASS];
	/*
	 * Object count per bin. Used to short-circuit the overflow
	 * check; alloc decrements on pop, free increments on push,
	 * batch flush deducts the flushed count.
	 */
	uint16_t bin_count[V8M_MEDIUM_FIRST_CLASS];
	/*
	 * Cap per bin. Frees beyond the cap trigger a batch flush
	 * (half the bin) back to the slab pool. v0 ships every class
	 * at V8M_BIN_CAPACITY_DEFAULT; the future adaptive controller
	 * (thread-cache.md §5.2) tunes per-class values within the
	 * [V8M_BIN_CAPACITY_MIN, V8M_BIN_CAPACITY_MAX] band.
	 */
	uint16_t bin_capacity[V8M_MEDIUM_FIRST_CLASS];
};

/*
 * Get the calling thread's cache pointer, allocating + registering
 * one if this is the thread's first call. Returns NULL only on
 * allocation failure (in which case the caller is expected to fall
 * back to the slow path). Thread-safe by construction — every
 * thread reads / writes its own TLS slot.
 *
 * Callable before the public API is fully ready (the cache itself
 * is allocated through bootstrap until dispatch is READY).
 */
struct v8m_thread_cache *v8m_thread_cache_get_or_create(void);

/*
 * Read-only accessor — returns the calling thread's cache pointer
 * if one already exists, else NULL. Does not allocate. Safe to
 * call from any context, including signal handlers (no syscalls,
 * no locks, no allocations).
 */
struct v8m_thread_cache *v8m_thread_cache_peek(void);

/*
 * Module init — wires up the pthread_key whose destructor reclaims
 * each thread's cache on thread exit. Called exactly once from the
 * library constructor, after dispatch is READY. Returns 0 on
 * success or the pthread_key_create errno on failure. Failure is
 * non-fatal at the caller level: without the destructor, a
 * thread's cache leaks at thread exit but the allocator stays
 * usable.
 */
int v8m_thread_cache_module_init(void);

/*
 * Module shutdown — clears the calling thread's TLS slot, tears
 * down the pthread_key, and (best-effort) releases the calling
 * thread's cache. Called from the library destructor. Other
 * threads' caches are not touched here; they're either already
 * gone (joined / exited) or will leak at process exit alongside
 * every other unmunmapped page. Safe to call when init failed —
 * acts as a no-op in that case.
 */
void v8m_thread_cache_module_shutdown(void);

/*
 * Diagnostic: cumulative count of caches the pthread_key
 * destructor has reclaimed since process start. Tests use this to
 * verify the destructor fires on thread exit without poking at
 * allocator internals; production callers shouldn't have a use
 * for it. Monotonic; survives module re-init.
 */
uint64_t v8m_thread_cache_destructor_calls(void);

/*
 * Pop one cached object of size class `cls` from this cache.
 * Returns NULL if the bin is empty (caller falls through to the
 * slow path / slab pool). `cls` must be < V8M_MEDIUM_FIRST_CLASS;
 * out-of-range classes return NULL. Single-threaded (called only
 * by the owner thread).
 */
void *v8m_thread_cache_alloc(struct v8m_thread_cache *cache, uint32_t cls);

/*
 * Push one freed object onto the bin for size class `cls`. Returns
 * `true` when the push left the bin at-or-above its capacity
 * (signalling the caller to invoke v8m_thread_cache_flush_half).
 * Returns `false` on the common "still has room" path. The object's
 * first 8 bytes are overwritten with the bin's previous head — the
 * caller has just relinquished `obj`, so this is safe.
 */
bool v8m_thread_cache_free(struct v8m_thread_cache *cache, uint32_t cls,
			   void *obj);

/*
 * Pop half the bin (rounded up — at least one object) and free
 * each via `v8m_slab_pool_free`. Used by the dispatcher's free
 * fast path when v8m_thread_cache_free reports overflow. Returns
 * the number of objects flushed (0 if the bin was empty). Walking
 * the popped chain reads each node's `next` pointer before the
 * slab_pool_free call zeroes the meta header, so the iteration is
 * forward-correct without per-node bookkeeping.
 */
size_t v8m_thread_cache_flush_half(struct v8m_thread_cache *cache,
				   struct v8m_slab_pool *pool, uint32_t cls);

/*
 * Drain every bin to the slab pool. Called from the module
 * shutdown path so a thread that exits during library teardown
 * does not leak its cached objects (the pthread_key destructor
 * runs after the slab pool is gone, so it cannot flush). Returns
 * the total number of objects freed across all bins.
 */
size_t v8m_thread_cache_drain_all(struct v8m_thread_cache *cache,
				  struct v8m_slab_pool *pool);

/*
 * Hook invoked by the pthread_key destructor to drain a cache's
 * bins back to the slab pool before the cache struct itself is
 * freed. The thread-cache module does not itself know about the
 * dispatcher singleton, so v8m_api.c installs a thin wrapper at
 * constructor time that routes the drain to `g_dispatch.slab`.
 * Without this hook the cached free slots would be lost when the
 * cache is freed (the slab pages would still consider them
 * allocated), leaking those slots until the surrounding pages
 * happened to drain empty by other means.
 */
typedef void (*v8m_thread_cache_drain_hook)(struct v8m_thread_cache *);
void v8m_thread_cache_set_drain_hook(v8m_thread_cache_drain_hook hook);

#endif /* V8M_THREAD_CACHE_H */
