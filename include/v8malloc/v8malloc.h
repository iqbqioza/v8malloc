/* SPDX-License-Identifier: Apache-2.0 */
/*
 * v8malloc — high-performance memory allocator for Linux.
 *
 * This header is the sole public entry point. Consumers include it as
 *
 *	#include <v8malloc/v8malloc.h>
 *
 * The library may be used in three ways:
 *
 *  1. LD_PRELOAD interposition — every malloc/free in the process is
 *     transparently routed through v8malloc with no source changes.
 *  2. Direct linking against libv8malloc.so / libv8malloc.a — replaces
 *     the libc allocator at link time.
 *  3. Side-by-side use via the namespaced v8m_* API below — useful when
 *     mixing allocators is required (the default libc allocator stays
 *     in place for everything else).
 *
 * See man v8malloc(3) for the full surface; the design is documented in
 * the project's .claude/docs/ tree.
 */

#ifndef V8MALLOC_V8MALLOC_H
#define V8MALLOC_V8MALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Compile-time version ------------------------------------------------ */

#define V8M_VERSION_MAJOR 0
#define V8M_VERSION_MINOR 1
#define V8M_VERSION_PATCH 0

#define V8M_VERSION_STRING "0.1.0"

/*
 * Numeric form: (major * 10000) + (minor * 100) + patch. Useful for
 * preprocessor comparisons such as `#if V8M_VERSION >= 10200`.
 */
#define V8M_VERSION                                                            \
	((V8M_VERSION_MAJOR * 10000) + (V8M_VERSION_MINOR * 100) +             \
	 V8M_VERSION_PATCH)

/* --- Symbol visibility --------------------------------------------------- */

#if defined(V8MALLOC_BUILDING)
#define V8M_EXPORT __attribute__((visibility("default")))
#else
#define V8M_EXPORT
#endif

/* --- Runtime version API ------------------------------------------------- */

/*
 * Return the version string the library was compiled with, e.g. "0.1.0".
 * The returned pointer has static storage duration; callers must not
 * free or modify it.
 */
V8M_EXPORT const char *v8m_version(void);

V8M_EXPORT int v8m_version_major(void);
V8M_EXPORT int v8m_version_minor(void);
V8M_EXPORT int v8m_version_patch(void);

/* --- Allocation API (v8m_-prefixed, namespaced) ------------------- */

#include <stddef.h>

/*
 * Namespaced allocation API. The standard `malloc`, `free`,
 * `calloc`, `realloc`, `reallocarray`, and `malloc_usable_size`
 * symbols are also exported by the library and route to the same
 * implementations; consumers get those prototypes from <stdlib.h>
 * and <malloc.h> as usual. The v8m_* names exist so a program can
 * call into v8malloc explicitly even when the standard symbols are
 * resolved to a different allocator.
 */
V8M_EXPORT void *v8m_malloc(size_t size);
V8M_EXPORT void v8m_free(void *ptr);
V8M_EXPORT void *v8m_calloc(size_t nmemb, size_t size);
V8M_EXPORT void *v8m_realloc(void *ptr, size_t size);
V8M_EXPORT void *v8m_reallocarray(void *ptr, size_t nmemb, size_t size);
V8M_EXPORT size_t v8m_malloc_usable_size(void *ptr);

/*
 * Aligned-allocation family. Same routing as v8m_malloc but the
 * returned pointer is `alignment`-aligned. `alignment` must be a
 * power of two; `posix_memalign` additionally requires `alignment`
 * to be a multiple of sizeof(void *). The largest alignment v0
 * supports is V8M_BUDDY_MAX_BLOCK (256 KiB) for size requests in
 * the slab/buddy range and V8M_PAGE_SIZE / 2 (32 KiB) for larger
 * sizes — requests beyond that return NULL with errno = EINVAL.
 */
V8M_EXPORT void *v8m_aligned_alloc(size_t alignment, size_t size);
V8M_EXPORT int v8m_posix_memalign(void **memptr, size_t alignment, size_t size);
V8M_EXPORT void *v8m_memalign(size_t alignment, size_t size);
V8M_EXPORT void *v8m_valloc(size_t size);
V8M_EXPORT void *v8m_pvalloc(size_t size);

/* --- Runtime configuration -------------------------------------- */

#include <stdint.h>

/*
 * Tunables. Values are seeded from the V8M_* environment variables
 * at library init (see man v8malloc(3)) and can be overridden at
 * runtime through v8m_set_option. The integer ids are stable
 * across v0; new options may be appended within a major release.
 */
enum v8m_option {
	V8M_OPT_VERBOSE = 0,	   /* 0 / 1 — emit diagnostics to stderr */
	V8M_OPT_PURGE_INTERVAL,	   /* seconds between background purges */
	V8M_OPT_THREAD_CACHE_MAX,  /* max objects held per TLC bin */
	V8M_OPT_HUGE_PAGES,	   /* 0 / 1 — try MAP_HUGETLB / MADV_HUGEPAGE */
	V8M_OPT_NUMA_AWARE,	   /* 0 / 1 — bind allocations to local node */
	V8M_OPT_DEBUG,		   /* 0 / 1 — guard pages, double-free checks */
	V8M_OPT_PROFILE,	   /* 0 / 1 — emit allocation profile */
	V8M_OPT_COMPACT_THRESHOLD, /* page utilization % below which a
				    * page becomes a compaction candidate */
	V8M_OPT_COUNT
};

/*
 * Set the tunable identified by `opt` to `value`. Returns 0 on
 * success or -1 with errno set to EINVAL if `opt` is out-of-range.
 * Pre-init (before our constructor runs) the call always returns
 * -1 with EAGAIN — callers should defer until after main() starts
 * or set the corresponding V8M_* environment variable.
 */
V8M_EXPORT int v8m_set_option(int opt, int64_t value);

/*
 * Read a tunable into `*out`. Same return semantics as
 * v8m_set_option. `out` must be non-NULL.
 */
V8M_EXPORT int v8m_get_option(int opt, int64_t *out);

/* --- Runtime statistics ----------------------------------------- */

/*
 * Snapshot of allocator-wide counters. All fields are monotonic
 * across the process lifetime except `live_regions` and
 * `live_bytes`, which reflect the current set of mmap'd regions.
 */
struct v8m_stats {
	uint64_t mmap_calls;
	uint64_t munmap_calls;
	uint64_t advise_calls;
	uint64_t bytes_mapped;
	uint64_t bytes_unmapped;
	uint64_t live_regions;
	uint64_t live_bytes;
};

/*
 * Snapshot the statistics into `*out`. Tolerates a NULL pointer
 * by no-op'ing. Pre-init returns all zeroes.
 */
V8M_EXPORT void v8m_get_stats(struct v8m_stats *out);

/*
 * Print a human-readable summary to stderr — equivalent to
 * malloc_stats(), kept under the v8m_ namespace so users can
 * call it explicitly without relying on glibc's deprecated
 * mallinfo path.
 */
V8M_EXPORT void v8m_dump_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* V8MALLOC_V8MALLOC_H */
