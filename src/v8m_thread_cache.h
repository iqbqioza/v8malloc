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

#include "v8m_arch.h" /* V8M_CACHELINE_ALIGNED for false-sharing isolation */
#include "v8m_remote_free.h" /* v8m_mpsc_queue */
#include "v8m_size_class.h" /* V8M_MEDIUM_FIRST_CLASS */
#include "v8malloc/v8malloc.h" /* V8M_OPT_*, struct v8m_size_class_histogram */

/*
 * Per-class bin-capacity bounds (thread-cache.md §5.2). The cache
 * starts every class at V8M_BIN_CAPACITY_DEFAULT; the per-class
 * EMA controller (`v8m_thread_cache_gc_tick`) recomputes capacity
 * from observed demand each GC interval and clamps the result into
 * the [MIN, MAX] band.
 *
 * MAX raised from 256 → 512 to amortize the slab-pool refill
 * mutex acquisition over a longer cached run on multi-thread
 * workloads. MB-02 @ 8 threads spends a measurable fraction of
 * its time blocked on `g_dispatch.slab.lock` during refills; a
 * larger bin capacity halves the refill rate per thread, cutting
 * the per-class lock pressure proportionally. Worst-case per-thread
 * RSS overhead is ~2× the prior figure for hot classes (the
 * adaptive controller leaves cold classes near MIN), which the
 * fragmentation bench (MB-05) confirms stays inside the
 * "ties mimalloc / glibc" band.
 */
#define V8M_BIN_CAPACITY_MIN 16U
#define V8M_BIN_CAPACITY_DEFAULT 64U
#define V8M_BIN_CAPACITY_MAX 512U

/*
 * Medium-class TLC (classes 32..37, 8 KiB .. 256 KiB; backed by
 * the buddy pool). The alloc / free hot paths churn the same
 * size repeatedly, so caching even a handful of entries collapses
 * the buddy-pool round-trip to a per-thread bin pop. Caps are
 * deliberately small to bound worst-case per-thread RSS:
 *
 *   cls 32 (8 KiB) cap 4 → 32 KiB
 *   cls 33 (16 KiB) cap 4 → 64 KiB
 *   cls 34 (32 KiB) cap 2 → 64 KiB
 *   cls 35 (64 KiB) cap 2 → 128 KiB
 *   cls 36 (128 KiB) cap 1 → 128 KiB
 *   cls 37 (256 KiB) cap 1 → 256 KiB
 *
 * Worst-case per-thread medium-TLC overhead: 672 KiB. The bins
 * are bounded above by these compile-time caps and never grow
 * (no adaptive controller for medium classes — workloads with
 * tight medium-band churn benefit, others pay no cost because
 * the bins stay empty).
 */
#define V8M_MEDIUM_TLC_FIRST_CLASS V8M_MEDIUM_FIRST_CLASS
#define V8M_MEDIUM_TLC_LAST_CLASS 37U /* 256 KiB; class 38+ goes to large */
#define V8M_MEDIUM_TLC_NUM_CLASSES                                             \
	(V8M_MEDIUM_TLC_LAST_CLASS - V8M_MEDIUM_TLC_FIRST_CLASS + 1U)

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
 * Predictive prefetch table size (winning-algorithms.md §9). The
 * cache stores one byte per slot — the most recently observed
 * size class for the call site that hashes there. Power of two so
 * the hash → index step is a single AND. 1024 entries fits in 16
 * cache lines (64 B-aligned) on every supported arch, comfortably
 * inside L1.
 */
#define V8M_PREDICT_TABLE_SIZE 1024U

/*
 * Sentinel for "no prediction yet" in the table. Stored as the
 * full uint8_t so a memset(0xFF) at cache create time initializes
 * every slot to "unknown" without iterating.
 */
#define V8M_PREDICT_NONE 0xFFU

