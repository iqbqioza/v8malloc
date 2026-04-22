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

#include "v8m_page.h" /* v8m_ptr_to_meta — meta recovery on flush */
#include "v8m_remote_free.h" /* v8m_mpsc_init */
#include "v8m_size_class.h" /* V8M_MEDIUM_FIRST_CLASS */
#include "v8m_slab_pool.h" /* v8m_slab_pool_free on overflow flush */

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
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		v8m_thread_cache_drain_hook hook =
		    (v8m_thread_cache_drain_hook)hook_raw;
		hook(cache);
	}
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
	cache = malloc(sizeof(*cache));
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

	t_cache = cache;
	if (atomic_load_explicit(&g_key_initialized, memory_order_acquire)) {
		/* Best-effort: a non-zero return here just means the
		 * destructor won't fire on thread exit (the cache then
		 * leaks at thread exit, not at process exit). The
		 * allocator stays usable. */
		(void)pthread_setspecific(g_destructor_key, cache);
	}
	return cache;
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
	 * is 8 bytes on every supported arch). */
	void *next = NULL;
	(void)memcpy(&next, head, sizeof(next));
	cache->bin_heads[cls] = next;
	cache->bin_count[cls]--;
	return head;
}

bool v8m_thread_cache_free(struct v8m_thread_cache *cache, uint32_t cls,
			   void *obj)
{
	if (cache == NULL || cls >= V8M_MEDIUM_FIRST_CLASS || obj == NULL) {
		return false;
	}
	void *prev_head = cache->bin_heads[cls];
	(void)memcpy(obj, &prev_head, sizeof(prev_head));
	cache->bin_heads[cls] = obj;
	cache->bin_count[cls]++;
	return cache->bin_count[cls] >= cache->bin_capacity[cls];
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
		(void)memcpy(&next, obj, sizeof(next));
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
		free(cache);
	}
	(void)pthread_key_delete(g_destructor_key);
	atomic_store_explicit(&g_key_initialized, false, memory_order_release);
}

uint64_t v8m_thread_cache_destructor_calls(void)
{
	return atomic_load_explicit(&g_destructor_calls, memory_order_relaxed);
}
