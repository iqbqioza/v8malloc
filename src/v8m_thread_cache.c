/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Thread cache (L1) — implementation. See v8m_thread_cache.h for
 * the design notes; this file just wires the TLS slot, the lazy
 * allocator, and the pthread_key-based destructor.
 *
 * The cache itself is allocated through the standard allocator
 * surface (`malloc`), which routes through dispatch when the
 * library is READY and through bootstrap otherwise. Bootstrap
 * allocations leak by design, but the cache is sized in tens of
 * bytes today, so a thread that creates its cache before the
 * constructor finishes leaks only a trivial amount.
 */

#include "v8m_thread_cache.h"

#include <pthread.h> /* IWYU pragma: keep — pthread_key_t */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/syscall.h> /* SYS_move_pages */
#include <unistd.h> /* syscall */

#include "v8m_arch.h" /* V8M_CACHE_LINE_SIZE for aligned_alloc */
#include "v8m_config.h" /* v8m_config_get for the migration opt-in */
/* v8m_arch_rdtsc + tsc_frequency_mhz live in v8m_arch.h via the
 * same include above — no extra include needed. */
#include "v8m_internal.h" /* V8M_PAGE_MASK */
#include "v8m_numa.h" /* v8m_numa_current_node */
#include "v8m_page.h" /* v8m_ptr_to_meta — meta recovery on flush */
#include "v8m_page_heap.h" /* v8m_page_heap_node_is_suppressed for TLC shrink */
#include "v8m_remote_free.h" /* v8m_mpsc_init */
#include "v8m_size_class.h" /* V8M_MEDIUM_FIRST_CLASS */
#include "v8m_slab_pool.h" /* v8m_slab_pool_free on overflow flush */
#include "v8malloc/v8malloc.h" /* V8M_OPT_NUMA_AGGRESSIVE_MIGRATION */

/*
 * Per-thread cache pointer. NULL until the thread first calls
 * v8m_thread_cache_get_or_create. Reset to NULL by the destructor
 * (which fires from glibc's TLS-cleanup path on thread exit) so a
 * thread that re-enters the allocator after pthread_exit (extremely
 * rare but possible via destructors-of-destructors) gets a fresh
 * cache rather than dangling.
 */
static __thread struct v8m_thread_cache *t_cache;

/*
 * Reentrancy guard. The cache itself is allocated via malloc, which
 * now routes through the dispatcher's TLC fast path — calling back
 * into v8m_thread_cache_get_or_create. The guard short-circuits the
 * inner call so the outer malloc serves directly from the slab pool;
 * the cache struct is then installed and subsequent calls take the
 * fast path normally. Without this the first allocation in any
 * thread would infinite-recurse and stack-overflow.
 */
static __thread bool t_in_create;

/*
 * Sticky once-set flag: marks the calling thread as past the
 * pthread_key destructor. After the thread's cache has been
 * reclaimed, any later allocation (the destructor itself frees the
 * cache, and the post-destructor cleanup path may allocate too)
 * must NOT install a fresh cache — the pthread runtime would then
 * iterate the destructor again on the new value, recursing up to
 * PTHREAD_DESTRUCTOR_ITERATIONS times before giving up. Bypassing
 * TLC for these tail allocations keeps the destructor count
 * matching the thread count.
 */
static __thread bool t_in_destructor;

/*
 * pthread_key whose destructor reclaims a thread's cache when the
 * thread exits. Initialized exactly once via
 * v8m_thread_cache_module_init; the `g_key_initialized` atomic
 * guards the lazy alloc path against a thread that races the
 * library constructor (the get-or-create call simply skips the
 * pthread_setspecific until the key is ready).
 */
/* pthread.h IS included above; the IWYU-style cleaner does not
 * always recognize that for typedef'd names like pthread_key_t. */
static pthread_key_t g_destructor_key; /* NOLINT(misc-include-cleaner) */
static atomic_bool g_key_initialized;

/* Cumulative count of caches the destructor has reclaimed. Useful
 * for tests that want to verify thread-exit cleanup fired without
 * having to reach into the allocator's internals. Relaxed-atomic;
 * the count is monotonic and the test reads it from the parent
 * after pthread_join. */
static _Atomic uint64_t g_destructor_calls;

/* Drain hook (uintptr_t storage so atomics on a function pointer
 * stay portable). Set by v8m_api.c at constructor time to a
 * wrapper that knows the dispatcher singleton; the destructor
 * calls it (when non-NULL) to drain cached slots back to the slab
 * pool before freeing the cache struct. */
static atomic_uintptr_t g_drain_hook;

/* Mirror the public histogram class count to the internal size-class
 * count. Bumping V8M_NUM_SIZE_CLASSES without bumping the public
 * mirror would silently truncate one direction of the snapshot. */
_Static_assert(V8M_NUM_SIZE_CLASSES == V8M_PUBLIC_NUM_SIZE_CLASSES,
	       "public histogram class count must match internal class count");

/*
 * Cache registry — singly-linked list of every live TLC. The
 * histogram aggregator walks this under g_registry_lock; cache
 * create / destroy mutates it under the same lock. The lock is
 * never held during malloc/free fast-path work, so the alloc path
 * never blocks on a concurrent aggregator.
 */
