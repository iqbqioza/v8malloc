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

#include <stdbool.h>
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
	V8M_OPT_VERBOSE = 0, /* 0 / 1 — emit diagnostics to stderr */
	V8M_OPT_PURGE_INTERVAL, /* seconds between background purges */
	V8M_OPT_THREAD_CACHE_MAX, /* max objects held per TLC bin */
	V8M_OPT_HUGE_PAGES, /* 0 / 1 — try MAP_HUGETLB / MADV_HUGEPAGE */
	V8M_OPT_NUMA_AWARE, /* 0 / 1 — bind allocations to local node */
	V8M_OPT_DEBUG, /* 0 / 1 — guard pages, double-free checks */
	V8M_OPT_PROFILE, /* 0 / 1 — emit allocation profile */
	V8M_OPT_COMPACT_THRESHOLD, /* page utilization % below which a
				    * page becomes a compaction candidate */
	V8M_OPT_VMA_WARN_THRESHOLD, /* /proc/self/maps line count above
				     * which the bg purge thread emits a
				     * one-line warning to stderr; 0
				     * disables. Default 1024. */
	V8M_OPT_NUMA_AGGRESSIVE_MIGRATION, /* 0 / 1 — when on, the per-thread
					    * cache's GC tick checks if the
					    * calling thread's NUMA node has
					    * changed since the last tick and
					    * issues `move_pages()` to
					    * relocate the cached free slots'
					    * pages to the new node. Off by
					    * default — relocation is
					    * expensive and only useful for
					    * threads with locality-sensitive
					    * working sets. */
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

/*
 * Per-class breakdown for the Large/Huge direct-mmap path. The
 * `*_alloc_count` / `*_free_count` fields are monotonic across
 * the process lifetime; `live_count = alloc - free`.
 * `*_bytes_in_use` is the current sum of mmap_size for live
 * allocations of that class — useful for sizing huge-page
 * reservations (huge-pages.md §8).
 */
struct v8m_huge_stats {
	uint64_t large_alloc_count;
	uint64_t large_free_count;
	uint64_t large_bytes_in_use;
	uint64_t huge_alloc_count;
	uint64_t huge_free_count;
	uint64_t huge_bytes_in_use;
};

/*
 * Snapshot the Large/Huge counters into `*out`. Tolerates NULL.
 * Pre-init returns all zeroes.
 */
V8M_EXPORT void v8m_get_huge_stats(struct v8m_huge_stats *out);

/*
 * Per-thread allocator stats. Reports cache hit/miss counters and
 * remote-free queue activity for the calling thread. v0 has no
 * thread cache yet, so every field reads as zero — the public
 * surface lands now to lock the contract.
 */
struct v8m_thread_stats {
	uint64_t fast_path_allocs;
	uint64_t slow_path_allocs;
	uint64_t fast_path_frees;
	uint64_t remote_frees_received;
	uint64_t bin_overflow_flushes;
};

/*
 * Snapshot the calling thread's stats. Tolerates NULL.
 */
V8M_EXPORT void v8m_get_thread_stats(struct v8m_thread_stats *out);

/*
 * Fragmentation snapshot. Reports the page-heap-derived metrics plus
 * the aggregate slab utilization across every Tiny/Small class. The
 * slab counters cover pages currently held in the per-class
 * `current` and `partials` lists — empty pages are returned to the
 * page heap on free, and full pages are off the lists by design, so
 * the walk reports the actively-partitioned population that drives
 * operational utilization decisions (fragmentation.md §4.1).
 * Per-class breakdown is a follow-up API once the per-class field
 * group warrants its own struct.
 */
