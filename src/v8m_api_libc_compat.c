/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libc / glibc compatibility surface — split out of v8m_api.c.
 * Three groups live here:
 *
 *   1. glibc statistics / tuning extensions (mallinfo, mallinfo2,
 *      malloc_stats, malloc_info, mallopt, malloc_trim) — required
 *      so legacy software that calls them keeps working when
 *      v8malloc replaces the allocator. Numbers are derived from
 *      the page-heap counters via the shared
 *      v8m_api_collect_live_stats helper (defined in v8m_api.c)
 *      so every reporter agrees.
 *
 *   2. POSIX malloc-family overrides (malloc, free, calloc, realloc,
 *      reallocarray, malloc_usable_size, aligned_alloc,
 *      posix_memalign, memalign, valloc, pvalloc) — pure thunks to
 *      the v8m_*-namespaced equivalents. Defined here as separate
 *      functions instead of `__attribute__((alias))` so each one
 *      gets its own caller-PC for the predictive prefetch path
 *      and the lifetime tracker.
 *
 *   3. glibc internal aliases (__libc_malloc / __libc_free / ...)
 *      — interpose on glibc-internal call sites that bypass the
 *      public symbols. Defined as `__attribute__((alias))` to
 *      keep the caller PC identical to the v8m_* implementation.
 *
 * Public ABI: every symbol below is exported under V8MALLOC_1.0
 * via `v8malloc.map`. Splitting them into a separate TU does not
 * change the symbol set or ordering.
 */

#include <errno.h>
#include <malloc.h> /* mallinfo / mallinfo2 / mallopt / malloc_* */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* posix_memalign prototype */

#include "v8m_api_internal.h"
#include "v8m_large.h"
#include "v8m_page_heap.h"
#include "v8malloc/v8malloc.h"

/* --- glibc statistics / tuning extensions -------------------------- */

/* `int` truncation is intentional — the legacy mallinfo struct
 * predates 64-bit address spaces. Modern code should call mallinfo2
 * instead; mallinfo lives on for ABI compatibility. */
/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT struct mallinfo mallinfo(void)
{
	struct v8m_live_stats live;
	v8m_api_collect_live_stats(&live);
	struct mallinfo info = {0};
	info.hblks = (int)live.live_regions;
	info.hblkhd = (int)live.live_bytes;
	info.arena = (int)live.live_bytes;
	info.uordblks = (int)live.live_bytes;
	return info;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT struct mallinfo2 mallinfo2(void)
{
	struct v8m_live_stats live;
	v8m_api_collect_live_stats(&live);
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
	v8m_api_collect_live_stats(&live);
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
	v8m_api_collect_live_stats(&live);
	struct v8m_page_heap_stats ph_stats = {0};
	struct v8m_large_stats large_stats = {0};
	if (v8m_api_dispatch_ready()) {
		v8m_page_heap_get_stats(&ph_stats);
		v8m_large_get_stats(&large_stats);
	}
	uint64_t vmas = v8m_count_vmas();
	(void)fprintf(
	    stream,
	    "<malloc version=\"v8malloc-%s\">\n"
	    "  <heap nr=\"0\">\n"
	    "    <total type=\"mmap\" count=\"%llu\" size=\"%llu\"/>\n"
	    "    <system type=\"current\" size=\"%llu\"/>\n"
	    "    <aspace type=\"total\" size=\"%llu\"/>\n"
	    "    <large alloc=\"%llu\" free=\"%llu\" bytes_in_use=\"%llu\"/>\n"
	    "    <huge alloc=\"%llu\" free=\"%llu\" bytes_in_use=\"%llu\"/>\n"
	    "    <hugetlb attempts=\"%llu\" failures=\"%llu\"/>\n"
	    "    <gigantic attempts=\"%llu\" failures=\"%llu\"/>\n"
	    "    <hugepage_advise calls=\"%llu\"/>\n"
	    "    <mbind calls=\"%llu\" failures=\"%llu\"/>\n"
	    "    <vma count=\"%llu\"/>\n"
	    "  </heap>\n"
	    "</malloc>\n",
	    V8M_VERSION_STRING, (unsigned long long)live.live_regions,
	    (unsigned long long)live.live_bytes,
	    (unsigned long long)live.live_bytes,
	    (unsigned long long)live.bytes_mapped,
	    (unsigned long long)large_stats.large_alloc_count,
	    (unsigned long long)large_stats.large_free_count,
	    (unsigned long long)large_stats.large_bytes_in_use,
	    (unsigned long long)large_stats.huge_alloc_count,
	    (unsigned long long)large_stats.huge_free_count,
	    (unsigned long long)large_stats.huge_bytes_in_use,
	    (unsigned long long)ph_stats.hugetlb_alloc_calls,
	    (unsigned long long)ph_stats.hugetlb_alloc_failures,
	    (unsigned long long)ph_stats.gigantic_alloc_calls,
	    (unsigned long long)ph_stats.gigantic_alloc_failures,
	    (unsigned long long)ph_stats.hugepage_advise_calls,
	    (unsigned long long)ph_stats.mbind_calls,
	    (unsigned long long)ph_stats.mbind_failures,
	    (unsigned long long)vmas);
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
 * allocator through the __libc_* prefixed names. Defined as thin
 * wrappers (not `__attribute__((alias))`) because alias must
 * point to a same-TU definition and the v8m_* implementations
 * live in v8m_api.c. The extra indirection is one tail-call jump,
 * negligible compared to the alloc cost itself. See
 * architecture.md §3.3. */
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 * readability-identifier-naming,bugprone-easily-swappable-parameters) */
/* Forward declarations satisfy -Wmissing-prototypes for the __libc_*
 * names (no system header declares them — they are reserved-name
 * extensions glibc uses internally). */
V8M_EXPORT void *__libc_malloc(size_t size);
V8M_EXPORT void __libc_free(void *ptr);
V8M_EXPORT void *__libc_calloc(size_t nmemb, size_t size);
V8M_EXPORT void *__libc_realloc(void *ptr, size_t size);
V8M_EXPORT void *__libc_memalign(size_t alignment, size_t size);
V8M_EXPORT void *__libc_valloc(size_t size);
V8M_EXPORT void *__libc_pvalloc(size_t size);

V8M_EXPORT void *__libc_malloc(size_t size)
{
	return v8m_malloc(size);
}
V8M_EXPORT void __libc_free(void *ptr)
{
	v8m_free(ptr);
}
V8M_EXPORT void *__libc_calloc(size_t nmemb, size_t size)
{
	return v8m_calloc(nmemb, size);
}
V8M_EXPORT void *__libc_realloc(void *ptr, size_t size)
{
	return v8m_realloc(ptr, size);
}
V8M_EXPORT void *__libc_memalign(size_t alignment, size_t size)
{
	return v8m_memalign(alignment, size);
}
V8M_EXPORT void *__libc_valloc(size_t size)
{
	return v8m_valloc(size);
}
V8M_EXPORT void *__libc_pvalloc(size_t size)
{
	return v8m_pvalloc(size);
}
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 * readability-identifier-naming,bugprone-easily-swappable-parameters) */