/*
 * Size-class histogram sample rate (size-classes.md §9). Every
 * V8M_HISTOGRAM_SAMPLE_RATE-th allocation observed by the dispatcher
 * is recorded into the per-class histogram; 1024 / 1024 = full
 * recording would cost two 8-byte writes per malloc fast path, so
 * the spec calls for sampling. Power of two so the modulo collapses
 * to a single AND on the cadence check.
 *
 * Sampled count → true count: multiply by V8M_HISTOGRAM_SAMPLE_RATE.
 * Sampled bytes → true bytes: same factor. The estimator is
 * unbiased in the limit of many allocations.
 */
#define V8M_HISTOGRAM_SAMPLE_RATE 64U

/*
 * Lifetime tracker (fragmentation.md §5.2). Sample one in every
 * V8M_LIFETIME_SAMPLE_RATE allocations into a per-TLC ring of
 * V8M_LIFETIME_RING_SIZE slots; the matching free linearly scans
 * the ring (cheap — 64 8-byte comparisons) and on a hit folds the
 * elapsed TSC ticks into the per-caller-PC EMA bucket. Bucket count
 * is small + linear-scan because lookup is on the cold (sampled)
 * path — only a tiny fraction of callers contribute distinct PCs.
 */
#define V8M_LIFETIME_SAMPLE_RATE 256U
#define V8M_LIFETIME_RING_SIZE 64U
#define V8M_LIFETIME_BUCKETS 32U

/*
 * Per-caller-PC lifetime bucket. `caller_pc == 0` marks an unused
 * slot (the bucket array is zero-initialized at cache create). The
 * EMA uses α = 0.25 (`new = (3 * old + sample) / 4`), matching the
 * adaptive bin-capacity controller above.
 */
struct v8m_lifetime_bucket {
	uintptr_t caller_pc;
	uint64_t sample_count;
	uint64_t ema_lifetime_ticks;
};

/*
 * Per-allocation ring entry — `ptr == NULL` marks the slot empty.
 * The next sampled allocation overwrites slot at `lifetime_ring_pos`
 * regardless of whether it's empty or holds a still-live entry; the
 * eviction count surfaces as the `samples_evicted` field of the
 * public stats so a caller can detect a too-small ring.
 */
struct v8m_lifetime_ring_entry {
	void *ptr;
	uintptr_t caller_pc;
	uint64_t alloc_tsc;
};

/*
 * Forward decl — the slab-class fast path needs to flush back to
 * the slab pool on overflow / module shutdown, but the cache header
 * does not pull v8m_slab_pool.h in. The .c file's include set
 * carries it.
 */
struct v8m_slab_pool;