struct v8m_frag_metrics {
	uint64_t live_regions; /* mmap'd page-heap regions */
	uint64_t live_bytes; /* bytes_mapped - bytes_unmapped */
	uint64_t bytes_per_region; /* live_bytes / live_regions, or 0 */
	uint64_t region_map_capacity; /* hard cap (4096 in v0) */
	uint64_t region_map_used_pct; /* live_regions / capacity * 100 */
	uint64_t large_live_count;
	uint64_t huge_live_count;
	uint64_t vma_count; /* /proc/self/maps lines, or 0 if unreadable */
	uint64_t slab_pages_in_use; /* slab pages on current+partials */
	uint64_t slab_slots_total; /* sum of capacity across those pages */
	uint64_t slab_slots_used; /* sum of used_count across those pages */
	uint64_t
	    slab_utilization_pct; /* slab_slots_used / slab_slots_total * 100 */
};

/*
 * Snapshot fragmentation metrics. Tolerates NULL. Pre-init
 * returns all zeroes.
 */
V8M_EXPORT void v8m_get_frag_metrics(struct v8m_frag_metrics *out);

/*
 * Process-wide VMA count, read live from /proc/self/maps. Useful
 * for surfacing the kernel-side fragmentation cost — every Huge
 * allocation that does NOT coalesce with an existing region grows
 * the line count, and a runaway count slows mmap / fork / page
 * fault paths in the kernel. Returns 0 if /proc/self/maps cannot
 * be opened (e.g. some seccomp sandboxes), which the caller can
 * treat as "unknown" rather than "zero".
 *
 * Cost: one open + sequential read + close, roughly O(VMA_count).
 * Cheap enough for periodic logging, too expensive for the alloc
 * fast path.
 */
V8M_EXPORT uint64_t v8m_count_vmas(void);

/*
 * Async-signal-safe emergency allocation
 * (architecture.md §6). `malloc` / `free` are not
 * async-signal-safe in general — the slab pool's `pthread_mutex`
 * would happily deadlock if a signal handler fires on a thread
 * that already holds it. This is the escape hatch: a small BSS
 * bump pool, atomic-only, no locks, no syscalls, safe to call
 * from any signal handler.
 *
 * Returns NULL on exhaustion (the buffer is intentionally tiny —
 * a one-page emergency budget — so signal handlers must check).
 * The returned pointer can be passed to `free()` like any other
 * v8malloc allocation; the dispatcher recognises it via a range
 * check and treats the free as a no-op (the buffer leaks for the
 * process lifetime by design).
 */
/* NOLINTNEXTLINE(readability-redundant-declaration) */
V8M_EXPORT void *v8m_signal_safe_alloc(size_t size);

/*
 * Force a purge cycle: page-heap regions that have drained
 * (e.g., empty buddy arenas, slab pages with `used_count == 0`)
 * are returned to the kernel. v0's slab and buddy pools already
 * release empty pages eagerly on free, so the synchronous purge
 * call is currently a no-op — the public surface lands to lock
 * the contract for the future bg purge thread cycle. Returns 0
 * on success.
 */
V8M_EXPORT int v8m_purge(void);

/*
 * Per-thread variant. Once the thread cache lands this releases
 * any cached objects bound to the calling thread back to the
 * pools. No-op in v0.
 */
V8M_EXPORT int v8m_purge_thread(void);

/* --- v8m_-namespaced glibc-compat extensions -------------------- */
/*
 * Mirrors of the standard glibc statistics / tuning extensions
 * that the library also exports under their unprefixed names.
 * Programs that link side-by-side with another allocator can call
 * these to talk to v8malloc explicitly when the standard symbols
 * have been resolved elsewhere. Forward to the same shared
 * `v8m_collect_live_stats` snapshot, so every reporter agrees.
 *
 * `struct mallinfo` and `struct mallinfo2` are declared in
 * `<malloc.h>`. Consumers needing the namespaced variants must
 * `#include <malloc.h>` ahead of this header (or `<stdio.h>` for
 * the FILE * argument of `v8m_malloc_info`).
 */
struct mallinfo;
struct mallinfo2;

V8M_EXPORT struct mallinfo v8m_mallinfo(void);
V8M_EXPORT struct mallinfo2 v8m_mallinfo2(void);
V8M_EXPORT void v8m_malloc_stats(void);
/* `stream` is `FILE *`; declared as `void *` so this header doesn't
 * need to drag in `<stdio.h>`. Callers cast their FILE pointer
 * implicitly through the void * conversion. */