/* NOLINTNEXTLINE(misc-include-cleaner) — pthread.h is included */
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct v8m_thread_cache *g_registry_head;

/*
 * Global histogram counters — cover allocations that bypassed TLC
 * (bootstrap, signal-safe) and accumulate the carry-over from caches
 * that have already exited (the destructor folds the cache's per-class
 * counts in here before freeing it). Atomic so the rare cross-thread
 * touches stay race-free; the per-TLC counters are plain uint64_t
 * because only the owning thread writes them.
 */
static _Atomic uint64_t g_global_request_count[V8M_NUM_SIZE_CLASSES];
static _Atomic uint64_t g_global_request_bytes[V8M_NUM_SIZE_CLASSES];
static _Atomic uint64_t g_global_huge_request_count;
static _Atomic uint64_t g_global_huge_request_bytes;
/*
 * Sample tick for the no-TLC fallback path. Bumped on every
 * histogram-record call that didn't find a per-thread cache; when a
 * tick crosses the sample-rate boundary we add to the global
 * counters. Atomic to keep the cross-thread sampling cadence
 * well-defined despite the path being rare (bootstrap + signal-safe).
 */
static _Atomic uint64_t g_global_histogram_tick;

/*
 * Lifetime tracker — global carry-over counters (cache fold-in
 * absorbs each TLC's stats at thread exit so the snapshot retains
 * full history across the process lifetime). All atomic so the
 * cross-thread folds and the aggregator's reads stay race-free.
 */
static _Atomic uint64_t g_global_lifetime_recorded;
static _Atomic uint64_t g_global_lifetime_completed;
static _Atomic uint64_t g_global_lifetime_evicted;
static _Atomic uint64_t g_global_lifetime_ephemeral;
static _Atomic uint64_t g_global_lifetime_short;
static _Atomic uint64_t g_global_lifetime_long;

/*
 * Lazily-initialized lifetime classification thresholds in TSC
 * ticks. EPHEMERAL means lifetime < 100 microseconds (function
 * scope), LONG means lifetime > 100 milliseconds (close to
 * application lifetime), SHORT covers the gap. Computed from
 * v8m_arch_tsc_frequency_mhz() the first time a sample completes;
 * the atomic store/load resolves the racy double-init harmlessly
 * (every initializer computes the same value).
 */
static _Atomic uint64_t g_lifetime_short_threshold_ticks;
static _Atomic uint64_t g_lifetime_long_threshold_ticks;

static void ensure_lifetime_thresholds(void)
{
	if (atomic_load_explicit(&g_lifetime_long_threshold_ticks,
				 memory_order_relaxed) != 0U) {
		return;
	}
	uint64_t mhz = (uint64_t)v8m_arch_tsc_frequency_mhz();
	if (mhz == 0U) {
		mhz = 1000U; /* defensive — non-x86_64 arch helper returns
			      * 1000 (ticks-per-µs basis) by contract. */
	}
	/* 100 µs and 100 ms in TSC ticks. */
	atomic_store_explicit(&g_lifetime_short_threshold_ticks, mhz * 100U,
			      memory_order_relaxed);
	atomic_store_explicit(&g_lifetime_long_threshold_ticks,
			      mhz * 100U * 1000U, memory_order_relaxed);
}

/*
 * Map a caller PC to a per-cache bucket. Linear-probe over a small
 * (V8M_LIFETIME_BUCKETS) array; on a fresh PC the first empty slot
 * claims it. When the array fills, returns NULL — at that point the
 * tracker silently drops new PCs (the existing buckets keep refining
 * their EMA, the dropped PC's samples still count toward
 * `samples_completed` and the per-class totals via the aggregate
 * classifier below).
 */
static struct v8m_lifetime_bucket *
lifetime_bucket_for(struct v8m_thread_cache *cache, uintptr_t caller_pc)
{
	if (caller_pc == 0U) {
		caller_pc = 1U; /* reserve 0 as the empty marker */
	}
	uint32_t start =
	    (uint32_t)((caller_pc >> 4U) & (V8M_LIFETIME_BUCKETS - 1U));
	for (uint32_t step = 0; step < V8M_LIFETIME_BUCKETS; step++) {
		uint32_t idx = (start + step) & (V8M_LIFETIME_BUCKETS - 1U);
		struct v8m_lifetime_bucket *bucket =
		    &cache->lifetime_buckets[idx];
		if (bucket->caller_pc == caller_pc) {
			return bucket;
		}
		if (bucket->caller_pc == 0U) {
			bucket->caller_pc = caller_pc;
			return bucket;
		}
	}
	return NULL;
}

static void classify_and_count(struct v8m_thread_cache *cache, uint64_t ticks)
{
	uint64_t short_thr = atomic_load_explicit(
	    &g_lifetime_short_threshold_ticks, memory_order_relaxed);
	uint64_t long_thr = atomic_load_explicit(
	    &g_lifetime_long_threshold_ticks, memory_order_relaxed);
	if (ticks < short_thr) {
		cache->lifetime_ephemeral_count++;
	} else if (ticks < long_thr) {
		cache->lifetime_short_count++;
	} else {
		cache->lifetime_long_count++;
	}
}

/*
 * Unlink `cache` from the registry list and fold its accumulated
 * histogram counters into the global carry-over. Called from the
 * destructor + module-shutdown paths so the snapshot does not lose
 * counts when a TLC goes away. Acquires the registry lock briefly;
 * histogram recording on other threads stays unaffected because
 * those bump their own per-cache counters without touching the lock.
 */
