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

#include "v8m_remote_free.h" /* v8m_mpsc_init */

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

static void destroy_cache(void *arg)
{
	struct v8m_thread_cache *cache = arg;
	if (cache == NULL) {
		return;
	}
	t_cache = NULL;
	free(cache);
	atomic_fetch_add_explicit(&g_destructor_calls, 1U,
				  memory_order_relaxed);
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
	cache = malloc(sizeof(*cache));
	if (cache == NULL) {
		return NULL;
	}
	(void)memset(cache, 0, sizeof(*cache));
	v8m_mpsc_init(&cache->remote);
	cache->initialized = 1U;

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
