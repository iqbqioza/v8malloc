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
#include <malloc.h> /* mallinfo/mallinfo2/mallopt/malloc_*  glibc extensions */
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v8m_bootstrap.h"
#include "v8m_config.h"
#include "v8m_dispatch.h"
#include "v8m_libc_fallback.h"
#include "v8m_numa.h"
#include "v8m_page_heap.h"
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

/* pthread_atfork wrappers — pthread_atfork takes parameter-less
 * function pointers, so the handlers thunk through to the dispatch
 * helpers using the global g_dispatch instance. Each one early-outs
 * unless the dispatcher is fully initialized; that protects against
 * the (unusual) case where another library forks during a
 * constructor chain that runs before ours. */

static void v8m_atfork_prepare(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 1) {
		v8m_dispatch_prefork(&g_dispatch);
	}
}

static void v8m_atfork_parent(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 1) {
		v8m_dispatch_postfork_parent(&g_dispatch);
	}
}

static void v8m_atfork_child(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 1) {
		v8m_dispatch_postfork_child(&g_dispatch);
	}
}

__attribute__((constructor(101))) static void v8m_constructor(void)
{
	/* Resolve libc fallbacks first — dlsym may itself allocate, and
	 * those calls hit our malloc override before dispatch_ready is
	 * true, falling through to the bootstrap allocator. Doing the
	 * resolution before dispatch_init keeps the recursion bounded
	 * to the bootstrap path. */
	v8m_libc_fallback_init();
	v8m_config_init();
	v8m_numa_init();
	if (v8m_dispatch_init(&g_dispatch) != 0) {
		abort_with("v8malloc: dispatch init failed\n");
	}
	/* Register fork handlers before publishing the ready flag so
	 * that any thread that calls fork() the moment we go live sees
	 * the locks acquired in deterministic order. pthread_atfork
	 * itself may allocate; that goes through bootstrap. */
	if (pthread_atfork(v8m_atfork_prepare, v8m_atfork_parent,
			   v8m_atfork_child) != 0) {
		abort_with("v8malloc: pthread_atfork registration failed\n");
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

/* --- v8m_-prefixed configuration & stats -------------------------- */

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_set_option(int opt, int64_t value)
{
	if (!dispatch_ready()) {
		errno = EAGAIN;
		return -1;
	}
	if (opt < 0 || opt >= V8M_OPT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	if (v8m_config_set((enum v8m_option)opt, value) != 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_get_option(int opt, int64_t *out)
{
	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (opt < 0 || opt >= V8M_OPT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	if (!dispatch_ready()) {
		errno = EAGAIN;
		return -1;
	}
	*out = v8m_config_get((enum v8m_option)opt);
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void v8m_get_stats(struct v8m_stats *out)
{
	if (out == NULL) {
		return;
	}
	struct v8m_page_heap_stats stats = {0};
	size_t live_regions = 0;
	if (dispatch_ready()) {
		v8m_page_heap_get_stats(&stats);
		live_regions = v8m_page_heap_live_region_count();
	}
	out->mmap_calls = stats.mmap_calls;
	out->munmap_calls = stats.munmap_calls;
	out->advise_calls = stats.advise_calls;
	out->bytes_mapped = stats.bytes_mapped;
	out->bytes_unmapped = stats.bytes_unmapped;
	out->live_regions = (uint64_t)live_regions;
	out->live_bytes = stats.bytes_mapped - stats.bytes_unmapped;
}

V8M_EXPORT void v8m_dump_stats(void)
{
	malloc_stats();
}

/* --- glibc statistics / tuning extensions -------------------------- */
/*
 * The functions below are not part of the POSIX core; they exist so
 * that legacy software linked against glibc keeps working when
 * v8malloc replaces the allocator. The numbers we report are
 * derived from the page-heap counters — there's no per-class chunk
 * accounting in v0, so the chunk-level fields stay zero. The
 * picture sharpens once the thread cache + per-class stats land.
 */

/*
 * Live-mmap accounting recovered from the page-heap counters. Used
 * by mallinfo / mallinfo2 / malloc_info / malloc_stats so they
 * agree on the same snapshot. Counters are monotonic so subtraction
 * is safe; pre-init / post-shutdown returns all zeroes.
 */
struct v8m_live_stats {
	uint64_t live_regions;
	uint64_t live_bytes;
	uint64_t bytes_mapped;
	uint64_t bytes_unmapped;
	uint64_t mmap_calls;
	uint64_t munmap_calls;
	uint64_t advise_calls;
};

static void v8m_collect_live_stats(struct v8m_live_stats *out)
{
	struct v8m_page_heap_stats stats = {0};
	size_t live_regions = 0;
	if (dispatch_ready()) {
		v8m_page_heap_get_stats(&stats);
		live_regions = v8m_page_heap_live_region_count();
	}
	out->bytes_mapped = stats.bytes_mapped;
	out->bytes_unmapped = stats.bytes_unmapped;
	out->mmap_calls = stats.mmap_calls;
	out->munmap_calls = stats.munmap_calls;
	out->advise_calls = stats.advise_calls;
	/* live_regions comes from the region map directly; the
	 * mmap/munmap call counters cannot be used because the
	 * over-allocate-and-trim strategy emits multiple munmaps
	 * per mmap, leaving the call-count difference net-negative. */
	out->live_regions = (uint64_t)live_regions;
	out->live_bytes = stats.bytes_mapped - stats.bytes_unmapped;
}

/* `int` truncation is intentional — the legacy mallinfo struct
 * predates 64-bit address spaces. Modern code should call mallinfo2
 * instead; mallinfo lives on for ABI compatibility. */
V8M_EXPORT struct mallinfo mallinfo(void)
{
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	struct mallinfo info = {0};
	info.hblks = (int)live.live_regions;
	info.hblkhd = (int)live.live_bytes;
	info.arena = (int)live.live_bytes;
	info.uordblks = (int)live.live_bytes;
	return info;
}

V8M_EXPORT struct mallinfo2 mallinfo2(void)
{
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	struct mallinfo2 info = {0};
	info.hblks = live.live_regions;
	info.hblkhd = live.live_bytes;
	info.arena = live.live_bytes;
	info.uordblks = live.live_bytes;
	return info;
}

V8M_EXPORT void malloc_stats(void)
{
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	(void)fprintf(stderr,
		      "v8malloc statistics (page-heap layer):\n"
		      "  mmap   calls: %llu\n"
		      "  munmap calls: %llu\n"
		      "  advise calls: %llu\n"
		      "  bytes mapped:   %llu\n"
		      "  bytes unmapped: %llu\n"
		      "  live regions:   %llu\n"
		      "  live bytes:     %llu\n",
		      (unsigned long long)live.mmap_calls,
		      (unsigned long long)live.munmap_calls,
		      (unsigned long long)live.advise_calls,
		      (unsigned long long)live.bytes_mapped,
		      (unsigned long long)live.bytes_unmapped,
		      (unsigned long long)live.live_regions,
		      (unsigned long long)live.live_bytes);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int malloc_info(int options, FILE *stream)
{
	(void)options; /* Reserved for future per-arena breakdowns. */
	if (stream == NULL) {
		errno = EINVAL;
		return -1;
	}
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	(void)fprintf(
	    stream,
	    "<malloc version=\"v8malloc-%s\">\n"
	    "  <heap nr=\"0\">\n"
	    "    <total type=\"mmap\" count=\"%llu\" size=\"%llu\"/>\n"
	    "    <system type=\"current\" size=\"%llu\"/>\n"
	    "    <aspace type=\"total\" size=\"%llu\"/>\n"
	    "  </heap>\n"
	    "</malloc>\n",
	    V8M_VERSION_STRING, (unsigned long long)live.live_regions,
	    (unsigned long long)live.live_bytes,
	    (unsigned long long)live.live_bytes,
	    (unsigned long long)live.bytes_mapped);
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
/* The mallopt(int, int) signature is fixed by glibc's ABI, so the
 * "easily swappable" linter check is irrelevant here. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
V8M_EXPORT int mallopt(int param, int value)
{
	(void)param;
	(void)value;
	/* The glibc parameter ids (M_TRIM_THRESHOLD, M_MMAP_MAX, ...)
	 * don't map cleanly to v8malloc's internals — runtime
	 * configuration lives behind the V8M_* environment variables
	 * and the v8m_config_set/get pair. mallopt's role here is
	 * pure ABI compatibility for legacy code that calls it
	 * unconditionally. Per the contract, returning 1 means
	 * "the parameter was accepted". */
	return 1;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int malloc_trim(size_t pad)
{
	(void)pad;
	/* No-op for v0: the page heap returns regions to the kernel
	 * via munmap as they drain; there is no separate "top chunk"
	 * to release. Returning 0 honestly reports that no memory
	 * was given back as a direct result of this call. */
	return 0;
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

/* --- glibc internal aliases --------------------------------------- */
/*
 * glibc's internal call sites — and any code linked against
 * libc_nonshared.a that bypasses the public symbols — reach the
 * allocator through the __libc_* prefixed names. Defining them as
 * aliases of our public implementations interposes on those call
 * sites too. See architecture.md §3.3. The names are reserved by
 * the implementation per C, but that is precisely why glibc uses
 * them; the linter's reserved-identifier / readability rules are
 * irrelevant here, hence the blanket NOLINTs. */
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 * readability-identifier-naming,bugprone-easily-swappable-parameters) */
V8M_EXPORT void *__libc_malloc(size_t size)
    __attribute__((alias("v8m_malloc")));
V8M_EXPORT void __libc_free(void *ptr) __attribute__((alias("v8m_free")));
V8M_EXPORT void *__libc_calloc(size_t nmemb, size_t size)
    __attribute__((alias("v8m_calloc")));
V8M_EXPORT void *__libc_realloc(void *ptr, size_t size)
    __attribute__((alias("v8m_realloc")));
V8M_EXPORT void *__libc_memalign(size_t alignment, size_t size)
    __attribute__((alias("v8m_memalign")));
V8M_EXPORT void *__libc_valloc(size_t size)
    __attribute__((alias("v8m_valloc")));
V8M_EXPORT void *__libc_pvalloc(size_t size)
    __attribute__((alias("v8m_pvalloc")));
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 * readability-identifier-naming,bugprone-easily-swappable-parameters) */