static void registry_unregister_and_fold(struct v8m_thread_cache *cache)
{
	if (cache == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&g_registry_lock);
	struct v8m_thread_cache **link = &g_registry_head;
	while (*link != NULL && *link != cache) {
		link = &(*link)->registry_next;
	}
	if (*link == cache) {
		*link = cache->registry_next;
		cache->registry_next = NULL;
	}
	(void)pthread_mutex_unlock(&g_registry_lock);

	for (uint32_t cls = 0; cls < V8M_NUM_SIZE_CLASSES; cls++) {
		if (cache->request_count[cls] != 0U) {
			atomic_fetch_add_explicit(&g_global_request_count[cls],
						  cache->request_count[cls],
						  memory_order_relaxed);
		}
		if (cache->request_bytes[cls] != 0U) {
			atomic_fetch_add_explicit(&g_global_request_bytes[cls],
						  cache->request_bytes[cls],
						  memory_order_relaxed);
		}
	}
	if (cache->huge_request_count != 0U) {
		atomic_fetch_add_explicit(&g_global_huge_request_count,
					  cache->huge_request_count,
					  memory_order_relaxed);
	}
	if (cache->huge_request_bytes != 0U) {
		atomic_fetch_add_explicit(&g_global_huge_request_bytes,
					  cache->huge_request_bytes,
					  memory_order_relaxed);
	}
	if (cache->lifetime_samples_recorded != 0U) {
		atomic_fetch_add_explicit(&g_global_lifetime_recorded,
					  cache->lifetime_samples_recorded,
					  memory_order_relaxed);
	}
	if (cache->lifetime_samples_completed != 0U) {
		atomic_fetch_add_explicit(&g_global_lifetime_completed,
					  cache->lifetime_samples_completed,
					  memory_order_relaxed);
	}
	if (cache->lifetime_samples_evicted != 0U) {
		atomic_fetch_add_explicit(&g_global_lifetime_evicted,
					  cache->lifetime_samples_evicted,
					  memory_order_relaxed);
	}
	if (cache->lifetime_ephemeral_count != 0U) {
		atomic_fetch_add_explicit(&g_global_lifetime_ephemeral,
					  cache->lifetime_ephemeral_count,
					  memory_order_relaxed);
	}
	if (cache->lifetime_short_count != 0U) {
		atomic_fetch_add_explicit(&g_global_lifetime_short,
					  cache->lifetime_short_count,
					  memory_order_relaxed);
	}
	if (cache->lifetime_long_count != 0U) {
		atomic_fetch_add_explicit(&g_global_lifetime_long,
					  cache->lifetime_long_count,
					  memory_order_relaxed);
	}
}

static void destroy_cache(void *arg)
{
	struct v8m_thread_cache *cache = arg;
	if (cache == NULL) {
		return;
	}
	/* Drain BEFORE latching the bypass flag — the drain calls
	 * v8m_slab_pool_free directly (no dispatcher round trip),
	 * so it is safe to run while TLC is still nominally live
	 * for this thread. After the drain, bin_count[] is zero so
	 * any subsequent push (which cannot happen, but defensively)
	 * would not leak slots. */
	uintptr_t hook_raw =
	    atomic_load_explicit(&g_drain_hook, memory_order_acquire);
	if (hook_raw != 0) {
		v8m_thread_cache_drain_hook hook;
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		hook = (v8m_thread_cache_drain_hook)hook_raw;
		hook(cache);
	}
	registry_unregister_and_fold(cache);
	/* Latch the bypass flag BEFORE the free — the free routes
	 * through the dispatcher's TLC fast path and would otherwise
	 * lazily install a fresh cache, which the pthread runtime
	 * would then iterate the destructor against again
	 * (PTHREAD_DESTRUCTOR_ITERATIONS = 4 on Linux, so a thread
	 * with a single cache would otherwise show 4 destructor
	 * calls before the runtime gives up). The flag is sticky
	 * (never cleared back to false) so any allocation in the
	 * thread's tail life — e.g., another library's pthread_key
	 * destructor that runs after ours — also bypasses TLC. */
	t_in_destructor = true;
	t_cache = NULL;
	free(cache);
	atomic_fetch_add_explicit(&g_destructor_calls, 1U,
				  memory_order_relaxed);
}

void v8m_thread_cache_set_drain_hook(v8m_thread_cache_drain_hook hook)
{
	atomic_store_explicit(&g_drain_hook, (uintptr_t)hook,
			      memory_order_release);
}

struct v8m_thread_cache *v8m_thread_cache_peek(void)
{
	return t_cache;
}