/*
 * Per-thread allocator state. The cross-thread MPSC `remote` head
 * lives at offset 0 and is producer-written; the V8M_CACHELINE_ALIGNED
 * marker on `initialized` forces every consumer-only field onto a
 * fresh cache line so producer writes do not invalidate the
 * consumer thread's hot reads. The layout is treated as internal —
 * consumers go through the helpers below — and the
 * intentional padding is the false-sharing isolation the audit
 * (TODO P1 row 137) calls for.
 */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
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
	 *
	 * V8M_CACHELINE_ALIGNED here forces every field that follows
	 * to live on a fresh cache line, isolated from the
	 * `remote` MPSC head above. Producers writing to
	 * `remote.head` (cross-thread free routing — dormant in v0,
	 * lights up with the thread-owned-slab refactor) would
	 * otherwise false-share with the owner thread's hot reads
	 * of bin_heads / bin_count on every alloc / free. The
	 * isolation is preemptive; today's v0 sees no MPSC traffic
	 * so the cost is just struct-size padding.
	 */
	V8M_CACHELINE_ALIGNED uint32_t initialized;
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
	/*
	 * Last NUMA node observed for the calling thread. Used by
	 * the aggressive-migration path (numa.md §4.3, gated on
	 * V8M_OPT_NUMA_AGGRESSIVE_MIGRATION) to detect when the
	 * thread has been rescheduled onto a different node and
	 * relocate the cached slot pages via `move_pages()`.
	 * Initialized to UINT32_MAX so the first GC tick sees a
	 * delta and updates without firing migration on a fresh
	 * cache (no slots to migrate yet).
	 */
	uint32_t last_numa_node;
	/*
	 * Cumulative count of move_pages() calls the
	 * aggressive-migration path has issued from this cache.
	 * Diagnostic; tests use it to confirm the wiring fires
	 * without inspecting kernel state. Stays zero on hosts
	 * where the option is off or where the topology has only
	 * one node.
	 */
	uint64_t numa_migration_calls;
	/*
	 * Predictive prefetch table (winning-algorithms.md §9).
	 * Indexed by `(caller_pc >> 4) & (V8M_PREDICT_TABLE_SIZE - 1)`,
	 * stores the most recently observed size class for that call
	 * site. Updated on each public-API alloc; consulted to issue
	 * a `__builtin_prefetch` against the predicted bin head so
	 * the next pop incurs fewer L1 misses. Initialized to
	 * V8M_PREDICT_NONE on cache create — the prefetch helper
	 * skips entries it has not yet learned, so the table earns
	 * its keep only after the first round of allocations from
	 * each call site.
	 */
	uint8_t predict_table[V8M_PREDICT_TABLE_SIZE];
	/*
	 * Size-class request histogram (size-classes.md §9). Every
	 * V8M_HISTOGRAM_SAMPLE_RATE-th allocation through the
	 * dispatcher bumps the bucket matching its size class with the
	 * raw user-requested byte count. Owning thread is the sole
	 * writer (no atomics needed); the aggregation reader walks
	 * the cache registry under g_registry_lock and accepts
	 * statistically-stale reads. Non-class (Huge) requests land
	 * in the `huge_request_*` overflow pair below.
	 */
	uint64_t request_count[V8M_NUM_SIZE_CLASSES];
	uint64_t request_bytes[V8M_NUM_SIZE_CLASSES];
	uint64_t huge_request_count;
	uint64_t huge_request_bytes;
	/*
	 * Sample-rate counter — incremented on every alloc observed
	 * by the dispatcher; one-in-V8M_HISTOGRAM_SAMPLE_RATE pulses
	 * trigger a histogram bucket bump. Power-of-two rate folds
	 * the predicate to a single AND.
	 */
	uint64_t histogram_tick;
	/*
	 * Per-cache mirror of the dispatcher's process-wide alloc /
	 * free counters (api.md §4.2 `total_alloc_count` /
	 * `total_free_count`). The owner thread bumps them with a
	 * plain non-atomic increment on every alloc / free that
	 * routes through `v8m_dispatch_record_alloc_cache` /
	 * `v8m_dispatch_record_free_cache`; aggregation reads them
	 * under `g_registry_lock` with the existing stale-tolerant
	 * pattern (`v8m_thread_cache_aggregate_alloc_free`). The
	 * indirection eliminates the cross-thread cacheline ping on
	 * the v8m_free hot path that the previous single global
	 * `atomic_fetch_add` produced — under MB-02 @ 8t the global
	 * was being hammered ~25M times/sec from every CPU.
	 */
	uint64_t local_alloc_count;
	uint64_t local_free_count;
	/*
	 * Registry list link (size-class histogram aggregation +
	 * lifetime stats aggregation share the same registry).
	 * Manipulated only at cache create / destroy under
	 * g_registry_lock; the aggregation walk reads it under the
	 * same lock. Plain pointer (not atomic) because every
	 * touch is lock-protected.
	 */
	struct v8m_thread_cache *registry_next;
	/*
	 * Lifetime tracker (fragmentation.md §5.2). All fields are
	 * single-thread-write (the owning thread); aggregation reads
	 * them under the registry lock with stale-read tolerance, the
	 * same model the size-class histogram uses.
	 */
	struct v8m_lifetime_ring_entry lifetime_ring[V8M_LIFETIME_RING_SIZE];
	uint32_t lifetime_ring_pos;
	uint64_t lifetime_sample_tick;
	struct v8m_lifetime_bucket lifetime_buckets[V8M_LIFETIME_BUCKETS];
	uint64_t lifetime_samples_recorded;
	uint64_t lifetime_samples_completed;
	uint64_t lifetime_samples_evicted;
	uint64_t lifetime_ephemeral_count;
	uint64_t lifetime_short_count;
	uint64_t lifetime_long_count;
	/*
	 * Medium-class bins (classes V8M_MEDIUM_TLC_FIRST_CLASS ..
	 * V8M_MEDIUM_TLC_LAST_CLASS). Indexed by `cls -
	 * V8M_MEDIUM_TLC_FIRST_CLASS`. Same intrusive-next-pointer convention
	 * as the slab bins above: cached object's first 8 bytes hold the next
	 * link. Safe because every cached medium block is at least 8 KiB and
	 * the buddy pool stamps no metadata into the block body.
	 *
	 * Caps are compile-time per-class (see V8M_MEDIUM_TLC_*),
	 * not adaptive — medium-class churn is bursty and the small
	 * caps already bound RSS overhead at <1 MiB / thread.
	 */
	void *medium_bin_heads[V8M_MEDIUM_TLC_NUM_CLASSES];
	uint8_t medium_bin_count[V8M_MEDIUM_TLC_NUM_CLASSES];
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
 * pthread_atfork plumbing for the cache registry mutex. A worker
 * mid-create or mid-destroy holds `g_registry_lock`; without these
 * hooks, a fork in that window leaves the child with the lock
 * inherited in held-by-dead-thread state and the child deadlocks
 * on the first allocation that lazy-creates a TLC. Called by the
 * api's atfork chain.
 */
