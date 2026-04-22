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
 * Per-class bin-capacity bounds (thread-cache.md §5.2). The cache
 * starts every class at V8M_BIN_CAPACITY_DEFAULT; the per-class
 * EMA controller (`v8m_thread_cache_gc_tick`) recomputes capacity
 * from observed demand each GC interval and clamps the result into
 * the [MIN, MAX] band.
 */
#define V8M_BIN_CAPACITY_MIN 16U
#define V8M_BIN_CAPACITY_DEFAULT 64U
#define V8M_BIN_CAPACITY_MAX 256U

/*
 * GC interval, in TLC operations (allocs + frees combined). The
 * controller recomputes per-class capacities every time the
 * counter crosses this threshold. Power of two so the trigger
 * check collapses to a single decrement + branch on the alloc
 * fast path. Spec calls for ~1000; 1024 is the closest power of
 * two.
 */
#define V8M_TLC_GC_INTERVAL 1024U

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
	 * (half the bin) back to the slab pool. Every class starts
	 * at V8M_BIN_CAPACITY_DEFAULT; the adaptive controller
	 * (`v8m_thread_cache_gc_tick`, thread-cache.md §5.2) tunes
	 * per-class values within the [V8M_BIN_CAPACITY_MIN,
	 * V8M_BIN_CAPACITY_MAX] band each GC interval.
	 */
	uint16_t bin_capacity[V8M_MEDIUM_FIRST_CLASS];
	/*
	 * Per-class allocation / free counters since the last GC
	 * tick. The controller computes demand = allocs - frees
	 * (clamped to ≥ 0) for each class and folds it into the
	 * EMA. Reset to zero each GC tick.
	 */
	uint16_t alloc_count_per_class[V8M_MEDIUM_FIRST_CLASS];
	uint16_t free_count_per_class[V8M_MEDIUM_FIRST_CLASS];
	/*
	 * Per-class EMA of demand (smoothed across GC ticks with
	 * α = 0.25 — i.e. ema_new = (3 × ema_old + demand) / 4).
	 * The controller derives bin_capacity from this each tick.
	 */
	uint16_t ema_demand[V8M_MEDIUM_FIRST_CLASS];
	/*
	 * Down-counter that triggers the GC tick on alloc / free
	 * fast paths. Initialized to V8M_TLC_GC_INTERVAL on cache
	 * create; each alloc / free decrements; a hit at zero
	 * fires `v8m_thread_cache_gc_tick` and resets the counter.
	 * A countdown is cheaper on the hot path than the alternative
	 * "modulo of running total" formulation.
	 */
	uint32_t gc_countdown;
	/*
	 * Number of GC ticks the controller has run on this cache.
	 * Diagnostic; tests use it to confirm a tick fired without
	 * having to inspect the per-class EMA directly.
	 */
	uint32_t gc_generation;
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
 * Drain the cross-thread MPSC remote-free queue and push each
 * drained slot onto the matching local bin (recovering the size
 * class from the page meta of each node). Returns the number of
 * nodes drained. Called on the dispatcher's TLC slow path —
 * before falling through to the slab pool — so a follow-up alloc
 * can satisfy from drained slots without a pool-mutex round trip.
 *
 * Slots whose meta is invalid or whose class is out of the
 * Tiny/Small range are skipped (they should not be in the queue —
 * only slab-class objects ever get pushed there — but the drain
 * tolerates them defensively rather than aborting). Overflow
 * during the local push is intentionally NOT flushed: drain runs
 * on the cold slow path that already pays the slab-pool latency
 * one allocation later, so the simpler "just push" semantics keep
 * the drain itself fast.
 *
 * v0 leaves the remote queue dormant — slab pages are pool-owned
 * (not thread-owned), so a free always lands in the freeing
 * thread's local bin and never in another cache's remote queue.
 * This drain is therefore a no-op in v0; it lights up when the
 * thread-owned-slab refactor wires owner-thread routing.
 */
size_t v8m_thread_cache_drain_remote(struct v8m_thread_cache *cache);

/*
 * Adaptive bin-capacity GC tick. Walks every Tiny/Small class,
 * folds the per-class allocation / free demand into the EMA
 * (α = 0.25), and recomputes bin_capacity = EMA × 2 clamped to
 * [V8M_BIN_CAPACITY_MIN, V8M_BIN_CAPACITY_MAX]. Resets the
 * per-class counters and increments `gc_generation`.
 *
 * Normally invoked from the alloc / free fast paths once
 * `gc_countdown` hits zero (every V8M_TLC_GC_INTERVAL operations).
 * Exposed publicly so tests can drive the controller deterministically
 * without having to issue the full interval's worth of operations.
 *
 * Excess slots — when the new (smaller) capacity is below the
 * current `bin_count` — are NOT eagerly flushed here; the next
 * free that crosses the new threshold will batch-flush via the
 * existing overflow path. Keeps the GC tick cheap and avoids
 * pulling the slab-pool pointer through this header.
 */
void v8m_thread_cache_gc_tick(struct v8m_thread_cache *cache);

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