struct v8m_thread_cache *v8m_thread_cache_get_or_create(void)
{
	struct v8m_thread_cache *cache = t_cache;
	if (cache != NULL) {
		return cache;
	}
	if (t_in_create || t_in_destructor) {
		/* Reentrant create-during-create OR allocation after
		 * the destructor latched: bypass TLC and let the
		 * dispatcher serve directly from the slab pool. */
		return NULL;
	}
	t_in_create = true;
	/* aligned_alloc requires size to be a multiple of alignment.
	 * The cache struct itself is V8M_CACHELINE_ALIGNED on its
	 * `initialized` field, but the C standard does not promise
	 * `sizeof(*cache)` is a multiple of V8M_CACHE_LINE_SIZE on
	 * every compiler — round up explicitly. The cache-line
	 * alignment of the struct base ensures the
	 * V8M_CACHELINE_ALIGNED field on `initialized` actually
	 * lands on a fresh cache line in memory (not just at the
	 * struct-relative offset). */
	size_t aligned_size = (sizeof(*cache) + V8M_CACHE_LINE_SIZE - 1U) &
			      ~((size_t)V8M_CACHE_LINE_SIZE - 1U);
	cache = aligned_alloc(V8M_CACHE_LINE_SIZE, aligned_size);
	t_in_create = false;
	if (cache == NULL) {
		return NULL;
	}
	(void)memset(cache, 0, sizeof(*cache));
	v8m_mpsc_init(&cache->remote);
	cache->initialized = 1U;
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		cache->bin_capacity[i] = V8M_BIN_CAPACITY_DEFAULT;
	}
	cache->gc_countdown = V8M_TLC_GC_INTERVAL;
	/* Predict table: every slot starts unknown so the prefetch
	 * helper skips them until the first observation lands. */
	(void)memset(cache->predict_table, V8M_PREDICT_NONE,
		     sizeof(cache->predict_table));
	/* UINT32_MAX = "no node observed yet" — the first GC tick
	 * after the cache populates will always update this without
	 * firing migration (the cache has nothing to migrate yet). */
	cache->last_numa_node = UINT32_MAX;

	t_cache = cache;
	if (atomic_load_explicit(&g_key_initialized, memory_order_acquire)) {
		/* Best-effort: a non-zero return here just means the
		 * destructor won't fire on thread exit (the cache then
		 * leaks at thread exit, not at process exit). The
		 * allocator stays usable. */
		(void)pthread_setspecific(g_destructor_key, cache);
	}
	/* Register on the histogram-aggregation list. Holding the lock
	 * across only the link-update keeps the critical section short;
	 * no allocations happen while it is held. */
	(void)pthread_mutex_lock(&g_registry_lock);
	cache->registry_next = g_registry_head;
	g_registry_head = cache;
	(void)pthread_mutex_unlock(&g_registry_lock);
	return cache;
}

/*
 * Tick the adaptive-controller down-counter and fire the GC tick
 * when it hits zero. Inlined into both alloc and free so the
 * fast-path cost is one decrement + one branch per call. Lives in
 * the same TU as gc_tick itself so the compiler can fold the call
 * site when the counter is non-zero (the common case).
 */
static inline void tlc_tick_gc(struct v8m_thread_cache *cache)
{
	if (--cache->gc_countdown == 0U) {
		v8m_thread_cache_gc_tick(cache);
	}
}

void *v8m_thread_cache_alloc(struct v8m_thread_cache *cache, uint32_t cls)
{
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS) {
		return NULL;
	}
	void *head = cache->bin_heads[cls];
	if (head == NULL) {
		return NULL;
	}
	/* The cached object's first 8 bytes hold the next pointer.
	 * Reading them is well-defined because every cached object
	 * is at least sizeof(void *) wide (the smallest size class
	 * is 8 bytes on every supported arch). The void * cast on
	 * `&next` keeps clang-tidy's multi-level-pointer rule happy
	 * (the natural type is `void **`, but memcpy takes `void *`). */
	void *next = NULL;
	(void)memcpy((void *)&next, head, sizeof(next));
	cache->bin_heads[cls] = next;
	cache->bin_count[cls]--;
	cache->alloc_count_per_class[cls]++;
	tlc_tick_gc(cache);
	return head;
}

bool v8m_thread_cache_free(struct v8m_thread_cache *cache, uint32_t cls,
			   void *obj)
{
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS || obj == NULL) {
		return false;
	}
	void *prev_head = cache->bin_heads[cls];
	(void)memcpy(obj, (const void *)&prev_head, sizeof(prev_head));
	cache->bin_heads[cls] = obj;
	cache->bin_count[cls]++;
	cache->free_count_per_class[cls]++;
	tlc_tick_gc(cache);
	return cache->bin_count[cls] >= cache->bin_capacity[cls];
}

/*
 * Index a caller PC into the predict table. Drop the low 4 bits
 * (instruction-alignment noise on every supported arch) and
 * mask down to the table size. The table size is a power of two
 * so the mask collapses to a single AND.
 */
static inline size_t predict_index(const void *caller_pc)
{
	uintptr_t pc_bits = (uintptr_t)caller_pc;
	return (size_t)((pc_bits >> 4U) & (V8M_PREDICT_TABLE_SIZE - 1U));
}

void v8m_thread_cache_predict_prefetch(struct v8m_thread_cache *cache,
				       const void *caller_pc)
{
	if (cache == NULL) {
		return;
	}
	size_t idx = predict_index(caller_pc);
	uint8_t predicted = cache->predict_table[idx];
	if (predicted >= V8M_MEDIUM_FIRST_CLASS) {
		/* No observation yet (V8M_PREDICT_NONE) or a class the
		 * TLC does not cache (Medium / Large / Huge). Skip the
		 * prefetch — there is no bin head to warm. */
		return;
	}
	/* `&cache->bin_heads[predicted]` is naturally `void **`; the
	 * cast to `const void *` keeps clang-tidy's
	 * multi-level-pointer rule happy without changing the
	 * generated prefetch (it remains a hint at the same
	 * address). */
	__builtin_prefetch((const void *)&cache->bin_heads[predicted], 0, 3);
}