V8M_EXPORT int v8m_malloc_info(int options, void *stream);
V8M_EXPORT int v8m_mallopt(int param, int value);
V8M_EXPORT int v8m_malloc_trim(size_t pad);

/* --- Failure-path hooks ----------------------------------------- */

/*
 * Out-of-memory handler signature. Invoked on the (rare) allocation
 * path that exhausts the page heap or trips the soft limit. The
 * handler receives the size that failed to satisfy and returns a
 * non-zero value to ask the allocator to retry the request once
 * (giving the program a chance to free other allocations first), or
 * zero to let the allocation fail with errno = ENOMEM.
 *
 * The handler runs on the calling thread with the dispatcher fully
 * initialized. It must be reentrancy-safe: if it allocates and the
 * sub-allocation also fails, the nested call returns NULL with
 * errno = ENOMEM rather than recursing back into the handler.
 */
typedef int (*v8m_oom_handler_t)(size_t requested_size);

/*
 * Install or clear the OOM handler. Pass NULL to clear (the default).
 * The previous handler is returned so callers can chain.
 */
V8M_EXPORT v8m_oom_handler_t v8m_set_oom_handler(v8m_oom_handler_t handler);

/*
 * Cap the bytes the page heap may have mapped at any one time.
 * Counted as `bytes_mapped - bytes_unmapped` (matches the
 * `live_bytes` field of struct v8m_stats). Allocations that would
 * push the live byte total above the limit fail with NULL /
 * errno = ENOMEM (after invoking the OOM handler if one is set,
 * to give the program a chance to free other allocations first).
 *
 * Pass 0 to disable the limit (the default). The limit applies to
 * the page heap only — bootstrap allocations are not counted.
 */
V8M_EXPORT void v8m_set_soft_limit(size_t bytes);

/*
 * Read the current soft limit. Returns 0 when no limit is set.
 */
V8M_EXPORT size_t v8m_get_soft_limit(void);

/* --- Pointer introspection -------------------------------------- */

/*
 * Backend that issued a given pointer. The ordering is stable
 * across v0; new backends are appended.
 */
enum v8m_ptr_backend {
	V8M_PTR_FOREIGN = 0, /* not v8malloc-issued (or NULL) */
	V8M_PTR_BOOTSTRAP, /* served from the pre-init bootstrap buffer */
	V8M_PTR_SLAB, /* slab pool — Tiny + Small classes */
	V8M_PTR_BUDDY, /* buddy pool — Medium classes */
	V8M_PTR_LARGE /* direct mmap path — Large + Huge */
};

/*
 * Snapshot of what v8malloc knows about a pointer. `usable_size`
 * matches what `malloc_usable_size(ptr)` would report; `size_class`
 * is the slab class id (0..31) for SLAB-backed pointers and -1
 * everywhere else.
 */
struct v8m_ptr_info {
	int backend; /* enum v8m_ptr_backend */
	size_t usable_size;
	int size_class; /* -1 if backend != V8M_PTR_SLAB */
};

/*
 * Quick predicate: true iff `ptr` was issued by v8malloc and is
 * recognized by the dispatcher. NULL is not a valid pointer.
 * Mid-allocation pointers (offsets inside a v8malloc region but
 * not the start of any individual allocation) are not recognized.
 */
V8M_EXPORT bool v8m_is_valid_ptr(const void *ptr);

/*
 * Fill `*out` with everything v8malloc knows about `ptr`. Returns
 * 0 on success, -1 with `errno = EINVAL` for NULL `out`, NULL
 * `ptr`, or a foreign / mid-allocation `ptr`. On failure `*out`
 * is left in a defined zero state with backend = V8M_PTR_FOREIGN.
 */
V8M_EXPORT int v8m_ptr_info(const void *ptr, struct v8m_ptr_info *out);

#ifdef __cplusplus
}
#endif

#endif /* V8MALLOC_V8MALLOC_H */
