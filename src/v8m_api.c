/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Public allocation API. Implements the standard malloc family
 * (malloc, free, calloc, realloc, reallocarray, malloc_usable_size)
 * along with the aligned-allocation family (aligned_alloc,
 * posix_memalign, memalign, valloc, pvalloc) and the v8m_-prefixed
 * equivalents, all routing through the single-process v8m_dispatch
 * instance. Library load runs the constructor that initializes the
 * dispatch; library unload runs the destructor that tears it down.
 *
 * Pre-init / post-shutdown allocations fall through to the
 * bootstrap allocator so library constructors that run before us
 * (and any late shutdown allocations) still get serviced. Bootstrap
 * pointers survive the transition: free() recognizes them via the
 * range check and treats them as no-ops.
 */

#include <errno.h>
#include <malloc.h> /* malloc_usable_size prototype (glibc extension) */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v8m_bootstrap.h"
#include "v8m_config.h"
#include "v8m_dispatch.h"
#include "v8malloc/v8malloc.h"

/* 0 = uninitialized, 1 = ready, 2 = shutting down. The constructor
 * sets it to 1 with release ordering; the destructor swaps it back
 * to 0 with acquire ordering before tearing the dispatcher down. */
static atomic_int g_init_state = 0;
static struct v8m_dispatch g_dispatch;

static void abort_with(const char *msg)
{
	(void)write(STDERR_FILENO, msg, strlen(msg));
	abort();
}

__attribute__((constructor(101))) static void v8m_constructor(void)
{
	v8m_config_init();
	if (v8m_dispatch_init(&g_dispatch) != 0) {
		abort_with("v8malloc: dispatch init failed\n");
	}
	atomic_store_explicit(&g_init_state, 1, memory_order_release);
}

__attribute__((destructor(101))) static void v8m_destructor(void)
{
	int prev =
	    atomic_exchange_explicit(&g_init_state, 0, memory_order_acquire);
	if (prev == 1) {
		v8m_dispatch_destroy(&g_dispatch);
	}
}

static bool dispatch_ready(void)
{
	return atomic_load_explicit(&g_init_state, memory_order_acquire) == 1;
}

/* --- v8m_-prefixed API --------------------------------------------- */

V8M_EXPORT void *v8m_malloc(size_t size)
{
	if (!dispatch_ready()) {
		/* Pre-init / post-shutdown — serve from bootstrap.
		 * size == 0 still produces a unique pointer per our
		 * malloc(0) policy. */
		return v8m_bootstrap_alloc(size > 0U ? size : 1U);
	}
	void *ptr = v8m_dispatch_alloc(&g_dispatch, size);
	if (ptr == NULL) {
		errno = ENOMEM;
	}
	return ptr;
}

V8M_EXPORT void v8m_free(void *ptr)
{
	if (ptr == NULL) {
		return;
	}
	if (v8m_ptr_is_bootstrap(ptr)) {
		/* Bootstrap allocations have no per-pointer free path
		 * — they're released only when the buffer is reset,
		 * which never happens in v0. */
		return;
	}
	if (!dispatch_ready()) {
		return;
	}
	v8m_dispatch_free(&g_dispatch, ptr);
}