void v8m_thread_cache_predict_update(struct v8m_thread_cache *cache,
				     const void *caller_pc, uint32_t cls)
{
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS) {
		return;
	}
	size_t idx = predict_index(caller_pc);
	cache->predict_table[idx] = (uint8_t)cls;
}

/*
 * Aggressive NUMA migration (numa.md §4.3, gated on
 * V8M_OPT_NUMA_AGGRESSIVE_MIGRATION). When the calling thread's
 * NUMA node has changed since the last GC tick, walk every cached
 * slot in the bins, collect each slot's containing page base into
 * a small unique-set, and call `move_pages()` to relocate those
 * pages to the new node. Skips on single-node hosts (nothing to
 * relocate to) and silently ignores syscall failures (the
 * relocation is a perf hint, not a correctness requirement).
 *
 * The unique-page array is sized at 256 — enough to cover a fully
 * loaded TLC at typical capacities (worst case 256 slots/class ×
 * 32 classes = 8192 slots, but each slab page holds many slots so
 * the unique page count is far smaller). When the actual count
 * exceeds the cap we migrate what we tracked and let the next
 * tick handle the rest.
 */
#define V8M_TLC_MIGRATION_MAX_PAGES 256U

static void check_numa_migration(struct v8m_thread_cache *cache)
{
	uint32_t current_node = v8m_numa_current_node();
	uint32_t last_node = cache->last_numa_node;
	cache->last_numa_node = current_node;

	if (last_node == current_node || last_node == UINT32_MAX) {
		return;
	}
	if (v8m_config_get(V8M_OPT_NUMA_AGGRESSIVE_MIGRATION) == 0) {
		return;
	}
	if (v8m_numa_node_count() <= 1U) {
		return;
	}

	void *pages[V8M_TLC_MIGRATION_MAX_PAGES];
	int nodes[V8M_TLC_MIGRATION_MAX_PAGES];
	size_t count = 0;
	for (uint32_t cls = 0; cls < V8M_MEDIUM_FIRST_CLASS &&
			       count < V8M_TLC_MIGRATION_MAX_PAGES;
	     cls++) {
		void *slot = cache->bin_heads[cls];
		while (slot != NULL && count < V8M_TLC_MIGRATION_MAX_PAGES) {
			void *page_base =
			    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
			    (void *)((uintptr_t)slot & V8M_PAGE_MASK);
			bool seen = false;
			for (size_t i = 0; i < count; i++) {
				if (pages[i] == page_base) {
					seen = true;
					break;
				}
			}
			if (!seen) {
				pages[count] = page_base;
				nodes[count] = (int)current_node;
				count++;
			}
			void *next = NULL;
			(void)memcpy((void *)&next, slot, sizeof(next));
			slot = next;
		}
	}
	if (count == 0U) {
		return;
	}
	(void)syscall(SYS_move_pages, 0, count, pages, nodes, NULL, 0);
	cache->numa_migration_calls++;
}

void v8m_thread_cache_gc_tick(struct v8m_thread_cache *cache)
{
	if (cache == NULL) {
		return;
	}
	check_numa_migration(cache);
	/* NUMA overload pressure: when the calling thread's node is
	 * suppressed by the page-heap rebalance action (numa.md §6.2),
	 * shrink the demand-driven capacity below the steady-state
	 * cap so the per-thread cache holds fewer slots — reduces
	 * the per-thread footprint that the rebalance is trying to
	 * relieve. The shrink is multiplicative (halve), so a thread
	 * that was at MAX cap drops toward MIN over a few overloaded
	 * ticks. Once the rebalance clears the suppressed flag, the
	 * normal EMA-driven growth restores capacity. */
	bool node_overloaded =
	    v8m_page_heap_node_is_suppressed(v8m_numa_current_node());
	for (uint32_t cls = 0; cls < V8M_MEDIUM_FIRST_CLASS; cls++) {
		uint16_t allocs = cache->alloc_count_per_class[cls];
		uint16_t frees = cache->free_count_per_class[cls];
		uint16_t demand =
		    (allocs > frees) ? (uint16_t)(allocs - frees) : 0U;
		/* EMA update: ema_new = (3 * ema_old + demand) / 4
		 * (α = 0.25). Promoted to uint32_t to avoid the
		 * intermediate overflow when ema_old approaches the
		 * MAX-capacity bound. */
		uint32_t ema = ((uint32_t)cache->ema_demand[cls] * 3U +
				(uint32_t)demand) >>
			       2U;
		if (ema > UINT16_MAX) {
			ema = UINT16_MAX;
		}
		cache->ema_demand[cls] = (uint16_t)ema;

		uint32_t new_cap = ema * 2U;
		if (node_overloaded) {
			new_cap >>= 1U;
		}
		if (new_cap < V8M_BIN_CAPACITY_MIN) {
			new_cap = V8M_BIN_CAPACITY_MIN;
		}
		if (new_cap > V8M_BIN_CAPACITY_MAX) {
			new_cap = V8M_BIN_CAPACITY_MAX;
		}
		cache->bin_capacity[cls] = (uint16_t)new_cap;

		cache->alloc_count_per_class[cls] = 0;
		cache->free_count_per_class[cls] = 0;
	}
	cache->gc_generation++;
	cache->gc_countdown = V8M_TLC_GC_INTERVAL;
}

