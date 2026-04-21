/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Libc fallback implementation. Resolves free/malloc/calloc/realloc
 * via dlsym(RTLD_NEXT, ...) and caches the function pointers.
 * Resolution runs from the library constructor before any user code;
 * dlsym itself may call into our malloc override, but the override
 * routes pre-init allocations to the bootstrap allocator, so the
 * recursion is bounded and crash-free.
 */

#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "v8m_libc_fallback.h"

typedef void (*libc_free_fn)(void *);
typedef void *(*libc_malloc_fn)(size_t);
typedef void *(*libc_calloc_fn)(size_t, size_t);
typedef void *(*libc_realloc_fn)(void *, size_t);

/*
 * Cached function pointers. Reads on the hot path use
 * memory_order_acquire so they pair with the release-store in
 * v8m_libc_fallback_init; writes during init are release-ordered so
 * the reader sees a fully-initialized pointer or NULL.
 */
static _Atomic(libc_free_fn) g_libc_free;
static _Atomic(libc_malloc_fn) g_libc_malloc;
static _Atomic(libc_calloc_fn) g_libc_calloc;
static _Atomic(libc_realloc_fn) g_libc_realloc;
static atomic_bool g_initialized;

void v8m_libc_fallback_init(void)
{
	bool expected = false;
	if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected,
						     true, memory_order_acq_rel,
						     memory_order_acquire)) {
		return; /* already initialized */
	}

	/* dlsym may itself call malloc/calloc; that's served from the
	 * bootstrap allocator since dispatch_ready() is still false at
	 * the point this function runs. */
	atomic_store_explicit(&g_libc_free,
			      (libc_free_fn)dlsym(RTLD_NEXT, "free"),
			      memory_order_release);
	atomic_store_explicit(&g_libc_malloc,
			      (libc_malloc_fn)dlsym(RTLD_NEXT, "malloc"),
			      memory_order_release);
	atomic_store_explicit(&g_libc_calloc,
			      (libc_calloc_fn)dlsym(RTLD_NEXT, "calloc"),
			      memory_order_release);
	atomic_store_explicit(&g_libc_realloc,
			      (libc_realloc_fn)dlsym(RTLD_NEXT, "realloc"),
			      memory_order_release);
}

bool v8m_libc_fallback_ready(void)
{
	return atomic_load_explicit(&g_libc_free, memory_order_acquire) != NULL;
}

void v8m_libc_free(void *ptr)
{
	libc_free_fn func =
	    atomic_load_explicit(&g_libc_free, memory_order_acquire);
	if (func != NULL) {
		func(ptr);
	}
}

void *v8m_libc_malloc(size_t size)
{
	libc_malloc_fn func =
	    atomic_load_explicit(&g_libc_malloc, memory_order_acquire);
	if (func != NULL) {
		return func(size);
	}
	return NULL;
}