V8M_EXPORT void *v8m_calloc(size_t nmemb, size_t size)
{
	if (size != 0U && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	size_t total = nmemb * size;
	void *ptr = v8m_malloc(total);
	if (ptr != NULL && total > 0U) {
		(void)memset(ptr, 0, total);
	}
	return ptr;
}

/* `ptr` is non-const to match the POSIX malloc_usable_size(void *)
 * signature even though the implementation never writes through it. */
/* cppcheck-suppress constParameterPointer */
V8M_EXPORT size_t v8m_malloc_usable_size(void *ptr)
{
	if (ptr == NULL) {
		return 0;
	}
	if (v8m_ptr_is_bootstrap(ptr)) {
		/* Bootstrap doesn't track per-allocation sizes; the
		 * caller can use v8m_bootstrap_remaining for an upper
		 * bound on what's safe to read. */
		return 0;
	}
	if (!dispatch_ready()) {
		return 0;
	}
	return v8m_dispatch_usable_size(&g_dispatch, ptr);
}

V8M_EXPORT void *v8m_realloc(void *ptr, size_t size)
{
	if (ptr == NULL) {
		return v8m_malloc(size);
	}
	if (size == 0U) {
		v8m_free(ptr);
		return NULL;
	}

	bool is_bootstrap = v8m_ptr_is_bootstrap(ptr);
	size_t old_usable = is_bootstrap ? v8m_bootstrap_remaining(ptr)
					 : v8m_malloc_usable_size(ptr);

	if (!is_bootstrap && old_usable >= size) {
		return ptr; /* shrink / fits in place */
	}

	void *new_ptr = v8m_malloc(size);
	if (new_ptr == NULL) {
		return NULL;
	}

	size_t copy = (old_usable < size) ? old_usable : size;
	if (copy > 0U) {
		(void)memcpy(new_ptr, ptr, copy);
	}
	v8m_free(ptr);
	return new_ptr;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (size != 0U && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	return v8m_realloc(ptr, nmemb * size);
}

/*
 * is_pow2 — true iff `value` is a non-zero power of two. Used to
 * validate the alignment argument of every aligned-alloc entry.
 */
static bool is_pow2(size_t value)
{
	return value != 0U && (value & (value - 1U)) == 0U;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_aligned_alloc(size_t alignment, size_t size)
{
	if (!is_pow2(alignment)) {
		errno = EINVAL;
		return NULL;
	}
	if (!dispatch_ready()) {
		/* Pre-init allocations cannot honour custom alignment;
		 * the bootstrap pointer is only 16-byte aligned. */
		if (alignment <= 16U) {
			return v8m_bootstrap_alloc(size > 0U ? size : 1U);
		}
		errno = ENOMEM;
		return NULL;
	}
	void *ptr = v8m_dispatch_alloc_aligned(&g_dispatch, size, alignment);
	if (ptr == NULL) {
		errno = ENOMEM;
	}
	return ptr;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (memptr == NULL) {
		return EINVAL;
	}
	/* posix_memalign requires alignment to be a power of two AND a
	 * multiple of sizeof(void *). */
	if (!is_pow2(alignment) || (alignment % sizeof(void *)) != 0U) {
		return EINVAL;
	}
	if (!dispatch_ready()) {
		if (alignment <= 16U) {
			void *ptr = v8m_bootstrap_alloc(size > 0U ? size : 1U);
			if (ptr == NULL) {
				return ENOMEM;
			}
			*memptr = ptr;
			return 0;
		}
		return ENOMEM;
	}
	void *ptr = v8m_dispatch_alloc_aligned(&g_dispatch, size, alignment);
	if (ptr == NULL) {
		return ENOMEM;
	}
	*memptr = ptr;
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_memalign(size_t alignment, size_t size)
{
	/* memalign(3) is the looser glibc cousin of aligned_alloc — it
	 * doesn't require size to be a multiple of alignment. The C11
	 * relaxation made aligned_alloc match this behaviour, so the two
	 * are functionally identical here. */
	return v8m_aligned_alloc(alignment, size);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_valloc(size_t size)
{
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) {
		page = 4096; /* defensive default */
	}
	return v8m_aligned_alloc((size_t)page, size);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_pvalloc(size_t size)
{
	long page_signed = sysconf(_SC_PAGESIZE);
	size_t page = (page_signed > 0) ? (size_t)page_signed : 4096U;
	/* pvalloc rounds size up to the next page boundary. Treat 0 as
	 * one page, matching glibc. */
	if (size == 0U) {
		size = page;
	}
	if (size > SIZE_MAX - (page - 1U)) {
		errno = ENOMEM;
		return NULL;
	}
	size_t rounded = (size + page - 1U) & ~(page - 1U);
	return v8m_aligned_alloc(page, rounded);
}

/* --- POSIX malloc family overrides -------------------------------- */

V8M_EXPORT void *malloc(size_t size)
{
	return v8m_malloc(size);
}

V8M_EXPORT void free(void *ptr)
{
	v8m_free(ptr);
}

V8M_EXPORT void *calloc(size_t nmemb, size_t size)
{
	return v8m_calloc(nmemb, size);
}

V8M_EXPORT void *realloc(void *ptr, size_t size)
{
	return v8m_realloc(ptr, size);
}

V8M_EXPORT void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	return v8m_reallocarray(ptr, nmemb, size);
}

V8M_EXPORT size_t malloc_usable_size(void *ptr)
{
	return v8m_malloc_usable_size(ptr);
}

V8M_EXPORT void *aligned_alloc(size_t alignment, size_t size)
{
	return v8m_aligned_alloc(alignment, size);
}

V8M_EXPORT int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	return v8m_posix_memalign(memptr, alignment, size);
}

V8M_EXPORT void *memalign(size_t alignment, size_t size)
{
	return v8m_memalign(alignment, size);
}

V8M_EXPORT void *valloc(size_t size)
{
	return v8m_valloc(size);
}

V8M_EXPORT void *pvalloc(size_t size)
{
	return v8m_pvalloc(size);
}