size_t v8m_thread_cache_flush_half(struct v8m_thread_cache *cache,
				   struct v8m_slab_pool *pool, uint32_t cls)
{
	if (cache == NULL || pool == NULL || cls >= V8M_MEDIUM_FIRST_CLASS) {
		return 0;
	}
	uint16_t flush_count = (uint16_t)((cache->bin_count[cls] + 1U) / 2U);
	size_t freed = 0;
	for (uint16_t i = 0; i < flush_count; i++) {
		void *obj = cache->bin_heads[cls];
		if (obj == NULL) {
			break;
		}
		void *next = NULL;
		(void)memcpy((void *)&next, obj, sizeof(next));
		cache->bin_heads[cls] = next;
		cache->bin_count[cls]--;
		/* Recover the meta from the object pointer; the slab
		 * pool's free path needs a v8m_page_meta. The page is
		 * still owned by the slab pool — we never zero its
		 * magic during caching — so v8m_ptr_to_meta + the
		 * slab_pool_free's own validation cover correctness. */
		struct v8m_page_meta *meta = v8m_ptr_to_meta(obj);
		(void)v8m_slab_pool_free(pool, meta, obj);
		freed++;
	}
	return freed;
}

size_t v8m_thread_cache_drain_all(struct v8m_thread_cache *cache,
				  struct v8m_slab_pool *pool)
{
	if (cache == NULL || pool == NULL) {
		return 0;
	}
	size_t total = 0;
	for (uint32_t cls = 0; cls < V8M_MEDIUM_FIRST_CLASS; cls++) {
		while (cache->bin_heads[cls] != NULL) {
			total += v8m_thread_cache_flush_half(cache, pool, cls);
		}
	}
	return total;
}

size_t v8m_thread_cache_drain_chain(struct v8m_thread_cache *cache,
				    uint32_t cls, size_t count, void **out_head,
				    void **out_tail)
{
	if (out_head != NULL) {
		*out_head = NULL;
	}
	if (out_tail != NULL) {
		*out_tail = NULL;
	}
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS || count == 0U ||
	    out_head == NULL || out_tail == NULL) {
		return 0;
	}

	void *first = v8m_thread_cache_alloc(cache, cls);
	if (first == NULL) {
		return 0;
	}
	void *tail = first;
	size_t got = 1;
	while (got < count) {
		void *node = v8m_thread_cache_alloc(cache, cls);
		if (node == NULL) {
			break;
		}
		(void)memcpy(tail, (const void *)&node, sizeof(node));
		tail = node;
		got++;
	}
	void *terminator = NULL;
	(void)memcpy(tail, (const void *)&terminator, sizeof(terminator));
	*out_head = first;
	*out_tail = tail;
	return got;
}

void v8m_thread_cache_install_chain(struct v8m_thread_cache *cache,
				    uint32_t cls, void *head, void *tail,
				    size_t count)
{
	if (cache == NULL || head == NULL || tail == NULL ||
	    cls >= V8M_MEDIUM_FIRST_CLASS || count == 0U) {
		return;
	}
	/* Splice the chain at the head of the bin: tail->next =
	 * existing bin head, then bin_heads[cls] = chain head. */
	void *prev_head = cache->bin_heads[cls];
	(void)memcpy(tail, (const void *)&prev_head, sizeof(prev_head));
	cache->bin_heads[cls] = head;
	cache->bin_count[cls] = (uint16_t)(cache->bin_count[cls] + count);
}

size_t v8m_thread_cache_drain_remote(struct v8m_thread_cache *cache)
{
	if (cache == NULL) {
		return 0;
	}
	struct v8m_mpsc_node *node = v8m_mpsc_drain(&cache->remote);
	size_t count = 0;
	while (node != NULL) {
		/* Read `next` BEFORE pushing onto the local bin —
		 * v8m_thread_cache_free overwrites the first 8 bytes
		 * of the slot with the bin's `next` pointer, which
		 * would clobber the MPSC chain link if we read it
		 * after. */
		struct v8m_mpsc_node *next =
		    atomic_load_explicit(&node->next, memory_order_relaxed);
		const struct v8m_page_meta *meta = v8m_ptr_to_meta(node);
		if (meta != NULL && v8m_page_meta_valid(meta) &&
		    meta->size_class < V8M_MEDIUM_FIRST_CLASS) {
			(void)v8m_thread_cache_free(cache, meta->size_class,
						    node);
		}
		node = next;
		count++;
	}
	return count;
}

int v8m_thread_cache_module_init(void)
{
	if (atomic_load_explicit(&g_key_initialized, memory_order_acquire)) {
		return 0;
	}
	int ret = pthread_key_create(&g_destructor_key, destroy_cache);
	if (ret != 0) {
		return ret;
	}
	atomic_store_explicit(&g_key_initialized, true, memory_order_release);
	/* If the calling thread already touched the cache before the
	 * key existed (extremely rare — only possible via reentrant
	 * malloc during dispatch init), retroactively register it so
	 * its destructor fires too. */
	if (t_cache != NULL) {
		(void)pthread_setspecific(g_destructor_key, t_cache);
	}
	return 0;
}