void v8m_thread_cache_prefork(void);
void v8m_thread_cache_postfork_parent(void);
void v8m_thread_cache_postfork_child(void);

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
 * Per-thread cache pointer, exposed so the inlined malloc fast path
 * can read the TLS slot directly (one %fs-relative load) instead of
 * routing through `v8m_thread_cache_peek` (function call + same load,
 * but defeating the inliner's view). Slow-path consumers stay on the
 * function form for the comment + scope niceness.
 */
extern __thread struct v8m_thread_cache *v8m_t_cache;

/* Forward decl: the canonical decl with the doc comment is further
 * down in this header (the GC-tick section); declared here too so
 * the inlines below resolve it. */
void v8m_thread_cache_gc_tick(struct v8m_thread_cache *cache);

/*
 * Inlined TLC bin pop. Exposes the same logic as
 * v8m_thread_cache_alloc but as a static inline so the malloc fast
 * path can collapse the call into a handful of instructions.
 * Returns NULL on bin-empty / out-of-range class — caller falls
 * through to the slow path. memcpy on a 1-byte read intrinsifies
 * to a mov.
 */
static inline void *
v8m_thread_cache_alloc_inline(struct v8m_thread_cache *cache, uint32_t cls)
{
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS) {
		return NULL;
	}
	void *head = cache->bin_heads[cls];
	if (head == NULL) {
		return NULL;
	}
	void *next = NULL;
	__builtin_memcpy((void *)&next, head, sizeof(next));
	cache->bin_heads[cls] = next;
	cache->bin_count[cls]--;
	cache->alloc_count_per_class[cls]++;
	if (__builtin_expect(--cache->gc_countdown == 0U, 0)) {
		v8m_thread_cache_gc_tick(cache);
	}
	return head;
}

/*
 * Inlined TLC bin push. Mirror of v8m_thread_cache_alloc_inline.
 * Returns true when the bin reached capacity (caller must invoke
 * the flush-half slow path). `obj`'s first sizeof(void *) bytes are
 * overwritten with the prior bin head — caller has relinquished the
 * slot.
 */
static inline bool v8m_thread_cache_free_inline(struct v8m_thread_cache *cache,
						uint32_t cls, void *obj)
{
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS || obj == NULL) {
		return false;
	}
	void *prev_head = cache->bin_heads[cls];
	__builtin_memcpy(obj, (const void *)&prev_head, sizeof(prev_head));
	cache->bin_heads[cls] = obj;
	cache->bin_count[cls]++;
	cache->free_count_per_class[cls]++;
	if (__builtin_expect(--cache->gc_countdown == 0U, 0)) {
		v8m_thread_cache_gc_tick(cache);
	}
	return cache->bin_count[cls] >= cache->bin_capacity[cls];
}

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
 * Medium-class TLC: pop one cached buddy block of size class
 * `cls`. `cls` must be in [V8M_MEDIUM_TLC_FIRST_CLASS,
 * V8M_MEDIUM_TLC_LAST_CLASS]; out-of-range classes return NULL.
 * Returns NULL on bin-empty.
 */