void v8m_thread_cache_module_shutdown(void)
{
	if (!atomic_load_explicit(&g_key_initialized, memory_order_acquire)) {
		return;
	}
	/* Clear our own TLS so a post-shutdown destructor invocation
	 * (the runtime may iterate keys multiple times) does not see
	 * a stale pointer. The destructor itself nulls t_cache, but
	 * doing it here covers the case where the calling thread's
	 * cache was registered with the now-deleted key. */
	struct v8m_thread_cache *cache = t_cache;
	t_cache = NULL;
	if (cache != NULL) {
		registry_unregister_and_fold(cache);
		free(cache);
	}
	(void)pthread_key_delete(g_destructor_key);
	atomic_store_explicit(&g_key_initialized, false, memory_order_release);
}

uint64_t v8m_thread_cache_destructor_calls(void)
{
	return atomic_load_explicit(&g_destructor_calls, memory_order_relaxed);
}

void v8m_thread_cache_record_alloc(uint32_t cls, size_t request_size)
{
	struct v8m_thread_cache *cache = t_cache;
	if (cache != NULL && cache->initialized != 0U) {
		/* Sample-rate cadence: only every Nth observation lands.
		 * Increment unconditionally so the cadence holds; the
		 * AND mask collapses the predicate to a single branch on
		 * the histogram's hot exit. */
		cache->histogram_tick++;
		if ((cache->histogram_tick &
		     (V8M_HISTOGRAM_SAMPLE_RATE - 1U)) != 0U) {
			return;
		}
		if (cls < V8M_NUM_SIZE_CLASSES) {
			cache->request_count[cls]++;
			cache->request_bytes[cls] += (uint64_t)request_size;
		} else {
			cache->huge_request_count++;
			cache->huge_request_bytes += (uint64_t)request_size;
		}
		return;
	}
	/* No TLC available: fall back to the global counters. The path
	 * is rare (bootstrap chain + signal-safe), so the relaxed
	 * atomic adds do not show up on profiles. */
	uint64_t tick = atomic_fetch_add_explicit(&g_global_histogram_tick, 1U,
						  memory_order_relaxed) +
			1U;
	if ((tick & (V8M_HISTOGRAM_SAMPLE_RATE - 1U)) != 0U) {
		return;
	}
	if (cls < V8M_NUM_SIZE_CLASSES) {
		atomic_fetch_add_explicit(&g_global_request_count[cls], 1U,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&g_global_request_bytes[cls],
					  (uint64_t)request_size,
					  memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&g_global_huge_request_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&g_global_huge_request_bytes,
					  (uint64_t)request_size,
					  memory_order_relaxed);
	}
}

void v8m_thread_cache_aggregate_histogram(struct v8m_size_class_histogram *out)
{
	if (out == NULL) {
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	for (uint32_t cls = 0; cls < V8M_NUM_SIZE_CLASSES; cls++) {
		out->request_count[cls] = atomic_load_explicit(
		    &g_global_request_count[cls], memory_order_relaxed);
		out->request_bytes[cls] = atomic_load_explicit(
		    &g_global_request_bytes[cls], memory_order_relaxed);
	}
	out->huge_request_count = atomic_load_explicit(
	    &g_global_huge_request_count, memory_order_relaxed);
	out->huge_request_bytes = atomic_load_explicit(
	    &g_global_huge_request_bytes, memory_order_relaxed);

	(void)pthread_mutex_lock(&g_registry_lock);
	for (struct v8m_thread_cache *cache = g_registry_head; cache != NULL;
	     cache = cache->registry_next) {
		for (uint32_t cls = 0; cls < V8M_NUM_SIZE_CLASSES; cls++) {
			out->request_count[cls] += cache->request_count[cls];
			out->request_bytes[cls] += cache->request_bytes[cls];
		}
		out->huge_request_count += cache->huge_request_count;
		out->huge_request_bytes += cache->huge_request_bytes;
	}
	(void)pthread_mutex_unlock(&g_registry_lock);
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void v8m_thread_cache_lifetime_record_alloc(void *ptr, const void *caller_pc,
					    uint64_t tsc)
{
	if (ptr == NULL || tsc == 0U) {
		return;
	}
	if (v8m_config_get(V8M_OPT_LIFETIME_TRACKING) == 0) {
		return;
	}
	struct v8m_thread_cache *cache = t_cache;
	if (cache == NULL || cache->initialized == 0U) {
		/* No TLC available; the very first allocation per thread
		 * lands here (the cache itself is allocated through
		 * malloc). Drop the sample silently — the next allocation
		 * has a TLC. */
		return;
	}
	cache->lifetime_sample_tick++;
	if ((cache->lifetime_sample_tick & (V8M_LIFETIME_SAMPLE_RATE - 1U)) !=
	    0U) {
		return;
	}
	uint32_t pos = cache->lifetime_ring_pos;
	struct v8m_lifetime_ring_entry *entry = &cache->lifetime_ring[pos];
	if (entry->ptr != NULL) {
		cache->lifetime_samples_evicted++;
	}
	entry->ptr = ptr;
	entry->caller_pc = (uintptr_t)caller_pc;
	entry->alloc_tsc = tsc;
	cache->lifetime_ring_pos = (pos + 1U) & (V8M_LIFETIME_RING_SIZE - 1U);
	cache->lifetime_samples_recorded++;
}

void v8m_thread_cache_lifetime_record_free(const void *ptr, uint64_t tsc)
{
	if (ptr == NULL) {
		return;
	}
	if (v8m_config_get(V8M_OPT_LIFETIME_TRACKING) == 0) {
		return;
	}
	struct v8m_thread_cache *cache = t_cache;
	if (cache == NULL || cache->initialized == 0U) {
		return;
	}
	for (uint32_t i = 0; i < V8M_LIFETIME_RING_SIZE; i++) {
		struct v8m_lifetime_ring_entry *entry =
		    &cache->lifetime_ring[i];
		if (entry->ptr != ptr) {
			continue;
		}
		uint64_t alloc_tsc = entry->alloc_tsc;
		uintptr_t caller_pc = entry->caller_pc;
		entry->ptr = NULL;
		entry->caller_pc = 0;
		entry->alloc_tsc = 0;
		if (tsc == 0U || alloc_tsc == 0U || tsc < alloc_tsc) {
			/* Wrap or unknown TSC — bump completed but skip
			 * the EMA / classification update. */
			cache->lifetime_samples_completed++;
			return;
		}
		uint64_t ticks = tsc - alloc_tsc;
		struct v8m_lifetime_bucket *bucket =
		    lifetime_bucket_for(cache, caller_pc);
		if (bucket != NULL) {
			/* α = 0.25, matches the bin-capacity controller. */
			uint64_t prev = bucket->ema_lifetime_ticks;
			bucket->ema_lifetime_ticks =
			    (prev == 0U) ? ticks : ((prev * 3U + ticks) / 4U);
			bucket->sample_count++;
		}
		ensure_lifetime_thresholds();
		classify_and_count(cache, ticks);
		cache->lifetime_samples_completed++;
		return;
	}
}

enum v8m_lifetime_class
v8m_thread_cache_lifetime_classify(const void *caller_pc)
{
	struct v8m_thread_cache *cache = t_cache;
	if (cache == NULL || cache->initialized == 0U) {
		return V8M_LIFETIME_UNKNOWN;
	}
	uintptr_t pc_bits = (uintptr_t)caller_pc;
	if (pc_bits == 0U) {
		pc_bits = 1U;
	}
	uint32_t start =
	    (uint32_t)((pc_bits >> 4U) & (V8M_LIFETIME_BUCKETS - 1U));
	for (uint32_t step = 0; step < V8M_LIFETIME_BUCKETS; step++) {
		uint32_t idx = (start + step) & (V8M_LIFETIME_BUCKETS - 1U);
		const struct v8m_lifetime_bucket *bucket =
		    &cache->lifetime_buckets[idx];
		if (bucket->caller_pc == 0U) {
			return V8M_LIFETIME_UNKNOWN;
		}
		if (bucket->caller_pc != pc_bits) {
			continue;
		}
		if (bucket->sample_count == 0U) {
			return V8M_LIFETIME_UNKNOWN;
		}
		ensure_lifetime_thresholds();
		uint64_t short_thr = atomic_load_explicit(
		    &g_lifetime_short_threshold_ticks, memory_order_relaxed);
		uint64_t long_thr = atomic_load_explicit(
		    &g_lifetime_long_threshold_ticks, memory_order_relaxed);
		uint64_t ema = bucket->ema_lifetime_ticks;
		if (ema < short_thr) {
			return V8M_LIFETIME_EPHEMERAL;
		}
		if (ema < long_thr) {
			return V8M_LIFETIME_SHORT;
		}
		return V8M_LIFETIME_LONG;
	}
	return V8M_LIFETIME_UNKNOWN;
}

void v8m_thread_cache_aggregate_lifetime(struct v8m_lifetime_stats *out)
{
	if (out == NULL) {
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	out->samples_recorded = atomic_load_explicit(
	    &g_global_lifetime_recorded, memory_order_relaxed);
	out->samples_completed = atomic_load_explicit(
	    &g_global_lifetime_completed, memory_order_relaxed);
	out->samples_evicted = atomic_load_explicit(&g_global_lifetime_evicted,
						    memory_order_relaxed);
	out->ephemeral_count = atomic_load_explicit(
	    &g_global_lifetime_ephemeral, memory_order_relaxed);
	out->short_count = atomic_load_explicit(&g_global_lifetime_short,
						memory_order_relaxed);
	out->long_count =
	    atomic_load_explicit(&g_global_lifetime_long, memory_order_relaxed);

	(void)pthread_mutex_lock(&g_registry_lock);
	for (struct v8m_thread_cache *cache = g_registry_head; cache != NULL;
	     cache = cache->registry_next) {
		out->samples_recorded += cache->lifetime_samples_recorded;
		out->samples_completed += cache->lifetime_samples_completed;
		out->samples_evicted += cache->lifetime_samples_evicted;
		out->ephemeral_count += cache->lifetime_ephemeral_count;
		out->short_count += cache->lifetime_short_count;
		out->long_count += cache->lifetime_long_count;
	}
	(void)pthread_mutex_unlock(&g_registry_lock);
}