void *v8m_thread_cache_medium_alloc(struct v8m_thread_cache *cache,
				    uint32_t cls);

/*
 * Medium-class TLC: push one freed buddy block. Returns true when
 * the bin would overflow this push (caller frees `obj` directly to
 * the buddy pool instead of caching it). Returns false on the
 * cached path.
 */
bool v8m_thread_cache_medium_free(struct v8m_thread_cache *cache, uint32_t cls,
				  void *obj);

/*
 * Drain every medium bin to `dispatch_buddy` (forward-declared
 * here as a void * to avoid pulling v8m_buddy_pool.h into the
 * thread-cache header). Called from the cache-destroy / drain
 * paths so a thread that exits with cached medium blocks does
 * not leak them.
 */
size_t v8m_thread_cache_drain_medium(struct v8m_thread_cache *cache,
				     void *dispatch_buddy);

/*
 * Drop the calling thread's TLS slot so the next allocation creates
 * a fresh cache. Caller is responsible for draining the cache via
 * `v8m_thread_cache_drain_all` first if it wants the slots back to
 * the pool — this helper purely resets the TLS pointer and frees
 * the cache struct. Backs the public `v8m_release_thread`. No-op
 * if the calling thread has no cache.
 */
void v8m_thread_cache_release_local(void);

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
 * Pop up to `count` slots from the cls bin and assemble them
 * into a forward-linked chain via the existing intrusive
 * next-pointer scheme. Returns the actual count, possibly
 * smaller than `count` if the bin had fewer entries (or zero
 * when the bin is empty). On a non-zero return, `*out_head`
 * points at the chain's head and `*out_tail` at the last node;
 * the chain is terminated (tail's next slot is NULL). Used by
 * the dispatcher's overflow path to ship a batch to the L2
 * core cache via a single push_batch instead of N individual
 * slab_pool_free calls.
 */
size_t v8m_thread_cache_drain_chain(struct v8m_thread_cache *cache,
				    uint32_t cls, size_t count, void **out_head,
				    void **out_tail);

/*
 * Install a forward-linked chain of `count` slots onto the cls
 * bin. The chain is the output of v8m_core_cache_pop_batch (or
 * v8m_thread_cache_drain_chain on another cache). Used by the
 * dispatcher's underflow path when a batch refill from the L2
 * lands cleanly on the local bin.
 */
void v8m_thread_cache_install_chain(struct v8m_thread_cache *cache,
				    uint32_t cls, void *head, void *tail,
				    size_t count);

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
 *
 * (Already forward-declared above for the inline-fast-path's
 * countdown-hit branch.)
 */
/* NOLINTNEXTLINE(readability-redundant-declaration) */
void v8m_thread_cache_gc_tick(struct v8m_thread_cache *cache);

/*
 * Issue a `__builtin_prefetch` for the bin head matching the
 * predicted class for `caller_pc`. No-op when the predict slot
 * is V8M_PREDICT_NONE (no prior observation) or when the cache
 * is NULL. Cheap — one table lookup + one prefetch hint; safe to
 * call on any thread, no locks taken.
 *
 * Pair with v8m_thread_cache_predict_update after the actual
 * allocation: the update records the class the predict was
 * supposed to forecast.
 */
void v8m_thread_cache_predict_prefetch(struct v8m_thread_cache *cache,
				       const void *caller_pc);

/*
 * Record `cls` as the size class observed for `caller_pc`. Hash
 * uses the same formula as predict_prefetch so the next call
 * from the same site sees the recorded class. `cls` ≥
 * V8M_NUM_SIZE_CLASSES is treated as "do not record" — the
 * Large/Huge sentinel and any out-of-range value bypasses the
 * write, leaving the prior prediction intact.
 */
void v8m_thread_cache_predict_update(struct v8m_thread_cache *cache,
				     const void *caller_pc, uint32_t cls);

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

/*
 * Sample one allocation request into the size-class histogram. Cheap
 * fast-path entry: looks up the calling thread's TLC via the read-only
 * peek (no lazy create — the caller may itself be in the middle of
 * creating one), advances the per-cache sample tick, and records the
 * sampled allocation into the matching class bucket. Allocations made
 * before the calling thread has a TLC (bootstrap chain, signal-safe)
 * route into the global fallback bucket.
 *
 * `cls` ≥ V8M_NUM_SIZE_CLASSES (i.e. V8M_CLASS_HUGE) routes to the
 * huge overflow pair instead.
 */
void v8m_thread_cache_record_alloc(uint32_t cls, size_t request_size);

/*
 * Snapshot the aggregated histogram across every live cache plus the
 * global carry-over (fallback path + cache-destroy fold-in) into
 * `*out`. Tolerates NULL. Holds the registry lock briefly during the
 * walk; allocations on other threads continue uninterrupted but their
 * snapshot value may be stale by one increment.
 */
void v8m_thread_cache_aggregate_histogram(struct v8m_size_class_histogram *out);

/*
 * Sum the per-cache `local_alloc_count` / `local_free_count`
 * mirrors across every live cache. Caller adds the dispatcher's
 * global atomic fallback values to obtain the api.md §4.2
 * `total_alloc_count` / `total_free_count` snapshots. Either out
 * pointer may be NULL to skip that field. Holds `g_registry_lock`
 * briefly during the walk; loads are non-atomic with stale-tolerance
 * (the same model the histogram aggregator uses).
 */
void v8m_thread_cache_aggregate_alloc_free(uint64_t *out_allocs,
					   uint64_t *out_frees);

/*
 * Reset the per-cache `local_alloc_count` / `local_free_count`
 * mirrors to zero across every live cache. Companion to
 * `v8m_dispatch_reset_alloc_free_counts`, invoked by
 * `v8m_reset_stats`. Holds `g_registry_lock` for the walk.
 */
void v8m_thread_cache_reset_alloc_free_counts(void);

/*
 * Lifetime tracker (fragmentation.md §5.2). Both record helpers
 * exit early when V8M_OPT_LIFETIME_TRACKING is off, so the cost of
 * being-disabled is one config load + one branch on the malloc /
 * free fast paths. When on, record_alloc samples 1-in-N allocations
 * into the calling thread's TLC ring, and record_free linearly
 * scans the ring on every free, classifying matches by EMA and
 * bumping the per-TLC counters.
 *
 * `caller_pc` should be the value `__builtin_return_address(0)`
 * captured by the callsite; the tracker hashes it down to a small
 * bucket index. The TSC read happens INSIDE the helpers, gated
 * behind the option + sample-rate / cache-presence check, so the
 * default-mode hot path (LIFETIME_TRACKING off) pays nothing — not
 * even an `rdtsc` instruction. The previous interface required the
 * caller to compute tsc up front, which silently put rdtsc on every
 * malloc / free.
 */
void v8m_thread_cache_lifetime_record_alloc(void *ptr, const void *caller_pc);
void v8m_thread_cache_lifetime_record_free(const void *ptr);

/*
 * Snapshot the lifetime-tracker stats across every live cache plus
 * the global carry-over into `*out`. Tolerates NULL. Pre-init or
 * with the option off returns the all-zero baseline.
 */
struct v8m_lifetime_stats; /* declared in v8malloc.h */
void v8m_thread_cache_aggregate_lifetime(struct v8m_lifetime_stats *out);

/*
 * Classify the per-caller-PC lifetime EMA accumulated by the
 * tracker. Returns one of the public `enum v8m_lifetime_class`
 * values. Tolerates NULL caller_pc, returning UNKNOWN. Reads only
 * the calling thread's TLC bucket — no cross-thread lookup, so
 * the per-thread observed lifetime is what drives the answer.
 */
enum v8m_lifetime_class
v8m_thread_cache_lifetime_classify(const void *caller_pc);

#endif /* V8M_THREAD_CACHE_H */
